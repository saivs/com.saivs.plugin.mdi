#pragma once

#include "Unity/IUnityInterface.h"
#include "Unity/IUnityGraphics.h"

#define UNITY_VULKAN_HEADER "vulkan/vulkan.h"
#include "Unity/IUnityGraphicsVulkan.h"

// Vulkan interception for the MDI backend. Two jobs:
//
// 1. Shadow-track, per VkCommandBuffer, the graphics pipeline and index
//    buffer bound since that buffer's vkBeginCommandBuffer. The MDI draw
//    records a raw vkCmdDrawIndexedIndirect that INHERITS the state the
//    C#-side prime draw left on Unity's command buffer. Two failure modes
//    break that assumption (both observed in the editor):
//      - the editor skips prime draws whose shader variant is still
//        async-compiling → nothing bound → undefined behavior (frame-wide
//        corruption, then a frame-pacing collapse: white screen, freezes);
//      - RenderGraph + Native Render Pass can defer the plugin event out of
//        the recorded raster pass → the callback fires inside a LATER pass
//        with a foreign pipeline / index buffer bound → valid but garbage
//        draws. ExecuteMDI uses the tracker to skip both cases.
//
// 2. Observe vkCreateDevice: which extensions and features Unity actually
//    enabled. vkGetDeviceProcAddr returning non-null is NOT proof a call is
//    legal (NVIDIA returns pointers for unenabled extensions), and Unity
//    creates the device as Vulkan 1.1 without VK_KHR_draw_indirect_count —
//    the observation lets the backend gate the count path on facts.
//
// Registration must happen during UnityPluginLoad, before the graphics
// device initializes; a late registration is reported by
// MDIVulkanStateTrackingActive() returning false and everything degrades to
// the old heuristics.

// Registers the Vulkan init interception (V2 AddInterceptInitialization when
// available, V1 InterceptInitialization otherwise). Safe to call when the
// Vulkan interface is absent — returns false.
bool MDIVulkanInterceptRegister(IUnityInterfaces* unityInterfaces);

// True when the hooks are live (at least one command buffer recorded
// through them). False = plugin loaded too late; no draw is blocked.
bool MDIVulkanStateTrackingActive();

// Last graphics pipeline / index buffer bound on `cb` since its
// vkBeginCommandBuffer (VK_NULL_HANDLE when none). Returns false when the
// command buffer was never seen by the hooks.
bool MDIVulkanStateQuery(VkCommandBuffer cb, VkPipeline* outPipeline, VkBuffer* outIndexBuffer);

// What Unity's vkCreateDevice actually enabled (valid when observed=true).
struct MDIVulkanDeviceInfo
{
    bool observed = false;                 // the create call went through our hook
    bool multiDrawIndirect = false;        // core feature enabled
    bool drawIndirectFirstInstance = false;// core feature enabled
    bool drawIndirectCountExt = false;     // VK_KHR/AMD_draw_indirect_count in enabled extensions
    bool drawIndirectCountCore = false;    // VkPhysicalDeviceVulkan12Features::drawIndirectCount enabled
};

const MDIVulkanDeviceInfo& MDIVulkanObservedDevice();
