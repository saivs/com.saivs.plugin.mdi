#include "MDIBackend_D3D11.h"

#ifdef _WIN32

#include <vector>
#include <cstring>
#include <d3d11shader.h>
#include "MDILog.h"
#include "InlineHook.h"
#include "DxbcInputSignature.h"

// -----------------------------------------------------------------------
// Input layout hook: patch TEXCOORD7 to per-instance on slot 15
// -----------------------------------------------------------------------

static constexpr uint32_t kInstanceVBSlot_D3D11 = 15;

using PFN_CreateInputLayout = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT,
    const void*, SIZE_T, ID3D11InputLayout**);

static bool g_d3d11DeviceHooked = false;
static InlineHookData g_hookInputLayout;
static uint32_t g_ilCallCount    = 0;
static uint32_t g_ilInjectedCount = 0;
static uint32_t g_ilAddedCount    = 0;
static uint32_t g_ilSkippedCount  = 0;

// -----------------------------------------------------------------------
// VS bytecode reflection — detect TEXCOORD7 in vertex shader input signature
//
// CreateInputLayout already receives the VS bytecode (4th argument, used to
// validate IL against the VS input signature). We reflect it on-the-fly to
// decide whether the user's shader declared TEXCOORD7 via MDI_INSTANCE_ID_PARAMETER.
// If yes and the IL doesn't carry TEXCOORD7 (i.e. the user's mesh has no such
// attribute), we add a per-instance TEXCOORD7 element on slot 15 — bound to
// our identity buffer at draw time.
//
// d3dcompiler_47.dll is loaded dynamically: no link-time dep, ships with
// every Win10+ install. If it's somehow missing we silently skip mesh-path
// augmentation (the existing indexed-path patching still works).
// -----------------------------------------------------------------------

// d3dcompiler_47.dll IID for ID3D11ShaderReflection (defined locally to avoid linking d3dcompiler.lib).
static const GUID kIID_ID3D11ShaderReflection_v47 =
    { 0x8d536ca1, 0x0cca, 0x4956, { 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 } };

using PFN_D3DReflect_t = HRESULT (WINAPI *)(LPCVOID, SIZE_T, REFIID, void**);

static HMODULE         g_d3dCompilerModule    = nullptr;
static PFN_D3DReflect_t g_D3DReflect          = nullptr;
static bool            g_d3dCompilerAttempted = false;

static void EnsureD3DCompilerLoaded()
{
    if (g_d3dCompilerAttempted) return;
    g_d3dCompilerAttempted = true;

    g_d3dCompilerModule = LoadLibraryA("d3dcompiler_47.dll");
    if (!g_d3dCompilerModule)
        g_d3dCompilerModule = LoadLibraryA("d3dcompiler_46.dll");

    if (g_d3dCompilerModule)
        g_D3DReflect = reinterpret_cast<PFN_D3DReflect_t>(
            GetProcAddress(g_d3dCompilerModule, "D3DReflect"));

    DebugLog("[MDI] D3D11 D3DReflect: %s\n", g_D3DReflect ? "loaded" : "NOT loaded (mesh-path augmentation disabled)");
}

// Parses the DXBC binary directly to find a TEXCOORD7 entry in the input
// signature (ISGN / ISG1 chunk). Necessary because Unity often passes a
// signature-only blob (from D3DGetInputSignatureBlob) to CreateInputLayout,
// and D3DReflect rejects those with E_INVALIDARG — it needs the full SHEX
// chunk. Implementation lives in DxbcInputSignature.h, shared with the D3D12
// backend (which relies on it for shader model 6 / DXIL).
//
// requireUint32 is false here: fxc for D3D11 doesn't reliably report uint
// inputs as UINT32, matching the reflection path's behaviour below.
static bool ParseDxbcInputSignature_HasTexcoord7(const void* bytecode, SIZE_T size)
{
    return MDIDxbc::InputSignatureHasSemantic(bytecode, size, "TEXCOORD", 7,
                                              /*requireUint32=*/false);
}

