# Deep Dive: Direct3D 12 & Direct3D 11

[← Back to README](../README.md)

At first glance, MDI on D3D under Unity is blocked twice over: the shader cannot learn which draw command it belongs to, and Unity exposes no way to modify the pipeline state that could fix it. D3D11 adds a third wall — the API has no MDI entry point at all.

## The Instance ID Problem

When issuing multiple draw commands via MDI, each draw has its own `startInstance` and `instanceCount` in the indirect arguments buffer. The shader needs a way to compute the global instance index: `startInstance + SV_InstanceID`.

On Vulkan, this works out of the box — `SV_InstanceID` (mapped to `gl_InstanceIndex` in SPIR-V) already includes the `firstInstance` offset, so it directly represents the global instance index.

On D3D11 and D3D12, however, `SV_InstanceID` always starts from zero regardless of the `startInstance` value in the draw arguments. The GPU does use `startInstance` internally to offset per-instance vertex buffer reads, but it does **not** expose that offset through the instance ID system value. This means the shader has no way to determine which draw command it belongs to, effectively making MDI useless on these APIs without a workaround.

## The D3D11 Challenge

D3D11 does not have a native MDI API at all. NVIDIA provides MDI functionality through their **NvAPI** extension (`NvAPI_D3D11_MultiDrawIndexedInstancedIndirect`), and AMD has an equivalent in AGS (`agsDriverExtensionsDX11_MultiDrawIndexedInstancedIndirect`). However, AGS requires the D3D11 device to be created through `agsDriverExtensionsDX11_CreateDevice` — since Unity creates the device itself, AGS extensions cannot be enabled retroactively. Because of this, the plugin currently supports D3D11 MDI on NVIDIA GPUs only.

Critically, the NvAPI MDI extension **also does not provide** `startInstance` to the shader. This limitation is well-documented — see [Interplay of Light: Experiments in GPU-based Occlusion Culling Part 2](https://interplayoflight.wordpress.com/2018/01/15/experiments-in-gpu-based-occlusion-culling-part-2-multidrawindirect-and-mesh-lodding/comment-page-1/) for a detailed discussion of this exact issue.

## The Solution: Per-Instance Identity Buffer

The approach used by this plugin (inspired by the article above) is to create a **per-instance vertex buffer** — an "identity buffer" — filled with sequential indices `[0, 1, 2, ..., N-1]`. When bound as a per-instance vertex input, the GPU's Input Assembler automatically offsets reads by `StartInstanceLocation`, so instance `i` in draw command `d` reads the value `startInstance_d + i` from the buffer. This value arrives in the shader as `TEXCOORD7` and serves as the global instance index — no args buffer lookup required.

### Limitations of the Identity Buffer

The identity buffer defaults to **65,536 elements**. This means `startInstance + instanceCount` for any single draw command must not exceed this value. The buffer size can be changed at runtime via the `MaxInstanceCount` property.

This limit applies only to APIs that use the identity buffer (D3D11, D3D12, OpenGL Core, OpenGL ES). On Vulkan, Metal, and WebGPU `MaxInstanceCount` returns 0 and has no effect — Vulkan's `gl_InstanceIndex` (and WebGPU's `instance_index`) already includes `startInstance`, and Metal's backend uses a different mechanism (see the [Metal deep dive](DeepDive-Metal.md)).

## The Unity Problem: No Access to Input Layouts

In a typical D3D11/D3D12 application, adding a per-instance vertex buffer is straightforward — you simply modify the input layout (PSO) to include the new element. But Unity does **not** expose any API for modifying Pipeline State Objects, input layouts, or vertex buffer bindings from C# or native plugins.

## The Workaround: Input Layout Hooking

The plugin solves the problem by **intercepting** `ID3D11Device::CreateInputLayout()` (and the equivalent D3D12 PSO creation entry points: `CreateGraphicsPipelineState`, the stream-based `CreatePipelineState`, and `ID3D12PipelineLibrary::LoadGraphicsPipeline`) at the native level using inline function hooks.

**Detection gate.** The hook must affect MDI shaders only — patching an unrelated PSO breaks rendering (notably Editor IMGUI). The defining marker is the vertex shader: `MDI_INSTANCE_ID_PARAMETER` expands to `uint _mdi_globalInstanceID : TEXCOORD7`, a uint scalar with a very specific signature. A secondary marker is used for the indexed path: the prime mesh's input layout always carries `TEXCOORD7` with `DXGI_FORMAT_R32_UINT`, a combination no Unity shader produces. Either marker alone is sufficient to recognise an MDI PSO:

- **D3D12** — VS-side detection via `D3DReflect` (`ComponentType == UINT32`, scalar). DXC reports the input type faithfully, so the strict filter is reliable.
- **D3D11** — Unity often passes a *signature-only* blob (from `D3DGetInputSignatureBlob`) to `CreateInputLayout`. `D3DReflect` rejects those with `E_INVALIDARG` because the `SHEX` chunk is absent. The plugin therefore parses the DXBC `ISGN` / `ISG1` chunk directly to find `TEXCOORD7` in the input signature. The IL-side R32_UINT marker also acts as a fallback for the indexed path: even when FXC's signature reporting is unhelpful, the prime mesh's IL alone triggers the hook.

**Once gated as MDI**, the hook then:

1. **Skips** the PSO if its IL is already correctly configured (slot 15, per-instance, step rate 1, `R32_UINT`) — re-patching a previously-rewritten PSO is a no-op.
2. **Replaces** an existing `TEXCOORD7` IL element with a per-instance one on slot 15. This covers both the indexed prime mesh (which declares `TEXCOORD7` as `R32_UINT` already) and the mesh path with a user mesh that happens to carry `TEXCOORD7` of any format — appending a duplicate semantic would fail `CreateInputLayout` / `CreateGraphicsPipelineState` with `E_INVALIDARG`.
3. **Adds** a new per-instance `TEXCOORD7` element on slot 15 if the IL has no `TEXCOORD7` at all (mesh path with a user mesh that doesn't carry that channel).

Non-MDI PSOs (skybox, post-processing, depth-only, Editor IMGUI, etc.) fall through unchanged.

At draw time, the plugin binds the identity buffer to slot 15. The Input Assembler then automatically loads the correct global index for each instance.

**D3D11 immediate-context state restoration.** Unlike D3D12 where each plugin event uses Unity's freshly-recorded command list, the D3D11 plugin event runs on Unity's immediate context. Unity caches IA shadow-state (bound IB, VB slot 15, primitive topology) and skips redundant `IASet*` calls based on it. To avoid leaking the plugin's `IASet*` changes into subsequent Unity draws (which would manifest as broken Editor IMGUI / UI), the backend saves the relevant IA state via `IAGetIndexBuffer` / `IAGetVertexBuffers` / `IAGetPrimitiveTopology` before binding its own buffers, and restores it after the NvAPI MDI call. `Get*` methods return AddRef'd buffer pointers, which the backend releases after re-setting.

**Triggering PSO recreation (indexed path):** Unity may have cached Pipeline State Objects for the user material before the native plugin is loaded, so the hook would not see them during initial PSO creation. For the indexed (procedural) path the plugin uses `VertexAttributeDescriptor` on the C# side to declare a tiny prime-mesh whose layout includes `TEXCOORD7` as `R32_UINT`. Drawing this prime mesh forces Unity to create a new input layout / PSO that passes through the hook. The mesh path doesn't need this trick — the user mesh's PSO has a different layout from anything Unity could have cached without the plugin, so its creation is always intercepted.
