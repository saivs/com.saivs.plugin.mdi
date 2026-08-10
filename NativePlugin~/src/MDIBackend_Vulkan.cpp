#include "MDIBackend_Vulkan.h"
#include "MDIVulkanStateTracker.h"
#include "MDILog.h"

#include <atomic>

// -----------------------------------------------------------------------
// Initialize / Shutdown
// -----------------------------------------------------------------------

// Whether the physical device reports the drawIndirectCount feature
// (VkPhysicalDeviceVulkan12Features). Fallback gate for the core-1.2
// vkCmdDrawIndexedIndirectCount entry point when vkCreateDevice was NOT
// observed: vkGetDeviceProcAddr returns it non-null on ANY 1.2 device, even
// when the feature was not enabled at device creation, so a non-null pointer
// alone doesn't make the call legal.
static bool QueryDrawIndirectCountFeature(const UnityVulkanInstance& instance)
{
    if (!instance.physicalDevice)
        return false;

    auto getPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        instance.getInstanceProcAddr(instance.instance, "vkGetPhysicalDeviceFeatures2"));
    if (!getPhysicalDeviceFeatures2)
        getPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            instance.getInstanceProcAddr(instance.instance, "vkGetPhysicalDeviceFeatures2KHR"));
    if (!getPhysicalDeviceFeatures2)
        return false;

    // Zero-init: a pre-1.2 driver ignores the unknown sType in the pNext chain
    // and drawIndirectCount stays VK_FALSE.
    VkPhysicalDeviceVulkan12Features features12 = {};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features12;

    getPhysicalDeviceFeatures2(instance.physicalDevice, &features2);
    return features12.drawIndirectCount == VK_TRUE;
}

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

    const MDIVulkanDeviceInfo& observed = MDIVulkanObservedDevice();

    // Optional: GPU-driven draw count (KHR/AMD extension, or core 1.2).
    //
    // When vkCreateDevice was observed we KNOW what Unity enabled and gate on
    // facts. Without observation (plugin loaded after device init) we fall
    // back to the old heuristics — with the caveat that some drivers (NVIDIA)
    // return non-null vkGetDeviceProcAddr even for unenabled extensions, so
    // the heuristic can be wrong; the observation path exists precisely to
    // close that hole.
    _vkCmdDrawIndexedIndirectCount = nullptr;
    if (observed.observed)
    {
        if (observed.drawIndirectCountExt)
        {
            _vkCmdDrawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(
                getDeviceProcAddr(instance.device, "vkCmdDrawIndexedIndirectCountKHR"));
            if (!_vkCmdDrawIndexedIndirectCount)
                _vkCmdDrawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(
                    getDeviceProcAddr(instance.device, "vkCmdDrawIndexedIndirectCountAMD"));
        }
        else if (observed.drawIndirectCountCore)
        {
            _vkCmdDrawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(
                getDeviceProcAddr(instance.device, "vkCmdDrawIndexedIndirectCount"));
        }
    }
    else
    {
        _vkCmdDrawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(
            getDeviceProcAddr(instance.device, "vkCmdDrawIndexedIndirectCountKHR"));
        if (!_vkCmdDrawIndexedIndirectCount)
            _vkCmdDrawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(
                getDeviceProcAddr(instance.device, "vkCmdDrawIndexedIndirectCountAMD"));
        if (!_vkCmdDrawIndexedIndirectCount)
        {
            auto coreProc = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(
                getDeviceProcAddr(instance.device, "vkCmdDrawIndexedIndirectCount"));
            if (coreProc && QueryDrawIndirectCountFeature(instance))
                _vkCmdDrawIndexedIndirectCount = coreProc;
        }
    }
    DebugLog("[MDI] Vulkan drawIndexedIndirectCount: %s (%s)\n",
        _vkCmdDrawIndexedIndirectCount ? "supported" : "NOT supported (GPU count will use maxDrawCount)",
        observed.observed ? "from observed device creation" : "heuristic — device creation not observed");

    // multiDrawIndirect: vkCmdDrawIndexedIndirect with drawCount > 1 requires
    // the feature ENABLED at device creation. Observed truth when available,
    // physical-device query otherwise (matches what Unity enables on every
    // driver validated so far).
    if (observed.observed)
    {
        _multiDrawIndirectSupported = observed.multiDrawIndirect;
    }
    else
    {
        auto vkGetPhysicalDeviceFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
            instance.getInstanceProcAddr(instance.instance, "vkGetPhysicalDeviceFeatures"));
        if (vkGetPhysicalDeviceFeatures && instance.physicalDevice)
        {
            VkPhysicalDeviceFeatures features = {};
            vkGetPhysicalDeviceFeatures(instance.physicalDevice, &features);
            _multiDrawIndirectSupported = features.multiDrawIndirect == VK_TRUE;
        }
        else
        {
            _multiDrawIndirectSupported = false;
        }
    }
    DebugLog("[MDI] Vulkan multiDrawIndirect: %s (%s)\n",
        _multiDrawIndirectSupported ? "supported" : "NOT supported (will use loop fallback)",
        observed.observed ? "from observed device creation" : "physical-device query");

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
        // ModifiesCommandBuffersState NOT set — deliberately, twice over:
        //  - the callback records nothing but the draw itself (buffer
        //    resolves are ObserveOnly), which inherits state and changes no
        //    bindings;
        //  - with the flag SET, Unity re-applies its cached state after
        //    every event, and on Android that re-application loses the
        //    surface pre-rotation correction — the frame renders squished
        //    into half the screen and shifted.
        config.flags = kUnityVulkanEventConfigFlag_EnsurePreviousFrameSubmission;
        vulkan->ConfigureEvent(baseEventID + i, &config);
    }

    // The prepare event sits one past the draw events and is the opposite
    // case: it exists precisely to record a buffer barrier, which is illegal
    // inside a render pass, so it must run outside one. Callers issue it
    // right after the compute dispatch (or upload) that wrote the args.
    {
        UnityVulkanPluginEventConfig config = {};
        config.renderPassPrecondition = kUnityVulkanRenderPass_EnsureOutside;
        config.graphicsQueueAccess = kUnityVulkanGraphicsQueueAccess_DontCare;
        config.flags = kUnityVulkanEventConfigFlag_EnsurePreviousFrameSubmission;
        vulkan->ConfigureEvent(baseEventID + count, &config);
    }

    DebugLog("[MDI] Configured Vulkan events [%d .. %d), prepare event %d\n",
        baseEventID, baseEventID + count, baseEventID + count);
}

