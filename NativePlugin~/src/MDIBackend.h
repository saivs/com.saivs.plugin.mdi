#pragma once

#include <cstdint>
#include "Unity/IUnityInterface.h"

// flags bitfield for MDIParams
static constexpr uint32_t MDI_FLAG_MESH_PATH = 1u << 0; // user-supplied mesh (vs. procedural / indexed)
static constexpr uint32_t MDI_FLAG_GPU_COUNT = 1u << 1; // draw count comes from countBuffer (GPU-written)

// Shared parameter struct for MDI calls
struct MDIParams
{
    void*    argsBuffer;          // Native GPU buffer pointer (ID3D12Resource*, VkBuffer, etc.)
    void*    indexBuffer;         // Native index buffer pointer (for D3D11 IASetIndexBuffer rebind)
    uint32_t argsOffsetBytes;     // Byte offset into the args buffer
    uint32_t maxDrawCount;        // Number of draw commands to execute (upper bound with MDI_FLAG_GPU_COUNT)
    uint32_t indexFormat;         // 0 = R16_UINT, 1 = R32_UINT
    uint32_t topology;            // 0 - triangle list, 3 - line list, 4 - line strip, 5 - point list
    uint32_t flags;               // MDI_FLAG_* bitfield
    uint32_t _pad;                // pad to 8-byte alignment (matches C# NativeMDIParams)
    void*    countBuffer;         // GPU draw-count buffer (MDI_FLAG_GPU_COUNT), else null
    uint32_t countOffsetBytes;    // Byte offset of the uint32 draw count inside countBuffer
    uint32_t _pad2;               // pad to 8-byte alignment
};

static constexpr int MDI_MAX_PENDING = 256;

// Abstract backend interface — implement per graphics API
struct IMDIBackend
{
    virtual ~IMDIBackend() = default;

    // Called once when the graphics device is created
    virtual bool Initialize(IUnityInterfaces* unityInterfaces) = 0;

    // Called when the graphics device is destroyed
    virtual void Shutdown() = 0;

    // Execute multi-draw indirect using params from the given slot
    virtual void ExecuteMDI(const MDIParams& params) = 0;

    // Record the write→indirect-read barrier for an args buffer. Issued via
    // the dedicated prepare event AFTER whatever wrote the args (a compute
    // dispatch, a CPU upload) and OUTSIDE any render pass — the only place a
    // buffer barrier is legal on Vulkan. ExecuteMDI itself never records
    // barriers: requesting one mid-render-pass forces the engine to split
    // the pass, which corrupts cached state and costs bandwidth.
    // No-op on backends that don't need it.
    virtual void PrepareIndirectArgs(void* argsBuffer) { (void)argsBuffer; }

    // Returns true if the backend is ready to issue MDI calls
    virtual bool IsSupported() const = 0;

    // Resize the per-instance identity buffer (D3D11/D3D12 only, no-op on other APIs)
    virtual bool ResizeInstanceIDBuffer(uint32_t newMaxCount) { (void)newMaxCount; return true; }

    // Current identity buffer capacity
    virtual uint32_t GetMaxInstanceCount() const { return 0; }
};
