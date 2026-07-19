#pragma once

#include "MDIBackend.h"
#include "Unity/IUnityGraphics.h"

// Override Unity's Vulkan header include to use our minimal header
#define UNITY_VULKAN_HEADER "vulkan/vulkan.h"
#include "Unity/IUnityGraphicsVulkan.h"

class MDIBackend_Vulkan final : public IMDIBackend
{
public:
    bool Initialize(IUnityInterfaces* unityInterfaces) override;
    void Shutdown() override;
    void ExecuteMDI(const MDIParams& params) override;
    bool IsSupported() const override;

    void ConfigureEvents(IUnityInterfaces* unityInterfaces, int baseEventID, int count);

private:
    IUnityGraphicsVulkanV2* _vulkan = nullptr;
    PFN_vkCmdDrawIndexedIndirect _vkCmdDrawIndexedIndirect = nullptr;
    PFN_vkCmdDrawIndexedIndirectCount _vkCmdDrawIndexedIndirectCount = nullptr;
    bool _initialized = false;
    bool _multiDrawIndirectSupported = false;
};
