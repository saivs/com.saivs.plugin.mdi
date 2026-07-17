# Unity Multi Draw Indirect Plugin

A native plugin that brings true **Multi-Draw Indirect (MDI)** to Unity.

![MDI Test](MDI_Test.gif)

## What is Multi-Draw Indirect?

A **draw call** is one instruction to the GPU: "render this object with this material". Each one has real CPU cost — the engine has to validate state, build a command, and submit it to the graphics driver.

A "small" scene easily hits **2,000–3,000 draw calls per frame**, even with very few objects on screen. Each visible object isn't drawn once — it's drawn several times per frame: once for the main image, once or twice more for shadows, often again for a depth pre-pass, plus extra passes for transparency and post-processing. Add LODs (different mesh detail at different distances) and the count grows fast.

Even on a fast desktop CPU with Unity's built-in batching, **1,000 draw calls can cost over 1 ms of CPU time per frame**. At 60 fps you have a 16.6 ms budget for *everything* — physics, AI, scripts, rendering — so 1 ms just submitting draws to the GPU is a real problem.

**Multi-Draw Indirect (MDI)** solves this by replacing N draw calls with one. You write the parameters for all N draws into a single GPU buffer, then make **one** call that tells the GPU "look in this buffer and run whatever draws you find". The GPU does the dispatch itself — there's no per-draw CPU overhead. This is the technique modern engines like Unreal's Nanite and Frostbite use to render millions of objects without the CPU collapsing under draw-call cost.

## Why this plugin?

Modern GPU-driven rendering pipelines rely on Multi-Draw Indirect to batch thousands of draw calls into a single GPU command. Unity does **not** expose MDI in any form:

- `Graphics.RenderPrimitivesIndexedIndirect / Graphics.RenderMeshIndirect` is the closest built-in alternative, but it is **not** MDI — it issues individual draw calls on the CPU side and, critically, **cannot be used inside CommandBuffers**, making it unusable in scriptable render pipelines and render graph workflows.
- `CommandBuffer.DrawProceduralIndirect / CommandBuffer.DrawMeshInstancedIndirect` supports only a single indirect draw per call. Issuing it in a loop ("ProceduralIndirect loop") works but scales poorly — each call has full CPU overhead of state validation, command recording, and managed-to-native transitions.

This plugin solves the problem by injecting a single native MDI command directly into Unity's graphics command stream via `IssuePluginEventAndData`, providing **true hardware-level batching** with minimal CPU cost.

## Supported Platforms

| Graphics API | Status | Backend |
|---|---|---|
| D3D11 | ✅ Supported | (Nvidia)NvAPI `DrawIndexedInstancedIndirect` / loop fallback |
| D3D12 | ✅ Supported | `ExecuteIndirect` via `CommandRecordingState` |
| Vulkan | ✅ Supported | `vkCmdDrawIndexedIndirect` (multi-draw or loop fallback) |
| OpenGL Core | ✅ Supported | `glMultiDrawElementsIndirect` |
| OpenGL ES 3.1+ | ✅ Supported | `glMultiDrawElementsIndirect` |
| Metal | ✅ Supported | `drawIndexedPrimitives:indirectBuffer:` via Objective-C method swizzling |
| WebGPU | ✅ Supported | Cached `GPURenderBundle` replay (or `multiDrawIndexedIndirect` where available) via JS prototype interception, pure `.jslib` |

### Operating Systems

The package ships prebuilt binaries for every supported OS — no manual compilation needed:

| OS | Binary | Graphics APIs |
|---|---|---|
| Windows x86_64 | `GfxPluginMDI.dll` | D3D11, D3D12, Vulkan, OpenGL |
| Linux x86_64 | `libGfxPluginMDI.so` | Vulkan, OpenGL |
| macOS (Intel + Apple Silicon) | `GfxPluginMDI.bundle` | Metal |
| Android (arm64-v8a, armeabi-v7a) | `libGfxPluginMDI.so` | Vulkan, OpenGL ES |
| Web | `MDIBackend_WebGPU.jslib` | WebGPU |