// Detects the MDI_INSTANCE_ID_PARAMETER marker: a TEXCOORD7 input. Verbose
// logging for the first few invocations so we can verify whether FXC strips
// the input, what ComponentType it reports, and whether Unity passes a full
// VS bytecode or a signature-only blob to CreateInputLayout.
static bool VSInputHasTexcoord7(const void* bytecode, SIZE_T size)
{
    static uint32_t s_invokeCount = 0;
    uint32_t myInvoke = ++s_invokeCount;
    bool verbose = (myInvoke <= 8);

    EnsureD3DCompilerLoaded();
    if (!g_D3DReflect || !bytecode || size == 0)
    {
        if (verbose)
            DebugLog("[MDI] D3D11 VSInputHasTexcoord7 #%u: early-out reflect=%p ptr=%p size=%zu\n",
                     myInvoke, g_D3DReflect, bytecode, size);
        return false;
    }

    ID3D11ShaderReflection* refl = nullptr;
    HRESULT hr = g_D3DReflect(bytecode, size, kIID_ID3D11ShaderReflection_v47,
                              reinterpret_cast<void**>(&refl));
    if (FAILED(hr) || !refl)
    {
        // D3DReflect needs the full DXBC with SHEX chunk; Unity often passes
        // a signature-only blob (D3DGetInputSignatureBlob) here. Parse the
        // ISGN chunk directly as a fallback.
        bool found = ParseDxbcInputSignature_HasTexcoord7(bytecode, size);
        if (verbose)
            DebugLog("[MDI] D3D11 VSInputHasTexcoord7 #%u: D3DReflect hr=0x%08X size=%zu, sigblob-parser found=%d\n",
                     myInvoke, hr, size, found ? 1 : 0);
        return found;
    }

    D3D11_SHADER_DESC desc = {};
    refl->GetDesc(&desc);

    if (verbose)
        DebugLog("[MDI] D3D11 VSInputHasTexcoord7 #%u: size=%zu, InputParameters=%u\n",
                 myInvoke, size, desc.InputParameters);

    bool found = false;
    bool sawAnyTexcoord7 = false;
    for (UINT i = 0; i < desc.InputParameters; ++i)
    {
        D3D11_SIGNATURE_PARAMETER_DESC p = {};
        if (FAILED(refl->GetInputParameterDesc(i, &p))) continue;
        if (verbose)
            DebugLog("[MDI]   input[%u] %s%u CompType=%d Mask=0x%X RWMask=0x%X\n",
                     i, p.SemanticName ? p.SemanticName : "?", p.SemanticIndex,
                     p.ComponentType, p.Mask, p.ReadWriteMask);
        if (!p.SemanticName || strcmp(p.SemanticName, "TEXCOORD") != 0 || p.SemanticIndex != 7)
            continue;
        sawAnyTexcoord7 = true;
        // Accept any TEXCOORD7 in the VS input signature — FXC for D3D11
        // doesn't reliably report uint inputs as ComponentType=UINT32.
        // The strict UINT32 filter is kept for D3D12 (DXC) only.
        found = true;
        break;
    }

    if (verbose && !sawAnyTexcoord7)
        DebugLog("[MDI] D3D11 VSInputHasTexcoord7 #%u: no TEXCOORD7 in input signature\n", myInvoke);

    refl->Release();
    return found;
}

// Any TEXCOORD7 in the IL — regardless of format. If present, we REPLACE
// that element (a duplicate semantic would make CreateInputLayout fail).
static bool HasTexcoord7_D3D11(const D3D11_INPUT_ELEMENT_DESC* elements, UINT count)
{
    for (UINT i = 0; i < count; ++i)
    {
        if (elements[i].SemanticIndex == 7 &&
            elements[i].SemanticName && strcmp(elements[i].SemanticName, "TEXCOORD") == 0)
            return true;
    }
    return false;
}

// TEXCOORD7 already in IL as R32_UINT — strongest possible MDI marker because
// no Unity shader declares TEXCOORD7 with this format. This is the signature
// our C# prime mesh produces (VertexAttributeFormat.UInt32, 1 component).
static bool HasR32UintTexcoord7_D3D11(const D3D11_INPUT_ELEMENT_DESC* elements, UINT count)
{
    for (UINT i = 0; i < count; ++i)
    {
        if (elements[i].SemanticIndex == 7 &&
            elements[i].SemanticName && strcmp(elements[i].SemanticName, "TEXCOORD") == 0 &&
            elements[i].Format == DXGI_FORMAT_R32_UINT)
            return true;
    }
    return false;
}

