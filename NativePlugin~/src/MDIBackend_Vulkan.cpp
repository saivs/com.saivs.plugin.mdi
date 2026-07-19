#include "MDIBackend_Vulkan.h"
#include "MDILog.h"

// -----------------------------------------------------------------------
// Initialize / Shutdown
// -----------------------------------------------------------------------

bool MDIBackend_Vulkan::Initialize(IUnityInterfaces* unityInterfaces)
{
    // Try V2 first, fall back to V1 (same vtable layout for methods we use)
    _vulkan = unityInterfaces->Get<IUnityGraphicsVulkanV2>();
    if (!_vulkan)
    {
        auto* v1 = unityInterfaces->Get<IUnityGraphicsVulkan>();
        if (!v1)
        {
            DebugLog("[MDI] Vulkan interface not available\n");
            return false;
        }
        // V2 extends V1 — same vtable layout for all V1 methods.
        // Safe to cast since we only use V1 methods.
        _vulkan = reinterpret_cast<IUnityGraphicsVulkanV2*>(v1);
    }

    // Resolve vkCmdDrawIndexedIndirect via Vulkan loader
    UnityVulkanInstance instance = _vulkan->Instance();
    if (!instance.getInstanceProcAddr || !instance.device)
    {
        DebugLog("[MDI] Vulkan instance not available\n");
        return false;
    }

    auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        instance.getInstanceProcAddr(instance.instance, "vkGetDeviceProcAddr"));
    if (!getDeviceProcAddr)
    {
        DebugLog("[MDI] vkGetDeviceProcAddr not found\n");
        return false;
    }

    _vkCmdDrawIndexedIndirect = reinterpret_cast<PFN_vkCmdDrawIndexedIndirect>(
        getDeviceProcAddr(instance.device, "vkCmdDrawIndexedIndirect"));
    if (!_vkCmdDrawIndexedIndirect)
    {
        DebugLog("[MDI] vkCmdDrawIndexedIndirect not found\n");
        return false;
    }

    // Optional: GPU-driven draw count (core 1.2, or KHR/AMD extension).
    _vkCmdDrawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(
        getDeviceProcAddr(instance.device, "vkCmdDrawIndexedIndirectCount"));
    if (!_vkCmdDrawIndexedIndirectCount)
        _vkCmdDrawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(
            getDeviceProcAddr(instance.device, "vkCmdDrawIndexedIndirectCountKHR"));
    if (!_vkCmdDrawIndexedIndirectCount)
        _vkCmdDrawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(
            getDeviceProcAddr(instance.device, "vkCmdDrawIndexedIndirectCountAMD"));
    DebugLog("[MDI] Vulkan drawIndexedIndirectCount: %s\n",
        _vkCmdDrawIndexedIndirectCount ? "supported" : "NOT supported (GPU count will use maxDrawCount)");

    // Query multiDrawIndirect support from the physical device.
    // vkCmdDrawIndexedIndirect with drawCount > 1 requires this feature.
    // Without it, drawCount must be 0 or 1 (Vulkan spec).
    auto vkGetPhysicalDeviceFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
        instance.getInstanceProcAddr(instance.instance, "vkGetPhysicalDeviceFeatures"));
    if (vkGetPhysicalDeviceFeatures && instance.physicalDevice)
    {
        VkPhysicalDeviceFeatures features = {};
        vkGetPhysicalDeviceFeatures(instance.physicalDevice, &features);
        _multiDrawIndirectSupported = features.multiDrawIndirect == VK_TRUE;
        DebugLog("[MDI] Vulkan multiDrawIndirect: %s\n",
            _multiDrawIndirectSupported ? "supported" : "NOT supported (will use loop fallback)");
    }
    else
    {
        _multiDrawIndirectSupported = false;
        DebugLog("[MDI] Could not query multiDrawIndirect, assuming not supported\n");
    }

    _initialized = true;
    DebugLog("[MDI] Vulkan backend initialized\n");
    return true;
}

void MDIBackend_Vulkan::ConfigureEvents(IUnityInterfaces* unityInterfaces, int baseEventID, int count)
{
    // Try V2 first, fall back to V1
    IUnityGraphicsVulkanV2* vulkan = unityInterfaces->Get<IUnityGraphicsVulkanV2>();
    if (!vulkan)
    {
        auto* v1 = unityInterfaces->Get<IUnityGraphicsVulkan>();
        if (!v1) return;
        vulkan = reinterpret_cast<IUnityGraphicsVulkanV2*>(v1);
    }

    for (int i = 0; i < count; ++i)
    {
        UnityVulkanPluginEventConfig config = {};
        // DontCare: we expect the prime draw to have started a render pass.
        // EnsureInside would restart it and lose pipeline/descriptor bindings.
        config.renderPassPrecondition = kUnityVulkanRenderPass_DontCare;
        config.graphicsQueueAccess = kUnityVulkanGraphicsQueueAccess_DontCare;
        // EnsurePreviousFrameSubmission (bit 0) — keep set for proper frame ordering.
        // ModifiesCommandBuffersState (bit 3) — NOT set: vkCmdDrawIndexedIndirect
        // doesn't modify pipeline/descriptor/vertex buffer bindings.
        config.flags = kUnityVulkanEventConfigFlag_EnsurePreviousFrameSubmission;
        vulkan->ConfigureEvent(baseEventID + i, &config);
    }

    DebugLog("[MDI] Configured Vulkan events [%d .. %d)\n", baseEventID, baseEventID + count);
}

