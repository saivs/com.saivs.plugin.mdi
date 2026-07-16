# Deep Dive: OpenGL Core & OpenGL ES

[← Back to README](../README.md)

On OpenGL, `gl_InstanceID` does **not** include `baseInstance` — this is a common misconception, as Vulkan's `gl_InstanceIndex` does. The GPU uses `baseInstance` internally to offset per-instance vertex buffer reads, but never exposes it through the instance ID system value, so the shader alone cannot tell which draw command it belongs to.

The solution is the same **per-instance identity buffer** approach as on D3D (fully explained in the [D3D deep dive](DeepDive-D3D.md)): a per-instance vertex attribute filled with `[0, 1, 2, ..., N-1]` that the vertex fetch stage automatically offsets by `baseInstance`, delivering the global instance index through `TEXCOORD7`. The identity buffer's `MaxInstanceCount` limit applies here as well.

The implementation, however, is simpler than D3D11/D3D12: OpenGL's vertex attribute state is dynamic, so no PSO hooking is needed.

## Indexed (procedural) path

The shader pulls vertex data via `SV_VertexID`, so a minimal VAO is enough. The plugin owns a dedicated `_mdiVAO`, binds the caller's index buffer and the identity buffer (to the `TEXCOORD7` attribute location resolved via `glGetAttribLocation` and bound with `glVertexAttribIPointer` + `glVertexAttribDivisor(location, 1)`), draws, and restores Unity's previous VAO. The attribute location and VAO are cached across frames; the VAO is validated via `glIsVertexArray` before reuse to handle Unity's implicit GL context resets (e.g. window maximize/detach).

## Mesh path

Modifying `TEXCOORD7` directly on Unity's mesh VAO is unsafe — it's recorded VAO state, persists across draws, and falls out of sync with Unity's state cache, breaking subsequent rendering of the same mesh. Instead, the plugin **clones** Unity's VAO into its own VAO on first use: it snapshots every enabled vertex attribute slot (size, stride, type, normalized, integer flag, divisor, buffer binding, pointer) and the `GL_ELEMENT_ARRAY_BUFFER` binding, replays them onto a clone VAO, and adds the per-instance `TEXCOORD7 → identity buffer` binding only on the clone. Unity's original VAO is left untouched.

Cloning is expensive (~80–130 GL calls per draw), so the plugin keeps a small fingerprint-keyed LRU cache of clones (4 entries by default). The fingerprint covers `(unityVAO ID, GL_CURRENT_PROGRAM, slot 0 buffer/stride/pointer, ELEMENT_ARRAY_BUFFER, identity buffer ID)` — enough to detect mesh re-uploads, shader switches, mesh recreation, and identity buffer resizes. On a cache hit the hot path is just the fingerprint queries plus a single `glBindVertexArray`. The cache is also explicitly invalidated when `MaxInstanceCount` is changed at runtime (since cached clones reference the old identity buffer).
