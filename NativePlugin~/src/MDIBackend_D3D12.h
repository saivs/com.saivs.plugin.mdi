#pragma once

#include "MDIBackend.h"

#ifdef _WIN32
#include <d3d12.h>
#include <dxgi.h>
#include "Unity/IUnityGraphicsD3D12.h"

class MDIBackend_D3D12 final : public IMDIBackend
{
public:
    bool Initialize(IUnityInterfaces* unityInterfaces) override;
    void Shutdown() override;
    void ExecuteMDI(const MDIParams& params) override;
    bool IsSupported() const override;
    bool ResizeInstanceIDBuffer(uint32_t newMaxCount) override;
    uint32_t GetMaxInstanceCount() const override { return _maxInstanceCount; }

    void ConfigureEvents(IUnityInterfaces* unityInterfaces, int baseEventID, int count);

private:
    void InstallDeviceHook();
    void CreateInstanceIDBuffer();

    IUnityGraphicsD3D12v7* _d3d12       = nullptr;
    // Optional (newer Unity): RequestResourceState/NotifyResourceState let us
    // cooperate with Unity's resource-state tracker instead of guessing states.
    IUnityGraphicsD3D12v8* _d3d12v8     = nullptr;
    ID3D12Device*          _device       = nullptr;
    ID3D12CommandSignature* _cmdSignature = nullptr;  // basic: DrawIndexed only
    bool                   _initialized  = false;

    // Per-instance identity buffer [0, 1, 2, ..., _maxInstanceCount-1]
    // Bound to VB slot kInstanceVBSlot before ExecuteIndirect.
    // Shader reads draw index via StartInstanceLocation offset.
    ID3D12Resource* _instanceIDBuffer = nullptr;

    uint32_t _maxInstanceCount = kDefaultMaxInstanceCount;
    static constexpr uint32_t kDefaultMaxInstanceCount = 65536;
    static constexpr uint32_t kInstanceVBSlot = 15;
};

#endif // _WIN32
