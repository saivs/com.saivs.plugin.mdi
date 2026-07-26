#include "MDIBackend_D3D12.h"

#ifdef _WIN32

#include <vector>
#include <cstring>
#include <d3d11shader.h>
#include "MDILog.h"
#include "InlineHook.h"
#include "DxbcInputSignature.h"

// -----------------------------------------------------------------------
// PSO hook: inject per-instance TEXCOORD7 into graphics PSOs.
//   • If IL already declares TEXCOORD7 (indexed prime-mesh path) -> patch
//     it to per-instance on slot 15.
//   • If IL has no TEXCOORD7 but the user shader's VS declares it
//     (mesh path: user shader uses MDI_INSTANCE_ID_PARAMETER over a
//     user-supplied mesh) -> append a per-instance TEXCOORD7 element on
//     slot 15. Bytecode is already passed into the PSO desc, so we just
//     reflect it inline.
//   • Otherwise pass through.
// -----------------------------------------------------------------------

static constexpr uint32_t kInstanceVBSlot = 15;

// -----------------------------------------------------------------------
// VS bytecode reflection (shared logic with D3D11 backend).
// d3dcompiler_47.dll loaded dynamically — ships with every Win10+; if it's
// somehow missing, we fall back to the container parser in
// DxbcInputSignature.h (existing indexed-path patching also keeps working
// since it doesn't rely on reflection at all).
//
// Shader model 6 note: Unity ships DXIL for SM6+, and D3DReflect only
// understands DXBC (SM <= 5.1), so it fails on every SM6 shader. DXIL
// containers still carry an input-signature chunk, so the parser fallback
// is what makes the TEXCOORD7 detection work under SM6. Without it,
// VSInputHasTexcoord7 returned false for all SM6 VS bytecode and the
// per-instance TEXCOORD7 element was never injected.
// -----------------------------------------------------------------------

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

    DebugLog("[MDI] D3D12 D3DReflect: %s\n",
             g_D3DReflect ? "loaded" : "NOT loaded (using container-parser fallback)");
}

// Detects the MDI_INSTANCE_ID_PARAMETER marker: a TEXCOORD7 input declared as
// a single uint scalar (`nointerpolation uint id : TEXCOORD7`). This is the
// strongest signal we can pull from the bytecode without modifying it — the
// combination "TEXCOORD7 + ComponentType=UINT32 + only x-component read" is
// essentially unique to our macro. Standard Unity shaders that happen to use
// TEXCOORD7 use it for UV-like FLOAT32 data (Mask >= 3), so they won't match.
static bool VSInputHasTexcoord7(const void* bytecode, SIZE_T size)
{
    EnsureD3DCompilerLoaded();
    if (!bytecode || size == 0)
        return false;

    // Quick DXBC magic check ('DXBC' as little-endian uint32 = 0x43425844).
    // Guards against fake bytecode pointers from heuristic stream scans.
    // Note: DXIL (SM6) containers use this same fourCC, so passing this check
    // does not imply the blob is reflectable by D3DReflect.
    if (size < 4 || *reinterpret_cast<const uint32_t*>(bytecode) != 0x43425844u)
        return false;

    // Strict UINT32 match is preserved in the fallback to keep the
    // false-positive risk identical to the reflection path below.
    auto parseFallback = [&]() -> bool {
        return MDIDxbc::InputSignatureHasSemantic(bytecode, size, "TEXCOORD", 7,
                                                  /*requireUint32=*/true);
    };

    if (!g_D3DReflect)
        return parseFallback();

    ID3D11ShaderReflection* refl = nullptr;
    HRESULT hr = g_D3DReflect(bytecode, size, kIID_ID3D11ShaderReflection_v47,
                              reinterpret_cast<void**>(&refl));
    if (FAILED(hr) || !refl)
    {
        // Expected for every SM6 shader (DXIL) and for signature-only blobs.
        bool found = parseFallback();
        static uint32_t s_fallbackCount = 0;
        if (s_fallbackCount++ < 8)
            DebugLog("[MDI] D3D12 D3DReflect failed hr=0x%08X size=%zu, container-parser found=%d\n",
                     hr, size, found ? 1 : 0);
        return found;
    }

    D3D11_SHADER_DESC desc = {};
    refl->GetDesc(&desc);

    bool found = false;
    for (UINT i = 0; i < desc.InputParameters; ++i)
    {
        D3D11_SIGNATURE_PARAMETER_DESC p = {};
        if (FAILED(refl->GetInputParameterDesc(i, &p))) continue;
        if (!p.SemanticName || strcmp(p.SemanticName, "TEXCOORD") != 0 || p.SemanticIndex != 7)
            continue;
        // Must be a uint type — rules out the common float UV-style TEXCOORD7.
        // We don't constrain the Mask here: FXC and DXC differ on how they
        // report the mask for uint inputs and we'd miss legitimate cases.
        static uint32_t s_dumpCount = 0;
        if (s_dumpCount++ < 8)
            DebugLog("[MDI] D3D12 VS TEXCOORD7 found: ComponentType=%d Mask=0x%X RWMask=0x%X\n",
                     p.ComponentType, p.Mask, p.ReadWriteMask);
        if (p.ComponentType != D3D_REGISTER_COMPONENT_UINT32)
            continue;
        found = true;
        break;
    }

    refl->Release();
    return found;
}