The Linux binary targets glibc 2.34+ (Ubuntu 22.04 and newer — Unity 6's Linux baseline) with the C++ runtime linked statically, so it has no dependencies beyond libc. To rebuild it from source, run `NativePlugin~/build_linux.sh` on any Linux machine or container.

## Performance

CPU time comparison for **25,000 draw calls**. D3D11/D3D12/Vulkan/OpenGL ES/WebGPU tested on RTX 3080, AMD Ryzen 9 5950X; Metal tested on Apple M2 Pro (Mac mini).
Measured as total `PlayerLoop` time (not just command submission) in the build, so the numbers include all engine overhead per frame:

### D3D11

| Method | CPU Time |
|---|---|
| **MultiDrawIndirect** | 0.41 ms |
| ProceduralIndirect Loop | 23.77 ms |
| RenderPrimitivesIndexedIndirect | 15.11 ms |

### D3D12

| Method | CPU Time |
|---|---|
| **MultiDrawIndirect** | 0.35 ms |
| ProceduralIndirect Loop | 28.61 ms |
| RenderPrimitivesIndexedIndirect | 36.24 ms |

### Vulkan

| Method | CPU Time |
|---|---|
| **MultiDrawIndirect** | 0.35 ms |
| ProceduralIndirect Loop | 25.06 ms |
| RenderPrimitivesIndexedIndirect | 23.08 ms |

### OpenGLES

| Method | CPU Time |
|---|---|
| **MultiDrawIndirect** | 1.18 ms |
| ProceduralIndirect Loop | 25.7 ms |
| RenderPrimitivesIndexedIndirect | 23.7 ms |

### Metal (Apple M2 Pro, Mac mini)

| Method | CPU Time |
|---|---|
| **MultiDrawIndirect** | 1.52 ms |
| ProceduralIndirect Loop | 23.06 ms |
| RenderPrimitivesIndexedIndirect | 18.43 ms |

### WebGPU

| Method | CPU Time |
|---|---|
| **MultiDrawIndirect** | 5.25 ms |
| ProceduralIndirect Loop | 53.00 ms |
| RenderPrimitivesIndexedIndirect | 31.12 ms |

## Limitations

- **D3D11 + RenderDoc**: The plugin uses NvAPI, which can cause Unity to crash when RenderDoc attempts to inject at runtime. To avoid this, attach RenderDoc **at Unity startup** (launch Unity from RenderDoc) rather than connecting mid-session.
- **D3D11 + AMD GPUs**: D3D11 does not have a native MDI API. On NVIDIA, this is solved via NvAPI, which can attach to an already-created D3D11 device. AMD has an equivalent extension in AGS (`agsDriverExtensionsDX11_MultiDrawIndexedInstancedIndirect`), but AGS requires the D3D11 device to be created through `agsDriverExtensionsDX11_CreateDevice` — since Unity creates the device itself, AGS extensions cannot be enabled retroactively. Because of this (and lack of AMD hardware for testing), MDI on D3D11 + AMD is not currently supported. AMD GPUs are fully supported under D3D12, Vulkan, and OpenGL.
- **Consoles**: The plugin has been tested on desktop Windows, Linux, macOS, Web (WebGPU in Chromium-based browsers), and mobile devices (iOS; Android with Adreno and Mali GPUs). It has not been verified on consoles (PlayStation, Xbox, Switch) — support there is not guaranteed.
- **Mobile hardware multi-draw coverage**: On iOS, Metal indirect draws (the mechanism behind the swizzling backend) require A9 hardware or newer (iPhone 6s, 2015+). On Android, true hardware MDI depends on the GPU's Vulkan `multiDrawIndirect` feature: **Qualcomm Adreno** exposes it, while **ARM Mali** generally does not (only the newest Immortalis-class drivers report it). The same split applies under OpenGL ES: `GL_EXT_multi_draw_indirect` is available on Adreno but not on Mali. On GPUs without the feature nothing breaks — the plugin detects it at init and transparently degrades to a **native loop** (`vkCmdDrawIndexedIndirect` with `drawCount = 1` per command, recorded directly into Unity's command buffer): still one plugin event per batch and far cheaper than the managed C# loop, just not a single hardware command.
- **Identity buffer instance limit (D3D11/D3D12/OpenGL/GLES)**: The per-instance identity buffer defaults to 65,536 entries. For any draw command in an MDI batch, `startInstance + instanceCount` must not exceed this value. Use `MultiDrawIndirect.MaxInstanceCount` to increase or decrease the limit at runtime. This limitation does not apply to Vulkan.

