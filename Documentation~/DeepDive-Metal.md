# Deep Dive: Metal

[← Back to README](../README.md)

Metal looked like a dead end at first. Three blockers stack on top of each other:

1. **`[[instance_id]]` does not include `[[base_instance]]`.** Apple's Metal Shading Language is explicit: `[[instance_id]]` is the per-draw counter that resets to 0 on every draw command, regardless of the `baseInstance` baked into the indirect arguments. So a Metal shader has no built-in path to "global instance index across draws".
2. **HLSL has no `SV_StartInstanceLocation` / `SV_BaseInstance`.** Unity's HLSL→MSL translator does not expose `[[base_instance]]` through any standard system value. Trying to declare `SV_StartInstanceLocation` on Metal produces `error X4567: maximum cbuffer exceeded` from Unity's compiler; it simply isn't a recognized semantic.
3. **`CommandBuffer.IssuePluginEventAndData` fires *outside* the active encoder in URP RenderGraph.** On Vulkan and D3D12, plugin events fire while Unity's command buffer/list is still recording, and the plugin gets a live `vkCommandBuffer` / `ID3D12GraphicsCommandList` via `IUnityGraphicsVulkan::CommandRecordingState` / `IUnityGraphicsD3D12v7::CommandRecordingState`. On Metal under URP NativePassCompiler, the situation is different: by the time the plugin event fires, Unity has already called `[encoder endEncoding]`. `IUnityGraphicsMetal::CurrentCommandEncoder()` returns a non-nil pointer that responds to Objective-C metadata selectors (`-respondsToSelector:`, `-conformsToProtocol:`) but segfaults inside the Metal driver on the first draw command, because it's a freshly ended encoder. Worse, even if we created our own encoder via `EndCurrentCommandEncoder` + `CurrentRenderPassDescriptor`, we have no access to the user's compiled `MTLRenderPipelineState`, and on Apple Silicon Metal will `__abort` the process if any draw call is encoded without a PSO bound.

The obvious approach (get the encoder, issue N indirect draws on it) is therefore not viable on Metal in modern Unity. Apple's official `NativeRenderingPlugin` sample sidesteps this by compiling its own PSO with its own shader, which is fine for self-contained drawing but useless when we want to use the user's `Material`.

## The Workaround: Method Swizzling

Instead of trying to inject draws after Unity's prime, the plugin intercepts Unity's prime draw itself, while the encoder is still alive and the user's PSO and resources are bound.

The plugin uses Objective-C runtime method swizzling to replace the implementation of two selectors on the concrete `MTLRenderCommandEncoder` class Unity uses (probed at init time by spinning up a throwaway encoder on Unity's `MetalDevice()`):

1. **`-drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:`** is exactly the Metal selector that `cmd.DrawProceduralIndirect(indexBuffer, ..., bufferWithArgs)` lowers to. The C# wrapper issues a "prime" `DrawProceduralIndirect` against a small dummy args buffer (`instanceCount = 0`, so no pixels) just before calling `IssuePluginEventAndData`. When Unity later replays this command, the swizzled implementation:
   - recognises the prime by pointer-equality with the registered dummy buffer,
   - decodes a per-call slot index from `indirectBufferOffset` (C# encodes it as `slot * 20` to map onto the size of `MTLDrawIndexedPrimitivesIndirectArguments`),
   - reads the real `argsBuffer` / `indexBuffer` / `drawCount` from a pinned C# `NativeArray` registered with the plugin via `MDI_SetParamsRing` (this matters: `g_pending`, populated by the plugin event, would not be ready yet, since the prime fires *before* the event),
   - and calls the original IMP N times on the same encoder, each time pointing at the user's real args at `argsOffset + i * 20`.

   A `thread_local` re-entry flag prevents the inner loop from recursing through the swizzle.

2. **`-setVertexBuffer:offset:atIndex:` and `-setVertexBuffers:offsets:withRange:`** are used to auto-detect the MSL buffer slot of `_MDI_DrawIndex_Buffer`. Unity's HLSL→MSL emitter (HLSLcc) reorders cbuffer / structured-buffer slots based on per-shader compilation order, so a hard-coded `register(bN)` does not survive translation. Instead, the plugin pre-creates a tiny `MTLBuffer` and registers its native pointer (`MDI_SetDrawIndexBuffer`); when Unity binds that buffer, both swizzled selectors fire, the hook compares the buffer pointer, and stores whichever slot Unity assigned. No guessing and no shader-specific tuning; this works for any user material that includes `MDI.hlsl`.

   With the slot known, the inner loop calls `[encoder setVertexBytes:&i length:16 atIndex:slot]` before each indirect draw to push the current draw index inline. The shader reads `_MDI_DrawIndex_Buffer[0]` and gets `i` back. Inline data has no buffer-size limit, so this works for arbitrary draw counts (tested at 200×200 grids = 20,000 draws per call without issues).

The end result is a real GPU-side multi-draw on Metal: one prime CPU call, one live encoder, N indirect draws, with full access to the user's compiled PSO and material resources. The shader then computes `globalInstanceID = _ArgsBuffer[_MDI_DrawIndex_Buffer[0]].startInstance + SV_InstanceID` to recover the correct per-instance index across the batch.