// Build a deep-copy of the input layout that adds a per-instance TEXCOORD7
// on slot 15 (used for the mesh-path "VS expects but IL lacks" case).
static void BuildAugmentedLayout(const D3D12_INPUT_ELEMENT_DESC* src, UINT srcCount,
                                  std::vector<D3D12_INPUT_ELEMENT_DESC>& out)
{
    out.assign(src, src + srcCount);

    D3D12_INPUT_ELEMENT_DESC tex7 = {};
    tex7.SemanticName         = "TEXCOORD";
    tex7.SemanticIndex        = 7;
    tex7.Format               = DXGI_FORMAT_R32_UINT;
    tex7.InputSlot            = kInstanceVBSlot;
    tex7.AlignedByteOffset    = 0;
    tex7.InputSlotClass       = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
    tex7.InstanceDataStepRate = 1;
    out.push_back(tex7);
}

// Legacy API
using PFN_CreateGraphicsPipelineState = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);

// Stream-based API (ID3D12Device2)
using PFN_CreatePipelineState = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);

// Pipeline Library hook — Unity may cache PSOs via ID3D12PipelineLibrary
using PFN_CreatePipelineLibrary = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, const void*, SIZE_T, REFIID, void**);
using PFN_LoadGraphicsPipeline = HRESULT(STDMETHODCALLTYPE*)(
    void*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);

static PFN_CreateGraphicsPipelineState g_origCreateGraphicsPSO  = nullptr;
static PFN_CreatePipelineState         g_origCreatePipelineState = nullptr;
static bool g_deviceHooked = false;

static uint32_t g_psoLegacyCallCount  = 0;
static uint32_t g_psoStreamCallCount  = 0;
static uint32_t g_psoInjectedCount    = 0;
static uint32_t g_psoAddedCount       = 0;
static uint32_t g_psoSkippedCount     = 0;
static uint32_t g_pipelineLibCallCount = 0;
static uint32_t g_loadGfxPipelineCallCount = 0;

// InlineHookData, InstallInlineHook, RemoveInlineHook from InlineHook.h

static InlineHookData g_hookLegacy;
static InlineHookData g_hookStream;
static InlineHookData g_hookPipelineLib;
static InlineHookData g_hookLoadGfxPipeline;

// -----------------------------------------------------------------------
// Shared: check if TEXCOORD7 already exists in input layout
// -----------------------------------------------------------------------

// Any TEXCOORD7 in the input layout — regardless of format. If present, we
// must REPLACE that element with our per-instance R32_UINT one (a duplicate
// would make CreateGraphicsPipelineState fail with E_INVALIDARG).
static bool HasTexcoord7(const D3D12_INPUT_ELEMENT_DESC* elements, UINT count)
{
    for (UINT i = 0; i < count; ++i)
    {
        if (elements[i].SemanticIndex == 7 &&
            elements[i].SemanticName && strcmp(elements[i].SemanticName, "TEXCOORD") == 0)
            return true;
    }
    return false;
}

// Already correctly configured for MDI: slot 15, per-instance, step rate 1,
// AND format R32_UINT. Anything else triggers a replace.
static bool IsTexcoord7Correct(const D3D12_INPUT_ELEMENT_DESC* elements, UINT count)
{
    for (UINT i = 0; i < count; ++i)
    {
        if (elements[i].SemanticIndex == 7 &&
            elements[i].SemanticName && strcmp(elements[i].SemanticName, "TEXCOORD") == 0)
        {
            return elements[i].InputSlot == kInstanceVBSlot &&
                   elements[i].InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA &&
                   elements[i].InstanceDataStepRate == 1 &&
                   elements[i].Format == DXGI_FORMAT_R32_UINT;
        }
    }
    return false;
}

// Build input layout: copy all elements, patch existing TEXCOORD7 to per-instance on slot 15.
// Only modifies PSOs that already have TEXCOORD7 (from our MDI Mesh vertex layout).
// PSOs without TEXCOORD7 (skybox, post-processing, etc.) are left untouched.
static void BuildInjectedLayout(const D3D12_INPUT_ELEMENT_DESC* src, UINT srcCount,
                                std::vector<D3D12_INPUT_ELEMENT_DESC>& out)
{
    out.clear();

    for (UINT i = 0; i < srcCount; ++i)
    {
        if (src[i].SemanticIndex == 7 &&
            src[i].SemanticName && strcmp(src[i].SemanticName, "TEXCOORD") == 0)
        {
            // Replace existing TEXCOORD7: change to per-instance on slot 15
            D3D12_INPUT_ELEMENT_DESC patched = src[i];
            patched.InputSlot            = kInstanceVBSlot;
            patched.AlignedByteOffset    = 0;
            patched.Format               = DXGI_FORMAT_R32_UINT;
            patched.InputSlotClass       = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
            patched.InstanceDataStepRate = 1;
            out.push_back(patched);
        }
        else
        {
            out.push_back(src[i]);
        }
    }
}