## Installation

Add the package via Unity Package Manager using a git URL:

1. Open **Window > Package Manager**
2. Click **+** > **Add package from git URL...**
3. Enter:
   ```
   https://github.com/saivs/com.saivs.plugin.mdi.git
   ```

## Usage

The plugin exposes two extension methods on `CommandBuffer` (and `RasterCommandBuffer` / `UnsafeCommandBuffer` in Unity 6+): an indexed/procedural form (`MultiDrawIndexedIndirect`) and a Mesh form (`MultiDrawMeshIndirect`). Both run as true single-call MDI on every supported backend.

```csharp
using Saivs.Graphics.Core.MDI;
```

### Identity Buffer
```csharp
// Increase the limit to 1,000,000 instances (D3D11/D3D12/OpenGL)
// See Documentation~/DeepDive-D3D.md — "Per-Instance Identity Buffer"
MultiDrawIndirect.MaxInstanceCount = 1_000_000;
// Query the current limit
uint current = MultiDrawIndirect.MaxInstanceCount;
```

### Mesh — `cmd.MultiDrawMeshIndirect`

Pass a Unity `Mesh` directly. Vertices come through the standard input assembler so the shader can use the regular `POSITION` / `NORMAL` / `TEXCOORD0` / etc.

```csharp
cmd.MultiDrawMeshIndirect(
    mesh:           mesh,
    material:       material,
    properties:     propertyBlock,
    shaderPass:     0,
    bufferWithArgs: argsBuffer,
    argsStartIndex: 0,
    argsCount:      drawCount
);
```
That's it — one call replaces the entire draw loop. When the native plugin is available, all draws are batched into a single MDI command. 