void MDIBackend_Vulkan::Shutdown()
{
    _vulkan = nullptr;
    _vkCmdDrawIndexedIndirect = nullptr;
    _vkCmdDrawIndexedIndirectCount = nullptr;
    _initialized = false;
    _multiDrawIndirectSupported = false;
    DebugLog("[MDI] Vulkan backend shutdown\n");
}

// -----------------------------------------------------------------------
// ExecuteMDI
// -----------------------------------------------------------------------

void MDIBackend_Vulkan::ExecuteMDI(const MDIParams& params)
{
    if (!_initialized || !params.argsBuffer || params.maxDrawCount == 0)
        return;

    // On Vulkan, GetNativeBufferPtr() returns an opaque Unity handle, NOT a VkBuffer.
    // Must use AccessBuffer() to resolve it to an actual VkBuffer and insert pipeline barriers.
    UnityVulkanBuffer vkArgsBuffer = {};
    if (!_vulkan->AccessBuffer(
        params.argsBuffer,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
        kUnityVulkanResourceAccess_PipelineBarrier,
        &vkArgsBuffer))
    {
        DebugLog("[MDI] Vulkan: AccessBuffer failed for args buffer\n");
        return;
    }

    // Resolve the GPU count buffer the same way, before querying recording state.
    const bool useGpuCount = (params.flags & MDI_FLAG_GPU_COUNT) != 0 &&
                             params.countBuffer != nullptr &&
                             _vkCmdDrawIndexedIndirectCount != nullptr;
    UnityVulkanBuffer vkCountBuffer = {};
    bool countResolved = false;
    if (useGpuCount)
    {
        countResolved = _vulkan->AccessBuffer(
            params.countBuffer,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
            kUnityVulkanResourceAccess_PipelineBarrier,
            &vkCountBuffer);
        if (!countResolved)
            DebugLog("[MDI] Vulkan: AccessBuffer failed for count buffer, using maxDrawCount\n");
    }

    // AccessBuffer invalidates the recording state — must re-query
    UnityVulkanRecordingState state = {};
    if (!_vulkan->CommandRecordingState(&state, kUnityVulkanGraphicsQueueAccess_DontCare))
        return;

    if (!state.commandBuffer) return;

    // Must be inside a render pass for draw commands.
    // The prime DrawProceduralIndirect should have started one,
    // and AccessBuffer with buffer-only barriers doesn't exit it.
    if (state.subPassIndex < 0)
    {
        DebugLog("[MDI] Vulkan: not inside render pass after AccessBuffer, skipping\n");
        return;
    }

    const uint32_t stride = 20; // 5 * sizeof(uint32_t) = IndirectDrawIndexedArgs

    if (useGpuCount && countResolved)
    {
        // GPU-driven draw count: hardware reads the actual count from the
        // count buffer, clamped to maxDrawCount.
        _vkCmdDrawIndexedIndirectCount(
            state.commandBuffer,
            vkArgsBuffer.buffer,
            static_cast<VkDeviceSize>(params.argsOffsetBytes),
            vkCountBuffer.buffer,
            static_cast<VkDeviceSize>(params.countOffsetBytes),
            params.maxDrawCount,
            stride
        );
    }
    else if (_multiDrawIndirectSupported)
    {
        // Hardware multi-draw: single call with drawCount > 1
        _vkCmdDrawIndexedIndirect(
            state.commandBuffer,
            vkArgsBuffer.buffer,
            static_cast<VkDeviceSize>(params.argsOffsetBytes),
            params.maxDrawCount,
            stride
        );
    }
    else
    {
        // Fallback: loop with drawCount=1 per call (always valid per Vulkan spec)
        VkDeviceSize offset = static_cast<VkDeviceSize>(params.argsOffsetBytes);
        for (uint32_t i = 0; i < params.maxDrawCount; ++i)
        {
            _vkCmdDrawIndexedIndirect(
                state.commandBuffer,
                vkArgsBuffer.buffer,
                offset,
                1,
                stride
            );
            offset += stride;
        }
    }
}

bool MDIBackend_Vulkan::IsSupported() const
{
    return _initialized && _vkCmdDrawIndexedIndirect != nullptr;
}