// -----------------------------------------------------------------------
// Hook: CreateGraphicsPipelineState (legacy, vtable[10])
// -----------------------------------------------------------------------

static HRESULT STDMETHODCALLTYPE Hook_CreateGraphicsPipelineState(
    ID3D12Device* self,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
    REFIID riid,
    void** ppPipelineState)
{
    g_psoLegacyCallCount++;

    // Call original via trampoline
    auto callOrig = reinterpret_cast<PFN_CreateGraphicsPipelineState>(g_hookLegacy.trampoline);

    if (!pDesc)
        return callOrig(self, pDesc, riid, ppPipelineState);

    const auto& il = pDesc->InputLayout;

    // The defining MDI marker is in the VS bytecode: a uint scalar TEXCOORD7
    // input (MDI_INSTANCE_ID_PARAMETER). PSOs without this marker get a true
    // pass-through.
    if (!VSInputHasTexcoord7(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength))
    {
        g_psoSkippedCount++;
        return callOrig(self, pDesc, riid, ppPipelineState);
    }

    // Already correctly configured for MDI (slot 15, per-instance, R32_UINT) —
    // skip, otherwise we'd modify a PSO we previously rewrote.
    if (IsTexcoord7Correct(il.pInputElementDescs, il.NumElements))
    {
        g_psoSkippedCount++;
        return callOrig(self, pDesc, riid, ppPipelineState);
    }

    // Replace if IL already carries a TEXCOORD7 (any format — user mesh may
    // already have it as e.g. float UV2; we override since the VS demands a
    // uint). Add otherwise.
    bool ilHasTexcoord7 = HasTexcoord7(il.pInputElementDescs, il.NumElements);
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    if (ilHasTexcoord7)
        BuildInjectedLayout(il.pInputElementDescs, il.NumElements, elements);
    else
        BuildAugmentedLayout(il.pInputElementDescs, il.NumElements, elements);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC modifiedDesc = *pDesc;
    modifiedDesc.InputLayout.pInputElementDescs = elements.data();
    modifiedDesc.InputLayout.NumElements        = static_cast<UINT>(elements.size());

    HRESULT hr = callOrig(self, &modifiedDesc, riid, ppPipelineState);

    if (ilHasTexcoord7) g_psoInjectedCount++; else g_psoAddedCount++;
    if ((g_psoInjectedCount + g_psoAddedCount) <= 8)
        DebugLog("[MDI] PSO legacy hook: %s per-instance TEXCOORD7 on slot %u, "
                 "elements %u -> %u, hr=0x%08X\n",
                 ilHasTexcoord7 ? "REPLACED" : "ADDED",
                 kInstanceVBSlot, il.NumElements, (UINT)elements.size(), hr);
    return hr;
}

// -----------------------------------------------------------------------
// Hook: CreatePipelineState (stream-based, vtable[46])
//
// Scans the pipeline state stream for the INPUT_LAYOUT subobject,
// modifies it to include our per-instance TEXCOORD7 element.
// -----------------------------------------------------------------------