void MDIBackend_Vulkan::Shutdown()
{
    _vulkan = nullptr;
    _vkCmdDrawIndexedIndirect = nullptr;
    _vkCmdDrawIndexedIndirectCount = nullptr;
    _initialized = false;
    _multiDrawIndirectSupported = false;
}

// -----------------------------------------------------------------------
// PrepareIndirectArgs — the ONLY place that records a barrier
// -----------------------------------------------------------------------

void MDIBackend_Vulkan::PrepareIndirectArgs(void* argsBuffer)
{
    if (!_initialized || !argsBuffer)
        return;

    // Issued from outside a render pass (the event is configured
    // EnsureOutside), so Unity is free to record the buffer barrier here.
    // Whatever wrote the args — a compute dispatch, a CPU upload — becomes
    // visible to indirect fetch now, and ExecuteMDI's ObserveOnly resolve
    // finds nothing left to do and leaves the render pass alone.
    UnityVulkanBuffer resolved = {};
    if (!_vulkan->AccessBuffer(
        argsBuffer,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
        kUnityVulkanResourceAccess_PipelineBarrier,
        &resolved))
    {
        DebugLog("[MDI] Vulkan: AccessBuffer failed while preparing indirect args\n");
    }
}

// -----------------------------------------------------------------------
// ExecuteMDI
// -----------------------------------------------------------------------