static bool IsTexcoord7Correct_D3D11(const D3D11_INPUT_ELEMENT_DESC* elements, UINT count)
{
    for (UINT i = 0; i < count; ++i)
    {
        if (elements[i].SemanticIndex == 7 &&
            elements[i].SemanticName && strcmp(elements[i].SemanticName, "TEXCOORD") == 0)
        {
            return elements[i].InputSlot == kInstanceVBSlot_D3D11 &&
                   elements[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA &&
                   elements[i].InstanceDataStepRate == 1 &&
                   elements[i].Format == DXGI_FORMAT_R32_UINT;
        }
    }
    return false;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateInputLayout(
    ID3D11Device* self,
    const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs,
    UINT NumElements,
    const void* pShaderBytecodeWithInputSignature,
    SIZE_T BytecodeLength,
    ID3D11InputLayout** ppInputLayout)
{
    g_ilCallCount++;

    auto callOrig = reinterpret_cast<PFN_CreateInputLayout>(g_hookInputLayout.trampoline);

    if (!pInputElementDescs || NumElements == 0)
        return callOrig(self, pInputElementDescs, NumElements,
                        pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout);

    // Two independent MDI markers:
    //   1. IL contains R32_UINT TEXCOORD7 — only our C# prime mesh produces this.
    //      Reliable signal regardless of how FXC reports the VS input type.
    //   2. VS bytecode declares a uint scalar TEXCOORD7 (MDI_INSTANCE_ID_PARAMETER).
    //      Necessary for the mesh path where the user's mesh has no TEXCOORD7.
    // Either one is sufficient to treat this IL as MDI.
    bool ilR32UintTexcoord7 = HasR32UintTexcoord7_D3D11(pInputElementDescs, NumElements);
    bool vsHasMdiMarker     = VSInputHasTexcoord7(pShaderBytecodeWithInputSignature, BytecodeLength);

    if (g_ilCallCount <= 8)
        DebugLog("[MDI] D3D11 IL call #%u: elems=%u, primeMeshIL=%d, vsMarker=%d\n",
                 g_ilCallCount, NumElements, ilR32UintTexcoord7 ? 1 : 0, vsHasMdiMarker ? 1 : 0);

    if (!ilR32UintTexcoord7 && !vsHasMdiMarker)
    {
        g_ilSkippedCount++;
        return callOrig(self, pInputElementDescs, NumElements,
                        pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout);
    }

    if (IsTexcoord7Correct_D3D11(pInputElementDescs, NumElements))
    {
        g_ilSkippedCount++;
        return callOrig(self, pInputElementDescs, NumElements,
                        pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout);
    }

    bool ilHasTexcoord7 = HasTexcoord7_D3D11(pInputElementDescs, NumElements);
    std::vector<D3D11_INPUT_ELEMENT_DESC> result(pInputElementDescs, pInputElementDescs + NumElements);

    if (ilHasTexcoord7)
    {
        // Replace the existing TEXCOORD7 — user mesh may carry it as a different
        // format (e.g. float UV2). Appending a duplicate would fail validation.
        for (auto& e : result)
        {
            if (e.SemanticIndex == 7 && e.SemanticName && strcmp(e.SemanticName, "TEXCOORD") == 0)
            {
                e.InputSlot            = kInstanceVBSlot_D3D11;
                e.AlignedByteOffset    = 0;
                e.Format               = DXGI_FORMAT_R32_UINT;
                e.InputSlotClass       = D3D11_INPUT_PER_INSTANCE_DATA;
                e.InstanceDataStepRate = 1;
            }
        }
    }
    else
    {
        D3D11_INPUT_ELEMENT_DESC tex7 = {};
        tex7.SemanticName         = "TEXCOORD";
        tex7.SemanticIndex        = 7;
        tex7.Format               = DXGI_FORMAT_R32_UINT;
        tex7.InputSlot            = kInstanceVBSlot_D3D11;
        tex7.AlignedByteOffset    = 0;
        tex7.InputSlotClass       = D3D11_INPUT_PER_INSTANCE_DATA;
        tex7.InstanceDataStepRate = 1;
        result.push_back(tex7);
    }

    HRESULT hr = callOrig(self, result.data(), static_cast<UINT>(result.size()),
                          pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout);

    if (ilHasTexcoord7) g_ilInjectedCount++; else g_ilAddedCount++;
    if ((g_ilInjectedCount + g_ilAddedCount) <= 8)
        DebugLog("[MDI] D3D11 InputLayout hook: %s per-instance TEXCOORD7 on slot %u, "
                 "elements %u -> %u, hr=0x%08X\n",
                 ilHasTexcoord7 ? "REPLACED" : "ADDED",
                 kInstanceVBSlot_D3D11, NumElements, (UINT)result.size(), hr);
    return hr;
}

// -----------------------------------------------------------------------
// NvAPI
// -----------------------------------------------------------------------

static constexpr unsigned int NVAPI_ID_Initialize                          = 0x0150e828;
static constexpr unsigned int NVAPI_ID_Unload                              = 0xd22bdd7e;
static constexpr unsigned int NVAPI_ID_D3D_RegisterDevice                  = 0x8c02c4d0;
static constexpr unsigned int NVAPI_ID_D3D11_MultiDrawIndexedInstancedIndirect = 0x59e890f9;

using NvAPI_QueryInterface_t = void* (*)(unsigned int id);

static bool IsRenderDocPresent()
{
    return GetModuleHandleA("renderdoc.dll") != nullptr;
}

bool MDIBackend_D3D11::TryInitNvAPI()
{
    _nvApiModule = LoadLibraryA("nvapi64.dll");
    if (!_nvApiModule) return false;

    auto queryInterface = reinterpret_cast<NvAPI_QueryInterface_t>(
        GetProcAddress(_nvApiModule, "nvapi_QueryInterface"));
    if (!queryInterface)
    {
        FreeLibrary(_nvApiModule); _nvApiModule = nullptr;
        return false;
    }

    auto nvInit     = reinterpret_cast<NvAPI_Initialize_t>(queryInterface(NVAPI_ID_Initialize));
    auto nvRegister = reinterpret_cast<NvAPI_D3D_RegisterDevice_t>(queryInterface(NVAPI_ID_D3D_RegisterDevice));
    _nvApiUnload    = reinterpret_cast<NvAPI_Unload_t>(queryInterface(NVAPI_ID_Unload));
    _nvApiMDI       = reinterpret_cast<NvAPI_D3D11_MultiDrawIndexedInstancedIndirect_t>(
                          queryInterface(NVAPI_ID_D3D11_MultiDrawIndexedInstancedIndirect));

    if (!nvInit || !nvRegister || !_nvApiUnload || !_nvApiMDI)
    {
        FreeLibrary(_nvApiModule); _nvApiModule = nullptr; _nvApiMDI = nullptr;
        return false;
    }
    if (nvInit() != 0)
    {
        FreeLibrary(_nvApiModule); _nvApiModule = nullptr; _nvApiMDI = nullptr;
        return false;
    }
    if (nvRegister(_device) != 0)
    {
        _nvApiUnload();
        FreeLibrary(_nvApiModule); _nvApiModule = nullptr; _nvApiMDI = nullptr;
        return false;
    }
    return true;
}

void MDIBackend_D3D11::ShutdownNvAPI()
{
    if (_nvApiUnload) _nvApiUnload();
    if (_nvApiModule) { FreeLibrary(_nvApiModule); _nvApiModule = nullptr; }
    _nvApiMDI = nullptr; _nvApiUnload = nullptr;
    _nvApiReady = false; _nvApiAttempted = false;
}

void MDIBackend_D3D11::EnsureNvAPIInitialized()
{
    if (_nvApiAttempted) return;
    _nvApiAttempted = true;

    if (IsRenderDocPresent())
    {
        DebugLog("[MDI] RenderDoc detected — skipping NvAPI\n");
        _nvApiReady = false;
        return;
    }

    _nvApiReady = TryInitNvAPI();
    DebugLog("[MDI] Lazy NvAPI init: %s\n", _nvApiReady ? "OK" : "failed/skipped");
}

// -----------------------------------------------------------------------
// Initialize / Shutdown
// -----------------------------------------------------------------------

bool MDIBackend_D3D11::Initialize(IUnityInterfaces* unityInterfaces)
{
    auto* d3d11 = unityInterfaces->Get<IUnityGraphicsD3D11>();
    if (!d3d11) return false;

    _device = d3d11->GetDevice();
    if (!_device) return false;

    _device->GetImmediateContext(&_context);
    if (!_context) return false;

    _context->QueryInterface(__uuidof(ID3DUserDefinedAnnotation),
        reinterpret_cast<void**>(&_annotation));

    _nvApiReady = false;
    _nvApiAttempted = false;

    // InputLayout hook must be installed early — Unity compiles many input
    // layouts (including those for user meshes with our MDI shader) at scene
    // load, BEFORE the first C# MDI draw call. A lazy install would miss
    // those, and any IL whose VS expects TEXCOORD7 but lacks it would fail
    // validation. Safety comes from the strict UINT32 / R32_UINT match in
    // VSInputHasTexcoord7 / HasTexcoord7_D3D11 — only our own
    // MDI_INSTANCE_ID_PARAMETER shaders are affected.
    InstallDeviceHook();
    CreateInstanceIDBuffer();

    DebugLog("[MDI] D3D11 backend initialized (InputLayout hook + per-instance VB + NvAPI deferred)\n");
    _initialized = true;
    return true;
}

void MDIBackend_D3D11::InstallDeviceHook()
{
    if (g_d3d11DeviceHooked || !_device) return;

    // ID3D11Device vtable[11] = CreateInputLayout
    void** vtable = *reinterpret_cast<void***>(_device);
    auto fnCreateIL = reinterpret_cast<void*>(vtable[11]);

    DebugLog("[MDI] D3D11 Device %p, vtable %p\n", _device, vtable);
    DebugLog("[MDI] CreateInputLayout = %p\n", fnCreateIL);

    bool hooked = InstallInlineHook(
        fnCreateIL,
        reinterpret_cast<void*>(&Hook_CreateInputLayout),
        g_hookInputLayout);

    g_d3d11DeviceHooked = true;
    DebugLog("[MDI] D3D11 InputLayout inline hook: %d\n", hooked);
}

void MDIBackend_D3D11::CreateInstanceIDBuffer()
{
    std::vector<uint32_t> data(_maxInstanceCount);
    for (uint32_t i = 0; i < _maxInstanceCount; ++i)
        data[i] = i;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth  = _maxInstanceCount * sizeof(uint32_t);
    desc.Usage      = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags  = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = data.data();

    HRESULT hr = _device->CreateBuffer(&desc, &initData, &_instanceIDBuffer);
    if (FAILED(hr))
        DebugLog("[MDI] Failed to create D3D11 instance ID buffer: 0x%08X\n", hr);
    else
        DebugLog("[MDI] D3D11 Instance ID buffer ready: %u entries, %u bytes\n",
                 _maxInstanceCount, _maxInstanceCount * (uint32_t)sizeof(uint32_t));
}

bool MDIBackend_D3D11::ResizeInstanceIDBuffer(uint32_t newMaxCount)
{
    if (newMaxCount == 0) return false;
    if (newMaxCount == _maxInstanceCount && _instanceIDBuffer) return true;

    if (_instanceIDBuffer) { _instanceIDBuffer->Release(); _instanceIDBuffer = nullptr; }

    _maxInstanceCount = newMaxCount;
    CreateInstanceIDBuffer();
    return _instanceIDBuffer != nullptr;
}

void MDIBackend_D3D11::Shutdown()
{
    if (_nvApiReady) ShutdownNvAPI();

    if (g_d3d11DeviceHooked)
    {
        RemoveInlineHook(g_hookInputLayout);
        g_d3d11DeviceHooked = false;
    }

    if (_instanceIDBuffer) { _instanceIDBuffer->Release(); _instanceIDBuffer = nullptr; }
    if (_annotation) { _annotation->Release(); _annotation = nullptr; }
    if (_context) { _context->Release(); _context = nullptr; }
    _device = nullptr;
    _initialized = false;

    if (g_d3dCompilerModule)
    {
        FreeLibrary(g_d3dCompilerModule);
        g_d3dCompilerModule = nullptr;
    }
    g_D3DReflect = nullptr;
    g_d3dCompilerAttempted = false;
}

// -----------------------------------------------------------------------
// ExecuteMDI
// -----------------------------------------------------------------------

void MDIBackend_D3D11::ExecuteMDI(const MDIParams& params)
{
    if (!_initialized || !_context || !params.argsBuffer || params.maxDrawCount == 0)
        return;

    EnsureNvAPIInitialized();

    auto* argsBuffer = static_cast<ID3D11Buffer*>(params.argsBuffer);
    constexpr uint32_t stride = 20;

    // Note: D3D11 has no GPU-count draw API (neither NvAPI MDI nor the loop
    // can read a count buffer). With MDI_FLAG_GPU_COUNT we execute the
    // maxDrawCount upper bound — the compute shader must zero the
    // instanceCount of unused args entries so extra draws produce nothing.

    // -------------------------------------------------------------------
    // Save IA state we're about to clobber. Unity tracks its own bound
    // state on D3D11; if we change IB / VB slot 15 / topology and don't
    // restore, Unity's shadow-state cache stays stale and subsequent draws
    // (notably Editor IMGUI, rendered later on the same immediate context)
    // inherit our state and misrender.
    // -------------------------------------------------------------------
    ID3D11Buffer*            savedIB        = nullptr;
    DXGI_FORMAT              savedIBFormat  = DXGI_FORMAT_UNKNOWN;
    UINT                     savedIBOffset  = 0;
    ID3D11Buffer*            savedVB15      = nullptr;
    UINT                     savedVB15Stride = 0;
    UINT                     savedVB15Offset = 0;
    D3D11_PRIMITIVE_TOPOLOGY savedTopology  = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

    if (params.indexBuffer)
        _context->IAGetIndexBuffer(&savedIB, &savedIBFormat, &savedIBOffset);
    if (_instanceIDBuffer)
        _context->IAGetVertexBuffers(kInstanceVBSlot, 1, &savedVB15, &savedVB15Stride, &savedVB15Offset);
    _context->IAGetPrimitiveTopology(&savedTopology);

    // Rebind caller's index buffer (prime DrawMesh sets its own 3-index IB)
    if (params.indexBuffer)
    {
        auto* ib = static_cast<ID3D11Buffer*>(params.indexBuffer);
        DXGI_FORMAT fmt = (params.indexFormat == 1) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        _context->IASetIndexBuffer(ib, fmt, 0);
    }

    // Bind per-instance identity VB to slot 15
    if (_instanceIDBuffer)
    {
        UINT vbStride = sizeof(uint32_t);
        UINT vbOffset = 0;
        _context->IASetVertexBuffers(kInstanceVBSlot, 1, &_instanceIDBuffer, &vbStride, &vbOffset);
    }

    switch (params.topology)
    {
        // MeshTopology.Triangles
        default:
        case 0:
            _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            break;
        // MeshTopology.Lines
        case 3:
            _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            break;
        case 4:
            _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
            break;
        case 5:
            _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            break;
    }

    // Debug marker
    if (_annotation)
        _annotation->BeginEvent(
            (_nvApiReady && _nvApiMDI)
                ? L"MDI::NvAPI [Native Hardware MDI + PerInstance VB]"
                : L"MDI::NativeLoop [Plugin Loop + PerInstance VB]");

    if (_nvApiReady && _nvApiMDI)
    {
        // NvAPI hardware MDI — single call
        _nvApiMDI(
            _context,
            params.maxDrawCount,
            argsBuffer,
            params.argsOffsetBytes,
            stride
        );
    }
    else
    {
        // Fallback: CPU loop of DrawIndexedInstancedIndirect
        for (uint32_t i = 0; i < params.maxDrawCount; ++i)
        {
            _context->DrawIndexedInstancedIndirect(
                argsBuffer,
                params.argsOffsetBytes + i * stride
            );
        }
    }

    if (_annotation)
        _annotation->EndEvent();

    // -------------------------------------------------------------------
    // Restore IA state. IAGetVertexBuffers / IAGetIndexBuffer return
    // AddRef'd interface pointers — must Release after re-setting.
    // -------------------------------------------------------------------
    if (params.indexBuffer)
    {
        _context->IASetIndexBuffer(savedIB, savedIBFormat, savedIBOffset);
        if (savedIB) savedIB->Release();
    }
    if (_instanceIDBuffer)
    {
        _context->IASetVertexBuffers(kInstanceVBSlot, 1, &savedVB15, &savedVB15Stride, &savedVB15Offset);
        if (savedVB15) savedVB15->Release();
    }
    _context->IASetPrimitiveTopology(savedTopology);

    static uint32_t s_callCount = 0;
    s_callCount++;
    if (s_callCount == 1 || s_callCount == 10 || s_callCount == 100 ||
        (s_callCount % 1000) == 0)
    {
        DebugLog("[MDI] D3D11 ExecuteMDI #%u: drawCount=%u, offset=%u, nvapi=%d\n",
                 s_callCount, params.maxDrawCount, params.argsOffsetBytes, _nvApiReady);
        DebugLog("[MDI] D3D11 IL hook stats: calls=%u, patched=%u, added=%u, skipped=%u\n",
                 g_ilCallCount, g_ilInjectedCount, g_ilAddedCount, g_ilSkippedCount);
    }
}

bool MDIBackend_D3D11::IsSupported() const
{
    return _initialized;
}

#endif // _WIN32