static HRESULT STDMETHODCALLTYPE Hook_CreatePipelineState(
    ID3D12Device* self,
    const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc,
    REFIID riid,
    void** ppPipelineState)
{
    g_psoStreamCallCount++;

    // Call original via trampoline
    auto callOrig = reinterpret_cast<PFN_CreatePipelineState>(g_hookStream.trampoline);

    if (!pDesc || !pDesc->pPipelineStateSubobjectStream || pDesc->SizeInBytes == 0)
        return callOrig(self, pDesc, riid, ppPipelineState);

    // Scan stream for INPUT_LAYOUT and VS subobjects. Stream is a sequence of
    // alignas(void*) subobjects. Each starts with D3D12_PIPELINE_STATE_SUBOBJECT_TYPE
    // (uint32) followed by a type-specific data struct. For pointer-containing
    // data (INPUT_LAYOUT, VS), the data starts at offset sizeof(void*).
    //
    // We walk by 8-byte steps and identify candidates by type-tag, validating
    // each candidate against sane data ranges to reject false positives from
    // step-aliased reads inside other subobjects' data.
    auto* stream = static_cast<const uint8_t*>(pDesc->pPipelineStateSubobjectStream);
    size_t streamSize = pDesc->SizeInBytes;
    constexpr size_t kAlign = sizeof(void*);  // 8 on x64

    size_t layoutOffset = SIZE_MAX;
    const D3D12_INPUT_LAYOUT_DESC* origLayout = nullptr;
    const D3D12_SHADER_BYTECODE*   origVS     = nullptr;

    for (size_t off = 0; off + kAlign <= streamSize; off += kAlign)
    {
        auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(stream + off);

        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT && !origLayout &&
            off + kAlign + sizeof(D3D12_INPUT_LAYOUT_DESC) <= streamSize)
        {
            auto* layout = reinterpret_cast<const D3D12_INPUT_LAYOUT_DESC*>(stream + off + kAlign);
            if (layout->NumElements < 64 && layout->pInputElementDescs)
            {
                origLayout   = layout;
                layoutOffset = off;
            }
        }
        else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS && !origVS &&
                 off + kAlign + sizeof(D3D12_SHADER_BYTECODE) <= streamSize)
        {
            auto* vs = reinterpret_cast<const D3D12_SHADER_BYTECODE*>(stream + off + kAlign);
            uintptr_t p = reinterpret_cast<uintptr_t>(vs->pShaderBytecode);
            // Sanity-check: pointer in canonical user-space, size reasonable.
            // Filters out false positives from step-aliased reads inside other
            // subobjects' data (e.g. a uint32==1 field followed by garbage).
            if (p > 0x10000ull && p < 0x7FFFFFFFFFFFull &&
                vs->BytecodeLength >= 12 && vs->BytecodeLength < (16ull * 1024 * 1024))
            {
                origVS = vs;
            }
        }
    }

    if (!origLayout)
    {
        // No INPUT_LAYOUT — compute PSO or mesh-shader PSO.
        if (g_psoStreamCallCount <= 3)
            DebugLog("[MDI] PSO stream hook (#%u): no INPUT_LAYOUT found, streamSize=%zu\n",
                     g_psoStreamCallCount, streamSize);
        return callOrig(self, pDesc, riid, ppPipelineState);
    }

    if (g_psoStreamCallCount <= 5)
    {
        DebugLog("[MDI] PSO stream hook (#%u): IL@%zu, elems=%u, VS=%s\n",
                 g_psoStreamCallCount, layoutOffset, origLayout->NumElements,
                 origVS ? "found" : "not found");
        for (UINT i = 0; i < origLayout->NumElements && i < 16; ++i)
        {
            const auto& e = origLayout->pInputElementDescs[i];
            DebugLog("[MDI]   [%u] %s%u slot=%u fmt=%u class=%d stepRate=%u\n",
                     i, e.SemanticName ? e.SemanticName : "?", e.SemanticIndex,
                     e.InputSlot, e.Format, e.InputSlotClass, e.InstanceDataStepRate);
        }
    }

    // Helper to issue PSO with a modified INPUT_LAYOUT subobject in a copied stream.
    auto issueWithLayout = [&](const std::vector<D3D12_INPUT_ELEMENT_DESC>& elements) -> HRESULT
    {
        std::vector<uint8_t> modifiedStream(stream, stream + streamSize);
        auto* patchedLayout = reinterpret_cast<D3D12_INPUT_LAYOUT_DESC*>(
            modifiedStream.data() + layoutOffset + kAlign);
        patchedLayout->pInputElementDescs = elements.data();
        patchedLayout->NumElements        = static_cast<UINT>(elements.size());

        D3D12_PIPELINE_STATE_STREAM_DESC modifiedDesc;
        modifiedDesc.SizeInBytes                   = pDesc->SizeInBytes;
        modifiedDesc.pPipelineStateSubobjectStream = modifiedStream.data();
        return callOrig(self, &modifiedDesc, riid, ppPipelineState);
    };

    // VS is the defining marker.
    if (!origVS || !VSInputHasTexcoord7(origVS->pShaderBytecode, origVS->BytecodeLength))
    {
        g_psoSkippedCount++;
        return callOrig(self, pDesc, riid, ppPipelineState);
    }

    if (IsTexcoord7Correct(origLayout->pInputElementDescs, origLayout->NumElements))
    {
        g_psoSkippedCount++;
        return callOrig(self, pDesc, riid, ppPipelineState);
    }

    bool ilHasTexcoord7 = HasTexcoord7(origLayout->pInputElementDescs, origLayout->NumElements);
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    if (ilHasTexcoord7)
        BuildInjectedLayout(origLayout->pInputElementDescs, origLayout->NumElements, elements);
    else
        BuildAugmentedLayout(origLayout->pInputElementDescs, origLayout->NumElements, elements);

    HRESULT hr = issueWithLayout(elements);
    if (ilHasTexcoord7) g_psoInjectedCount++; else g_psoAddedCount++;
    if ((g_psoInjectedCount + g_psoAddedCount) <= 8)
        DebugLog("[MDI] PSO stream hook: %s per-instance TEXCOORD7 on slot %u, "
                 "elements %u -> %u, hr=0x%08X\n",
                 ilHasTexcoord7 ? "REPLACED" : "ADDED",
                 kInstanceVBSlot, origLayout->NumElements, (UINT)elements.size(), hr);
    return hr;
}

// -----------------------------------------------------------------------
// Hook: ID3D12PipelineLibrary::LoadGraphicsPipeline (vtable[9])
// Unity may use pipeline libraries to cache PSOs, bypassing Create*PSO.
// -----------------------------------------------------------------------