void MDIBackend_Vulkan::ExecuteMDI(const MDIParams& params)
{
    if (!_initialized || !params.argsBuffer || params.maxDrawCount == 0)
        return;

    // On Vulkan, GetNativeBufferPtr() returns an opaque Unity handle, NOT a
    // VkBuffer — AccessBuffer resolves it. ObserveOnly, NOT PipelineBarrier:
    // the write→indirect-read barrier is the prepare event's job
    // (PrepareIndirectArgs), which runs outside a render pass where the
    // barrier is legal. Requesting a barrier here — inside the pass — forces
    // Unity to end/split the render pass to record it, which desyncs cached
    // command buffers (observed in the editor as flickering black quads over
    // UI) and costs a store/load of every attachment per split.
    UnityVulkanBuffer vkArgsBuffer = {};
    if (!_vulkan->AccessBuffer(
        params.argsBuffer,
        0, 0,
        kUnityVulkanResourceAccess_ObserveOnly,
        &vkArgsBuffer))
    {
        DebugLog("[MDI] Vulkan: AccessBuffer failed for args buffer\n");
        return;
    }

    // Resolve the GPU count buffer the same way (ObserveOnly — callers must
    // PrepareIndirectArgs the count buffer too when a compute writes it).
    const bool useGpuCount = (params.flags & MDI_FLAG_GPU_COUNT) != 0 &&
                             params.countBuffer != nullptr &&
                             _vkCmdDrawIndexedIndirectCount != nullptr;
    UnityVulkanBuffer vkCountBuffer = {};
    bool countResolved = false;
    if (useGpuCount)
    {
        countResolved = _vulkan->AccessBuffer(
            params.countBuffer,
            0, 0,
            kUnityVulkanResourceAccess_ObserveOnly,
            &vkCountBuffer);
        if (!countResolved)
            DebugLog("[MDI] Vulkan: AccessBuffer failed for count buffer, using maxDrawCount\n");
    }

    // Resolve the index buffer the batch was recorded against — only needed
    // to verify the inherited state below actually belongs to our prime draw.
    VkBuffer expectedIndexBuffer = VK_NULL_HANDLE;
    if (params.indexBuffer)
    {
        UnityVulkanBuffer vkIndexBuffer = {};
        if (_vulkan->AccessBuffer(params.indexBuffer, 0, 0,
                kUnityVulkanResourceAccess_ObserveOnly, &vkIndexBuffer))
            expectedIndexBuffer = vkIndexBuffer.buffer;
    }

    // AccessBuffer invalidates the recording state — must re-query
    UnityVulkanRecordingState state = {};
    if (!_vulkan->CommandRecordingState(&state, kUnityVulkanGraphicsQueueAccess_DontCare))
        return;

    if (!state.commandBuffer) return;

    // Must be inside a render pass for draw commands.
    if (state.subPassIndex < 0)
    {
        DebugLog("[MDI] Vulkan: not inside render pass, skipping\n");
        return;
    }

    // The raw draw below INHERITS the pipeline and index buffer the prime
    // draw left on this command buffer, so both must (a) exist and (b) be
    // OURS. Two observed failure modes say they may not be:
    //   - the editor skips prime draws whose shader variant is still
    //     async-compiling → nothing bound → undefined behavior;
    //   - RenderGraph + Native Render Pass can defer the plugin event out of
    //     the recorded raster pass → the callback fires inside a LATER pass
    //     with a FOREIGN pipeline and index buffer bound → valid but garbage
    //     draws. The index-buffer identity check catches exactly that.
    if (MDIVulkanStateTrackingActive())
    {
        VkPipeline boundPipeline = VK_NULL_HANDLE;
        VkBuffer boundIndexBuffer = VK_NULL_HANDLE;
        MDIVulkanStateQuery(state.commandBuffer, &boundPipeline, &boundIndexBuffer);

        const bool noState = (boundPipeline == VK_NULL_HANDLE || boundIndexBuffer == VK_NULL_HANDLE);
        const bool foreignIndex = !noState && expectedIndexBuffer != VK_NULL_HANDLE &&
                                  boundIndexBuffer != expectedIndexBuffer;
        if (noState || foreignIndex)
        {
            static std::atomic<uint32_t> s_skipCount{0};
            const uint32_t skips = s_skipCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skips == 1 || (skips & 0x3F) == 0)
                DebugLog("[MDI] Vulkan: skipping batch — %s (pipeline=%p, boundIB=%p, expectedIB=%p; %u skipped so far)\n",
                         noState ? "prime draw state missing"
                                 : "foreign index buffer bound (event deferred into another pass?)",
                         (void*)boundPipeline, (void*)boundIndexBuffer, (void*)expectedIndexBuffer,
                         skips);
            return;
        }
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
