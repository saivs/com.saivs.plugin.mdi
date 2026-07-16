# Deep Dive: WebGPU — MDI in the Browser Without a Native Plugin

[← Back to README](../README.md)

The Web platform stacks its own set of blockers, different from every other backend:

1. **No native plugin interface for WebGPU.** Unity exposes no `IUnityGraphicsWebGPU` — there is no supported way for a plugin to obtain the `GPUDevice`, the current render pass encoder, or anything else from the WebGPU backend. The renderer enum value is not even public in `IUnityGraphics.h`.
2. **No dynamic libraries.** Web builds are statically linked WebAssembly — there is no `.dll`/`.so` to load. "Native" plugins on the Web are C/C++ compiled by Emscripten into the wasm module, or JavaScript `.jslib` libraries merged into the build.
3. **Multi-draw is not in the WebGPU standard.** `multiDrawIndexedIndirect` exists only as the `chromium-experimental-multi-draw-indirect` feature behind Chrome's *Unsafe WebGPU Support* flag — unusable as a baseline.
4. **`firstInstance` is conditional.** Without the (standard, optional) `indirect-first-instance` device feature, any indirect draw whose args carry a non-zero `firstInstance` is silently discarded. Unity does not request that feature on its own.

## The Insight: Unity's WebGPU Backend Ends in JavaScript

Unity drives WebGPU through JS bindings (`lib_webgpu.js`): every command the engine encodes ultimately calls a method on a browser-side prototype — `GPURenderPassEncoder`, `GPUCommandEncoder`, `GPUAdapter`. A `.jslib` shipped with the package lives in the same Emscripten module scope, has full access to wasm memory (`HEAPU32`) and to lib_webgpu's handle table, and can patch those prototypes before the engine initializes.

This is the same architecture as the [Metal backend](DeepDive-Metal.md) — intercept Unity's own prime draw at the point where all state is bound — with JS prototype patching playing the role of Objective-C method swizzling. The entire backend is one `.jslib`; there is no C++ at all. The jslib itself provides the `MDI_*` entry points (`DllImport("__Internal")` on Web), so the same C# code path works unchanged.

At module load the backend installs four patches:

1. **`GPUAdapter.prototype.requestDevice`** — appends `indirect-first-instance` (and, opportunistically, `chromium-experimental-multi-draw-indirect` when the adapter offers it) to the `requiredFeatures` of the device Unity is about to create. Device features are immutable after creation, and Unity would never request these on its own. As a side effect this also un-breaks Unity's *own* indirect draws with non-zero `firstInstance` on WebGPU.
2. **`GPUTexture.prototype.createView` + `GPUCommandEncoder.prototype.beginRenderPass`** — record each render pass's attachment formats and sample count. `GPUTextureView` exposes no readable format, but render bundles must declare the exact formats of the pass they execute in.
3. **State shadow-tracking** on `setPipeline` / `setBindGroup` / `setIndexBuffer` / `setVertexBuffer` — WebGPU has no state queries, so the backend records what Unity binds on each pass encoder.
4. **`GPURenderPassEncoder.prototype.drawIndexedIndirect`** — the interception point.

## Recognising the Prime, Reading the Params

The C# wrapper is untouched: it records the usual prime draw against the dummy args buffer with `argsOffset = slot * 20`, exactly like the Metal path. When Unity encodes that draw, the patch recognises the dummy buffer — resolved once from `GraphicsBuffer.GetNativeBufferPtr()` through lib_webgpu's handle table, with a size/usage/magic heuristic as fallback — and reads the `MDIParams` for that slot straight out of the pinned C# ring buffer in wasm memory. A magic constant written into the params' padding field validates the slot. The dummy draw itself is dropped (`instanceCount = 0` — it would render nothing anyway).

## Three-Tier Execution

With the real args buffer resolved (again via the handle table), the batch executes through the best available path:

1. **`multiDrawIndexedIndirect`** — if the experimental Chromium feature happens to be enabled: one call, done.
2. **Cached `GPURenderBundle`** — the standard-WebGPU workhorse. The backend builds a render bundle containing the shadow-tracked pass state plus N `drawIndexedIndirect` commands, and replays it with a single `executeBundles()` call. Bundles are pre-validated at record time and replayed natively by the browser's GPU process — per-draw cost drops from "full engine + JS + IPC round trip" to a native command replay. Bundles are cached by a key covering the pipeline, bind groups (with dynamic offsets), index/vertex buffers, args buffer, offset, draw count, and pass formats — for stable scenes each batch records once and replays every frame. Since `executeBundles()` resets render pass state, the tracked state is re-applied afterwards so Unity's subsequent draws are unaffected. Crucially, the bundle references buffer *objects*, not contents: per-frame GPU culling that rewrites `instanceCount`/`startInstance` in the args buffer never invalidates the cache.
3. **Loop fallback** — if bundle validation ever fails, the backend degrades to N direct `drawIndexedIndirect` calls on the live encoder.

On WebGL2 builds (no WebGPU) the same jslib degrades gracefully: `MDI_IsSupported()` returns 0 and the C# wrapper falls back — though note that WebGL2 has neither indirect draws nor compute shaders, so GPU-driven pipelines don't apply there at all.

## Instance ID: Free on WebGPU

WGSL's `instance_index` follows Vulkan semantics — it **includes** `firstInstance` from the indirect args. `MDI_INSTANCE_ID` therefore resolves to plain `SV_InstanceID`, no identity buffer and no args lookup needed. The only requirement is the `indirect-first-instance` feature, which the `requestDevice` patch guarantees whenever the adapter supports it; `MDI_IsSupported()` reports `false` otherwise and the C# wrapper falls back to its managed loop.