static HRESULT STDMETHODCALLTYPE Hook_LoadGraphicsPipeline(
    void* self,   // ID3D12PipelineLibrary*
    LPCWSTR pName,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
    REFIID riid,
    void** ppPipelineState)
{
    g_loadGfxPipelineCallCount++;

    auto callOrig = reinterpret_cast<PFN_LoadGraphicsPipeline>(g_hookLoadGfxPipeline.trampoline);

    if (!pDesc)
        return callOrig(self, pName, pDesc, riid, ppPipelineState);

    const auto& il = pDesc->InputLayout;

    // VS is the defining marker.
    if (!VSInputHasTexcoord7(pDesc->VS.pShaderBytecode, pDesc->VS.BytecodeLength))
    {
        g_psoSkippedCount++;
        return callOrig(self, pName, pDesc, riid, ppPipelineState);
    }
    if (IsTexcoord7Correct(il.pInputElementDescs, il.NumElements))
    {
        g_psoSkippedCount++;
        return callOrig(self, pName, pDesc, riid, ppPipelineState);
    }

    enum Mode { ModePatch, ModeAdd };
    Mode mode = HasTexcoord7(il.pInputElementDescs, il.NumElements) ? ModePatch : ModeAdd;

    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    if (mode == ModePatch)
        BuildInjectedLayout(il.pInputElementDescs, il.NumElements, elements);
    else
        BuildAugmentedLayout(il.pInputElementDescs, il.NumElements, elements);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC modifiedDesc = *pDesc;
    modifiedDesc.InputLayout.pInputElementDescs = elements.data();
    modifiedDesc.InputLayout.NumElements        = static_cast<UINT>(elements.size());

    // Try loading with modified desc — will likely miss cache (different layout).
    // On cache miss, Unity falls back to CreateGraphicsPipelineState which our other hook catches.
    HRESULT hr = callOrig(self, pName, &modifiedDesc, riid, ppPipelineState);

    if (FAILED(hr))
    {
        if (g_loadGfxPipelineCallCount <= 5)
            DebugLog("[MDI] LoadGraphicsPipeline cache miss (modified desc), hr=0x%08X\n", hr);
        return callOrig(self, pName, pDesc, riid, ppPipelineState);
    }

    if (mode == ModePatch) g_psoInjectedCount++; else g_psoAddedCount++;
    if ((g_psoInjectedCount + g_psoAddedCount) <= 5)
        DebugLog("[MDI] LoadGraphicsPipeline: %s TEXCOORD7, elements=%u, hr=0x%08X\n",
                 mode == ModePatch ? "patched" : "added", (unsigned)elements.size(), hr);
    return hr;
}

// -----------------------------------------------------------------------
// Hook: ID3D12Device1::CreatePipelineLibrary (vtable[44])
// Intercept to hook LoadGraphicsPipeline on the returned library object.
// -----------------------------------------------------------------------

static HRESULT STDMETHODCALLTYPE Hook_CreatePipelineLibrary(
    ID3D12Device* self,
    const void* pLibraryBlob,
    SIZE_T blobLength,
    REFIID riid,
    void** ppPipelineLibrary)
{
    g_pipelineLibCallCount++;

    auto callOrig = reinterpret_cast<PFN_CreatePipelineLibrary>(g_hookPipelineLib.trampoline);
    HRESULT hr = callOrig(self, pLibraryBlob, blobLength, riid, ppPipelineLibrary);

    if (SUCCEEDED(hr) && ppPipelineLibrary && *ppPipelineLibrary)
    {
        DebugLog("[MDI] CreatePipelineLibrary succeeded (#%u), library=%p, blobLen=%zu\n",
                 g_pipelineLibCallCount, *ppPipelineLibrary, blobLength);

        // Hook LoadGraphicsPipeline on the returned library object
        if (!g_hookLoadGfxPipeline.target)
        {
            void** libVtable = *reinterpret_cast<void***>(*ppPipelineLibrary);
            void* fnLoadGfx = libVtable[9];  // LoadGraphicsPipeline
            DebugLog("[MDI] PipelineLibrary vtable=%p, LoadGraphicsPipeline=%p\n",
                     libVtable, fnLoadGfx);

            bool hooked = InstallInlineHook(
                fnLoadGfx,
                reinterpret_cast<void*>(&Hook_LoadGraphicsPipeline),
                g_hookLoadGfxPipeline);
            DebugLog("[MDI] LoadGraphicsPipeline inline hook: %d\n", hooked);
        }
    }
    else if (g_pipelineLibCallCount <= 3)
    {
        DebugLog("[MDI] CreatePipelineLibrary failed (#%u), hr=0x%08X\n",
                 g_pipelineLibCallCount, hr);
    }

    return hr;
}

// -----------------------------------------------------------------------
// Initialize / Shutdown
// -----------------------------------------------------------------------