The `argsBuffer` layout is identical to `MultiDrawIndexedIndirect` ([GraphicsBuffer.IndirectDrawIndexedArgs](https://docs.unity3d.com/6000.4/Documentation/ScriptReference/GraphicsBuffer.IndirectDrawIndexedArgs.html)). Each entry's `startIndex` / `baseVertexIndex` directly drives which slice of the mesh's index/vertex buffer is read for that draw — letting you scatter different shapes across the batch by combining several meshes into one and indexing them through args. Multi-submesh meshes aren't addressed via a `submeshIndex` parameter; encode whatever slice you need directly through `startIndex` / `baseVertexIndex` / `indexCountPerInstance`.

Matching vertex shader — vertex data arrives through the standard semantics:

```hlsl
HLSLPROGRAM
#pragma vertex vert
#pragma fragment frag

#include "Packages/com.saivs.multi-draw-indirect/Runtime/ShaderLibrary/MDI.hlsl"

struct Attributes
{
    float4 positionOS : POSITION;
    float3 normalOS   : NORMAL;
};

VertexOutput vert(Attributes input, MDI_INSTANCE_ID_PARAMETER)
{
    uint globalInstanceID = MDI_INSTANCE_ID;
    // ... transform input.positionOS, fetch per-instance data by globalInstanceID, etc.
}
ENDHLSL
```

### Indexed / procedural — `cmd.MultiDrawIndexedIndirect`

The shader pulls vertex data from a `StructuredBuffer` indexed by `SV_VertexID`.

```csharp

cmd.MultiDrawIndexedIndirect(
    indexBuffer:    indexBuffer,
    material:       material,
    properties:     propertyBlock,
    shaderPass:     0,
    topology:       MeshTopology.Triangles,
    bufferWithArgs: argsBuffer,
    argsStartIndex: 0,
    argsCount:      drawCount
);
```

Matching vertex shader — vertex data fetched manually from a `StructuredBuffer` via `SV_VertexID`:

```hlsl
HLSLPROGRAM
#pragma vertex vert
#pragma fragment frag

#include "Packages/com.saivs.multi-draw-indirect/Runtime/ShaderLibrary/MDI.hlsl"

VertexOutput vert(uint vertexID : SV_VertexID, MDI_INSTANCE_ID_PARAMETER)
{
    uint globalInstanceID = MDI_INSTANCE_ID;

    // Use globalInstanceID and vertexID to fetch per-instance data (positions, transforms, etc.)
}
ENDHLSL
```

**Backend support.** True single-call MDI through this API runs on every supported backend (D3D11, D3D12, Vulkan, Metal, WebGPU, OpenGL Core, OpenGL ES). On Metal, Vulkan and WebGPU the user mesh doesn't need a `TEXCOORD7` element — `MDI.hlsl` resolves `MDI_INSTANCE_ID` through `SV_InstanceID`. On D3D11 and D3D12 the native plugin reflects the user shader's vertex bytecode and patches the input layout / PSO at creation time to add a per-instance `TEXCOORD7 → identity buffer` element on slot 15, leaving the user mesh's vertex buffers untouched. On OpenGL / OpenGL ES the plugin clones Unity's mesh VAO into its own VAO and adds the same per-instance `TEXCOORD7` binding, with a small fingerprint-keyed cache so repeated draws of the same mesh skip the cloning. The mesh's `indexBufferTarget` is augmented with `Raw` automatically the first time it's seen, so `mesh.GetIndexBuffer()` returns a buffer the native plugin can address.

### `MDI.hlsl` macros

Both APIs above share two macros that handle cross-platform instance ID resolution automatically:

| Macro | Purpose |
|---|---|
| `MDI_INSTANCE_ID_PARAMETER` | Place in vertex shader signature — expands to the correct platform-specific parameter |
| `MDI_INSTANCE_ID` | Use in vertex shader body — resolves to the global instance index across all draw commands |

The macros expand differently depending on the platform and compile-time defines:

| Platform | `MDI_INSTANCE_ID_PARAMETER` | `MDI_INSTANCE_ID` |
|---|---|---|
| D3D11 / D3D12 / OpenGL / OpenGL ES | `uint : TEXCOORD7` | Identity buffer value (global instance index) |
| Vulkan / WebGPU | `uint : SV_InstanceID` | `SV_InstanceID` (already includes `startInstance`) |
| Metal | `uint : SV_InstanceID` | `_ArgsBuffer[_MDI_DrawIndex_Buffer[0]].startInstance + SV_InstanceID` |
| Fallback loop (`MDI_NATIVE_LOOP`) | `uint : SV_InstanceID` | `_ArgsBuffer[_MDI_DrawIndex].startInstance + SV_InstanceID` |

See the included [sample shader](Samples~/MDITest/Shaders/MDITestShader.shader) for a complete working example with multiple pass configurations.

## Technical Deep Dive

There was no single central problem to solve to bring MDI to every graphics API — each backend hit its own wall, and on several platforms the real issue wasn't a missing feature but that support looked outright **impossible** at first glance: Unity exposes no way to modify pipeline state on D3D, on Metal the command encoder is already dead by the time a plugin event fires, and on the Web there is no native plugin interface for WebGPU at all. Each of those walls turned out to have a way around it — inline hooking, method swizzling, or prototype patching.

One recurring (but not universal) sub-problem is obtaining a correct **global instance index** — one that uniquely identifies each instance across all draw commands in a single MDI batch. Vulkan hands it out for free (`gl_InstanceIndex` includes `firstInstance`), WebGPU follows the same semantics, and D3D11/D3D12/OpenGL zero out `SV_InstanceID` on every draw command, each needing its own workaround.

The full story for each backend:

| Backend | The wall | The way around |
|---|---|---|
| [Direct3D 12 & 11](Documentation~/DeepDive-D3D.md) | `SV_InstanceID` ignores `startInstance`; Unity gives no access to PSOs / input layouts; D3D11 has no MDI API at all | Per-instance identity buffer + inline-hooking PSO creation; NvAPI on D3D11 |
| [OpenGL Core & ES](Documentation~/DeepDive-OpenGL.md) | `gl_InstanceID` doesn't include `baseInstance`; touching Unity's VAO breaks its state cache | Identity buffer on a dedicated / cloned VAO with a fingerprint-keyed cache |
| [Metal](Documentation~/DeepDive-Metal.md) | Encoder already ended when plugin events fire; no `[[base_instance]]` through HLSL; encoding a draw without a PSO aborts the process | Objective-C method swizzling of Unity's own prime draw |
| [WebGPU](Documentation~/DeepDive-WebGPU.md) | No native plugin interface; no dynamic libraries; multi-draw not in the WebGPU standard | JS prototype patching + cached `GPURenderBundle` replay, pure `.jslib` |
