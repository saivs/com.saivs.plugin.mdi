#include "MDIVulkanStateTracker.h"
#include "MDILog.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>

// -----------------------------------------------------------------------
// Per-command-buffer state tracking
// -----------------------------------------------------------------------

namespace
{
    struct CmdBufState
    {
        VkPipeline pipeline    = VK_NULL_HANDLE; // last graphics pipeline
        VkBuffer   indexBuffer = VK_NULL_HANDLE; // last bound index buffer
    };

    // Binds are hot-path (thousands per frame, recorded from multiple threads
    // with graphics jobs) — shard the map so concurrent recorders rarely
    // contend on the same mutex.
    struct Shard
    {
        std::mutex mutex;
        std::unordered_map<VkCommandBuffer, CmdBufState> state;
    };

    constexpr size_t kShardCount = 16; // power of two
    Shard s_shards[kShardCount];

    std::atomic<bool> s_seenBegin{false};

    Shard& ShardFor(VkCommandBuffer cb)
    {
        size_t h = std::hash<const void*>{}(reinterpret_cast<const void*>(cb));
        return s_shards[(h >> 4) & (kShardCount - 1)];
    }

    MDIVulkanDeviceInfo s_deviceInfo;

    PFN_vkGetInstanceProcAddr s_loaderGipa = nullptr;
    PFN_vkGetDeviceProcAddr   s_loaderGdpa = nullptr;

    PFN_vkCreateDevice        s_origCreateDevice       = nullptr;
    PFN_vkBeginCommandBuffer  s_origBeginCommandBuffer = nullptr;
    PFN_vkCmdBindPipeline     s_origCmdBindPipeline    = nullptr;
    PFN_vkCmdBindIndexBuffer  s_origCmdBindIndexBuffer = nullptr;
    PFN_vkFreeCommandBuffers  s_origFreeCommandBuffers = nullptr;

    VKAPI_ATTR VkResult VKAPI_CALL Hook_vkBeginCommandBuffer(
        VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo)
    {
        s_seenBegin.store(true, std::memory_order_relaxed);
        {
            Shard& shard = ShardFor(commandBuffer);
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.state[commandBuffer] = CmdBufState{};
        }
        return s_origBeginCommandBuffer(commandBuffer, pBeginInfo);
    }

    VKAPI_ATTR void VKAPI_CALL Hook_vkCmdBindPipeline(
        VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline)
    {
        if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
        {
            Shard& shard = ShardFor(commandBuffer);
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.state[commandBuffer].pipeline = pipeline;
        }
        s_origCmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
    }

    VKAPI_ATTR void VKAPI_CALL Hook_vkCmdBindIndexBuffer(
        VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType)
    {
        {
            Shard& shard = ShardFor(commandBuffer);
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.state[commandBuffer].indexBuffer = buffer;
        }
        s_origCmdBindIndexBuffer(commandBuffer, buffer, offset, indexType);
    }

    VKAPI_ATTR void VKAPI_CALL Hook_vkFreeCommandBuffers(
        VkDevice device, VkCommandPool commandPool,
        uint32_t commandBufferCount, const VkCommandBuffer* pCommandBuffers)
    {
        for (uint32_t i = 0; i < commandBufferCount; ++i)
        {
            VkCommandBuffer cb = pCommandBuffers[i];
            Shard& shard = ShardFor(cb);
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.state.erase(cb);
        }
        s_origFreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
    }

    // -------------------------------------------------------------------
    // vkCreateDevice observation — records what Unity enabled, forwards the
    // call untouched. Never patches the create info: this plugin has no
    // retry-unpatched safety net, so observation only.
    // -------------------------------------------------------------------

    VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateDevice(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice)
    {
        s_deviceInfo.observed = true;

        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
        {
            const char* name = pCreateInfo->ppEnabledExtensionNames[i];
            if (strcmp(name, "VK_KHR_draw_indirect_count") == 0 ||
                strcmp(name, "VK_AMD_draw_indirect_count") == 0)
                s_deviceInfo.drawIndirectCountExt = true;
        }

        // Core features arrive either through pEnabledFeatures or through a
        // VkPhysicalDeviceFeatures2 in the pNext chain (never both).
        const VkPhysicalDeviceFeatures* features = pCreateInfo->pEnabledFeatures;
        for (const VkBaseInStructure* node = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             node != nullptr;
             node = node->pNext)
        {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 && !features)
                features = &reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->features;
            else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
                s_deviceInfo.drawIndirectCountCore =
                    reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node)->drawIndirectCount == VK_TRUE;
        }
        if (features)
        {
            s_deviceInfo.multiDrawIndirect = features->multiDrawIndirect == VK_TRUE;
            s_deviceInfo.drawIndirectFirstInstance = features->drawIndirectFirstInstance == VK_TRUE;
        }

        DebugLog("[MDI] vkCreateDevice observed: multiDrawIndirect=%d firstInstance=%d countExt=%d countCore=%d\n",
                 s_deviceInfo.multiDrawIndirect ? 1 : 0,
                 s_deviceInfo.drawIndirectFirstInstance ? 1 : 0,
                 s_deviceInfo.drawIndirectCountExt ? 1 : 0,
                 s_deviceInfo.drawIndirectCountCore ? 1 : 0);

        return s_origCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }

    // -------------------------------------------------------------------
    // Loader interception — Unity resolves every Vulkan function through the
    // wrapped vkGetInstanceProcAddr / vkGetDeviceProcAddr once registered.
    // -------------------------------------------------------------------

    PFN_vkVoidFunction RouteFunction(const char* name, PFN_vkVoidFunction orig)
    {
        // Unity may resolve the same name more than once (instance
        // trampoline, then device-direct) — always keep the latest original.
        if (strcmp(name, "vkBeginCommandBuffer") == 0)
        {
            s_origBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(orig);
            return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkBeginCommandBuffer);
        }
        if (strcmp(name, "vkCmdBindPipeline") == 0)
        {
            s_origCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(orig);
            return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdBindPipeline);
        }
        if (strcmp(name, "vkCmdBindIndexBuffer") == 0)
        {
            s_origCmdBindIndexBuffer = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(orig);
            return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCmdBindIndexBuffer);
        }
        if (strcmp(name, "vkFreeCommandBuffers") == 0)
        {
            s_origFreeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(orig);
            return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkFreeCommandBuffers);
        }
        if (strcmp(name, "vkCreateDevice") == 0)
        {
            s_origCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(orig);
            return reinterpret_cast<PFN_vkVoidFunction>(&Hook_vkCreateDevice);
        }
        return orig;
    }

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Hook_GetDeviceProcAddr(VkDevice device, const char* pName)
    {
        PFN_vkVoidFunction orig = s_loaderGdpa ? s_loaderGdpa(device, pName) : nullptr;
        if (!orig || !pName)
            return orig;
        return RouteFunction(pName, orig);
    }

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Hook_GetInstanceProcAddr(VkInstance instance, const char* pName)
    {
        PFN_vkVoidFunction orig = s_loaderGipa(instance, pName);
        if (!orig || !pName)
            return orig;

        if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        {
            s_loaderGdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(orig);
            return reinterpret_cast<PFN_vkVoidFunction>(&Hook_GetDeviceProcAddr);
        }

        // Device-level functions can also come back from the loader's gipa as
        // instance-dispatch trampolines — route those too (vkCreateDevice is
        // an instance-level function and always arrives here).
        return RouteFunction(pName, orig);
    }

    PFN_vkGetInstanceProcAddr UNITY_INTERFACE_API InitCallback(PFN_vkGetInstanceProcAddr getInstanceProcAddr, void* /*userdata*/)
    {
        s_loaderGipa = getInstanceProcAddr;
        DebugLog("[MDI] Vulkan init interception active\n");
        return &Hook_GetInstanceProcAddr;
    }
} // namespace

bool MDIVulkanInterceptRegister(IUnityInterfaces* unityInterfaces)
{
    // V2 preferred: AddInterceptInitialization composes with other plugins'
    // callbacks (e.g. GfxPluginUGE) instead of claiming the single
    // max-priority slot V1 uses.
    if (auto* v2 = unityInterfaces->Get<IUnityGraphicsVulkanV2>())
    {
        if (v2->AddInterceptInitialization(InitCallback, nullptr, 0))
        {
            DebugLog("[MDI] Vulkan interception registered (V2)\n");
            return true;
        }
        DebugLog("[MDI] AddInterceptInitialization failed — device already initialized?\n");
        return false;
    }

    if (auto* v1 = unityInterfaces->Get<IUnityGraphicsVulkan>())
    {
        if (v1->InterceptInitialization(InitCallback, nullptr))
        {
            DebugLog("[MDI] Vulkan interception registered (V1)\n");
            return true;
        }
        DebugLog("[MDI] InterceptInitialization failed — device already initialized?\n");
        return false;
    }

    return false;
}

bool MDIVulkanStateTrackingActive()
{
    return s_seenBegin.load(std::memory_order_relaxed);
}

bool MDIVulkanStateQuery(VkCommandBuffer cb, VkPipeline* outPipeline, VkBuffer* outIndexBuffer)
{
    Shard& shard = ShardFor(cb);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.state.find(cb);
    if (it == shard.state.end())
    {
        if (outPipeline)    *outPipeline = VK_NULL_HANDLE;
        if (outIndexBuffer) *outIndexBuffer = VK_NULL_HANDLE;
        return false;
    }
    if (outPipeline)    *outPipeline = it->second.pipeline;
    if (outIndexBuffer) *outIndexBuffer = it->second.indexBuffer;
    return true;
}

const MDIVulkanDeviceInfo& MDIVulkanObservedDevice()
{
    return s_deviceInfo;
}