bool MDIBackend_D3D12::Initialize(IUnityInterfaces* unityInterfaces)
{
    _d3d12 = unityInterfaces->Get<IUnityGraphicsD3D12v7>();
    if (!_d3d12)
    {
        DebugLog("[MDI] D3D12 v7 interface not available\n");
        return false;
    }

    // Optional: v8 adds RequestResourceState/NotifyResourceState, which route
    // our INDIRECT_ARGUMENT transitions through Unity's own state tracker
    // (Unity knows the actual current state; we don't). Without it, ExecuteMDI
    // falls back to assuming the buffers arrive in UAV state.
    _d3d12v8 = unityInterfaces->Get<IUnityGraphicsD3D12v8>();
    DebugLog("[MDI] D3D12 v8 interface: %s\n",
             _d3d12v8 ? "available (tracked resource states)"
                      : "NOT available (assuming UAV state for args/count buffers)");

    _device = _d3d12->GetDevice();
    if (!_device) return false;

    // Basic command signature: DrawIndexed only (no root signature needed)
    D3D12_INDIRECT_ARGUMENT_DESC argDesc = {};
    argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
    sigDesc.ByteStride       = 20;
    sigDesc.NumArgumentDescs = 1;
    sigDesc.pArgumentDescs   = &argDesc;
    sigDesc.NodeMask         = 0;

    HRESULT hr = _device->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&_cmdSignature));
    if (FAILED(hr))
    {
        DebugLog("[MDI] CreateCommandSignature failed: 0x%08X\n", hr);
        return false;
    }

    // PSO hooks must be installed early — Unity compiles many PSOs (including
    // those for user meshes with our MDI shader) at scene load, BEFORE the
    // first C# MDI draw call. A lazy/opt-in install would miss those PSOs,
    // and any PSO whose VS expects TEXCOORD7 but whose IL doesn't carry it
    // would fail D3D12 validation. Safety comes from the strict UINT32 /
    // R32_UINT match in VSInputHasTexcoord7 / HasTexcoord7 below — only our
    // own MDI_INSTANCE_ID_PARAMETER shaders are affected.
    InstallDeviceHook();
    CreateInstanceIDBuffer();

    _initialized = true;
    DebugLog("[MDI] D3D12 backend initialized (PSO hooks + per-instance VB)\n");
    return true;
}

void MDIBackend_D3D12::InstallDeviceHook()
{
    if (g_deviceHooked || !_device) return;

    // Get function pointers from vtable (Unity caches these, so vtable
    // patching alone doesn't work — we need inline hooks on the actual code)
    void** vtable = *reinterpret_cast<void***>(_device);
    auto fnLegacy = reinterpret_cast<void*>(vtable[10]);  // CreateGraphicsPipelineState
    auto fnStream = reinterpret_cast<void*>(vtable[47]);  // ID3D12Device2::CreatePipelineState

    DebugLog("[MDI] Device %p, vtable %p\n", _device, vtable);
    DebugLog("[MDI] CreateGraphicsPipelineState = %p\n", fnLegacy);
    DebugLog("[MDI] CreatePipelineState = %p\n", fnStream);

    // Also hook CreatePipelineLibrary (vtable[44]) to intercept PSO caching
    auto fnPipelineLib = reinterpret_cast<void*>(vtable[44]);  // ID3D12Device1::CreatePipelineLibrary
    DebugLog("[MDI] CreatePipelineLibrary = %p\n", fnPipelineLib);

    // Install inline hooks (patch actual function code)
    bool hookedLegacy = InstallInlineHook(
        fnLegacy,
        reinterpret_cast<void*>(&Hook_CreateGraphicsPipelineState),
        g_hookLegacy);
    g_origCreateGraphicsPSO = reinterpret_cast<PFN_CreateGraphicsPipelineState>(
        g_hookLegacy.trampoline);

    bool hookedStream = InstallInlineHook(
        fnStream,
        reinterpret_cast<void*>(&Hook_CreatePipelineState),
        g_hookStream);
    g_origCreatePipelineState = reinterpret_cast<PFN_CreatePipelineState>(
        g_hookStream.trampoline);

    bool hookedPipelineLib = InstallInlineHook(
        fnPipelineLib,
        reinterpret_cast<void*>(&Hook_CreatePipelineLibrary),
        g_hookPipelineLib);

    g_deviceHooked = true;
    DebugLog("[MDI] Inline hooks: legacy=%d, stream=%d, pipelineLib=%d\n",
             hookedLegacy, hookedStream, hookedPipelineLib);
}

void MDIBackend_D3D12::CreateInstanceIDBuffer()
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    uint64_t bufferSize = static_cast<uint64_t>(_maxInstanceCount) * sizeof(uint32_t);

    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Width            = bufferSize;
    resDesc.Height           = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels        = 1;
    resDesc.SampleDesc.Count = 1;
    resDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = _device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&_instanceIDBuffer));

    if (FAILED(hr))
    {
        DebugLog("[MDI] Failed to create instance ID buffer: 0x%08X\n", hr);
        return;
    }

    void* mapped = nullptr;
    hr = _instanceIDBuffer->Map(0, nullptr, &mapped);
    if (SUCCEEDED(hr))
    {
        auto* data = static_cast<uint32_t*>(mapped);
        for (uint32_t i = 0; i < _maxInstanceCount; ++i)
            data[i] = i;
        _instanceIDBuffer->Unmap(0, nullptr);
        DebugLog("[MDI] Instance ID buffer ready: %u entries, %llu bytes\n",
                 _maxInstanceCount, static_cast<unsigned long long>(bufferSize));
    }
}

bool MDIBackend_D3D12::ResizeInstanceIDBuffer(uint32_t newMaxCount)
{
    if (newMaxCount == 0) return false;
    if (newMaxCount == _maxInstanceCount && _instanceIDBuffer) return true;

    if (_instanceIDBuffer) { _instanceIDBuffer->Release(); _instanceIDBuffer = nullptr; }

    _maxInstanceCount = newMaxCount;
    CreateInstanceIDBuffer();
    return _instanceIDBuffer != nullptr;
}

// -----------------------------------------------------------------------
// ConfigureEvents
// -----------------------------------------------------------------------

void MDIBackend_D3D12::ConfigureEvents(IUnityInterfaces* unityInterfaces, int baseEventID, int count)
{
    auto* d3d12 = unityInterfaces->Get<IUnityGraphicsD3D12v7>();
    if (!d3d12) return;

    for (int i = 0; i < count; ++i)
    {
        UnityD3D12PluginEventConfig config = {};
        config.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_DontCare;
        // We change IB / VB slot 15 / topology inside ExecuteMDI on Unity's
        // command list. Without ModifiesCommandBuffersState, Unity's internal
        // state-tracking goes stale after our event and subsequent draws
        // (notably Editor IMGUI) inherit our state and misrender.
        config.flags = kUnityD3D12EventConfigFlag_ModifiesCommandBuffersState;
        // Make Unity (re)bind the active RT before our event so we always
        // execute against a valid render target — important on the first
        // frames in Editor when RT state hasn't fully settled yet.
        config.ensureActiveRenderTextureIsBound = true;
        d3d12->ConfigureEvent(baseEventID + i, &config);
    }

    DebugLog("[MDI] Configured D3D12 events [%d .. %d)\n", baseEventID, baseEventID + count);
}

// -----------------------------------------------------------------------
// Shutdown
// -----------------------------------------------------------------------

void MDIBackend_D3D12::Shutdown()
{
    if (g_deviceHooked)
    {
        RemoveInlineHook(g_hookLegacy);
        RemoveInlineHook(g_hookStream);
        RemoveInlineHook(g_hookPipelineLib);
        RemoveInlineHook(g_hookLoadGfxPipeline);
        g_origCreateGraphicsPSO = nullptr;
        g_origCreatePipelineState = nullptr;
        g_deviceHooked = false;
    }

    if (_instanceIDBuffer) { _instanceIDBuffer->Release(); _instanceIDBuffer = nullptr; }
    if (_cmdSignature) { _cmdSignature->Release(); _cmdSignature = nullptr; }

    _device      = nullptr;
    _d3d12       = nullptr;
    _d3d12v8     = nullptr;
    _initialized = false;

    if (g_d3dCompilerModule)
    {
        FreeLibrary(g_d3dCompilerModule);
        g_d3dCompilerModule = nullptr;
    }
    g_D3DReflect = nullptr;
    g_d3dCompilerAttempted = false;

    DebugLog("[MDI] D3D12 backend shutdown\n");
}

// -----------------------------------------------------------------------
// ExecuteMDI
// -----------------------------------------------------------------------

void MDIBackend_D3D12::ExecuteMDI(const MDIParams& params)
{
    if (!_initialized || !params.argsBuffer || params.maxDrawCount == 0)
        return;

    UnityGraphicsD3D12RecordingState recordingState = {};
    if (!_d3d12->CommandRecordingState(&recordingState))
        return;

    ID3D12GraphicsCommandList* cmdList = recordingState.commandList;
    if (!cmdList) return;

    auto* argsResource = static_cast<ID3D12Resource*>(params.argsBuffer);

    // Rebind caller's index buffer (prime DrawMesh sets its own 3-index IB)
    if (params.indexBuffer)
    {
        auto* indexResource = static_cast<ID3D12Resource*>(params.indexBuffer);
        D3D12_INDEX_BUFFER_VIEW ibView = {};
        ibView.BufferLocation = indexResource->GetGPUVirtualAddress();
        ibView.SizeInBytes    = static_cast<UINT>(indexResource->GetDesc().Width);
        ibView.Format         = (params.indexFormat == 0) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        cmdList->IASetIndexBuffer(&ibView);
    }

    // Bind per-instance identity VB to slot 15.
    //
    // NOTE (not fixed here): what the VS receives in TEXCOORD7 is the IA
    // instance index, i.e. StartInstanceLocation + instanceIndex of the
    // sub-draw — NOT the index of the draw argument within argsBuffer. There
    // is no INCREMENTING_CONSTANT in _cmdSignature because that argument type
    // requires the command signature to be created with an ID3D12RootSignature
    // (and a matching root constant slot), which Unity owns and we don't have.
    //
    // If a shader needs the argument index, the compute shader that fills
    // argsBuffer can write StartInstanceLocation = drawIndex (with
    // InstanceCount = 1); the IA then fetches element drawIndex from this
    // buffer and TEXCOORD7 becomes the argument index — no command signature
    // or root signature change required. That also makes _maxInstanceCount
    // the upper bound on draw count, so ResizeInstanceIDBuffer would need to
    // be driven by params.maxDrawCount.
    if (_instanceIDBuffer)
    {
        D3D12_VERTEX_BUFFER_VIEW vbView = {};
        vbView.BufferLocation = _instanceIDBuffer->GetGPUVirtualAddress();
        vbView.SizeInBytes    = _maxInstanceCount * sizeof(uint32_t);
        vbView.StrideInBytes  = sizeof(uint32_t);
        cmdList->IASetVertexBuffers(kInstanceVBSlot, 1, &vbView);
    }

    switch (params.topology)
    {
        // MeshTopology.Triangles
        default:
        case 0:
            cmdList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            break;
            // MeshTopology.Lines
        case 3:
            cmdList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            break;
        case 4:
            cmdList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
            break;
        case 5:
            cmdList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            break;
    }

    // Single ExecuteIndirect — true multi-draw indirect. With MDI_FLAG_GPU_COUNT
    // the GPU reads the actual draw count from the count buffer (maxDrawCount
    // acts as the upper bound, per the D3D12 count-buffer contract).
    ID3D12Resource* countResource = nullptr;
    if ((params.flags & MDI_FLAG_GPU_COUNT) && params.countBuffer)
        countResource = static_cast<ID3D12Resource*>(params.countBuffer);

    // -------------------------------------------------------------------
    // Get args and count buffers into INDIRECT_ARGUMENT state.
    //
    // Preferred path (v8): RequestResourceState — Unity's state tracker
    // knows the actual current state (UAV after a culling compute, COPY_DEST
    // after SetData, ...), inserts the right barrier into the active command
    // list, and keeps tracking consistent for whatever Unity records next.
    //
    // Fallback path (no v8): we cannot query the real state, so assume the
    // common case for this feature — a compute shader just wrote the buffers,
    // i.e. UAV. To stay valid across repeated MDI draws in one frame and to
    // keep Unity's (unaware) tracker matching reality, we transition back to
    // UAV after the ExecuteIndirect.
    // -------------------------------------------------------------------
    if (_d3d12v8)
    {
        if (argsResource)
            _d3d12v8->RequestResourceState(argsResource, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        if (countResource)
            _d3d12v8->RequestResourceState(countResource, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    else
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        UINT barrierCount = 0;

        if (argsResource)
        {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[barrierCount].Transition.pResource = argsResource;
            barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            barrierCount++;
        }

        if (countResource)
        {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[barrierCount].Transition.pResource = countResource;
            barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            barrierCount++;
        }

        if (barrierCount > 0)
            cmdList->ResourceBarrier(barrierCount, barriers);
    }

    cmdList->ExecuteIndirect(
        _cmdSignature,
        params.maxDrawCount,
        argsResource,
        params.argsOffsetBytes,
        countResource,
        countResource ? params.countOffsetBytes : 0);

    if (!_d3d12v8)
    {
        // Fallback only: restore the assumed UAV state (see comment above).
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        UINT barrierCount = 0;

        if (argsResource)
        {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[barrierCount].Transition.pResource = argsResource;
            barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrierCount++;
        }

        if (countResource)
        {
            barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[barrierCount].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barriers[barrierCount].Transition.pResource = countResource;
            barriers[barrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrierCount++;
        }

        if (barrierCount > 0)
            cmdList->ResourceBarrier(barrierCount, barriers);
    }

    static uint32_t s_callCount = 0;
    s_callCount++;

    if (s_callCount == 1 || s_callCount == 10 || s_callCount == 100 ||
        (s_callCount % 1000) == 0)
    {
        DebugLog("[MDI] ExecuteMDI #%u: drawCount=%u, offset=%u\n",
                 s_callCount, params.maxDrawCount, params.argsOffsetBytes);
        DebugLog("[MDI] Hook stats: legacy=%u, stream=%u, pipelineLib=%u, "
                 "loadGfx=%u, patched=%u, added=%u, skipped=%u\n",
                 g_psoLegacyCallCount, g_psoStreamCallCount,
                 g_pipelineLibCallCount, g_loadGfxPipelineCallCount,
                 g_psoInjectedCount, g_psoAddedCount, g_psoSkippedCount);
    }
}

bool MDIBackend_D3D12::IsSupported() const
{
    return _initialized && _cmdSignature != nullptr;
}

#endif // _WIN32
