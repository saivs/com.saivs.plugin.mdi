using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace Saivs.Graphics.Core.MDI
{
    // CommandBuffer.MultiDrawIndexedIndirect — vertex data is fetched manually
    // from a StructuredBuffer indexed by SV_VertexID; the user supplies the
    // index buffer explicitly. Supported on every backend.
    public static partial class MultiDrawIndirect
    {
        // -----------------------------------------------------------------------
        // Indexed-only state: prime mesh dictionary used on D3D11/D3D12 to force
        // a PSO whose input layout includes TEXCOORD7 (the per-instance identity
        // buffer). Not needed on backends that resolve the global instance ID
        // through SV_InstanceID.
        // -----------------------------------------------------------------------
        private static bool _usesPerInstanceVB;
        private static bool _perInstanceVBChecked;
        private static Dictionary<MeshTopology, Mesh> _primeMeshes;

        private static bool UsesPerInstanceVB
        {
            get
            {
                if (!_perInstanceVBChecked)
                {
                    _perInstanceVBChecked = true;
                    try { _usesPerInstanceVB = _supported && MDI_UsesPerInstanceVB() != 0; }
                    catch { _usesPerInstanceVB = false; }
                }
                return _usesPerInstanceVB;
            }
        }

        static partial void InitIndexedState()
        {
            _primeMeshes = new Dictionary<MeshTopology, Mesh>();
        }

        static partial void DisposeIndexedState()
        {
            if (_primeMeshes != null)
            {
                foreach (var pair in _primeMeshes)
                    UnityEngine.Object.DestroyImmediate(pair.Value);
                _primeMeshes.Clear();
            }
            _perInstanceVBChecked = false;
            _usesPerInstanceVB = false;
        }

        // Build (and cache) a minimal mesh whose vertex layout includes TEXCOORD7.
        // When Unity renders this mesh, it creates a PSO with TEXCOORD7 in the
        // input layout — the native hook then patches it to be per-instance on
        // VB slot 15 and binds the identity buffer there.
        private static Mesh GetPrimeMesh(MeshTopology topology)
        {
            if (_primeMeshes.TryGetValue(topology, out var mesh))
                return mesh;

            mesh = new Mesh
            {
                name = $"MDI_PrimeMesh_{topology}",
                hideFlags = HideFlags.HideAndDontSave,
            };

            mesh.SetVertexBufferParams(3,
                new VertexAttributeDescriptor(VertexAttribute.Position, VertexAttributeFormat.Float32, 3, stream: 0),
                new VertexAttributeDescriptor(VertexAttribute.TexCoord7, VertexAttributeFormat.UInt32, 1, stream: 1)
            );

            mesh.SetVertexBufferData(new Vector3[3], 0, 0, 3, stream: 0);
            mesh.SetVertexBufferData(new uint[3], 0, 0, 3, stream: 1);

            switch (topology)
            {
                case MeshTopology.Lines:
                    mesh.SetIndices(new int[] { 0, 1 }, MeshTopology.Lines, 0);
                    break;
                case MeshTopology.Points:
                    mesh.SetIndices(new int[] { 0 }, MeshTopology.Points, 0);
                    break;
                case MeshTopology.Triangles:
                default:
                    mesh.SetIndices(new int[] { 0, 1, 2 }, MeshTopology.Triangles, 0);
                    break;
            }

            mesh.bounds = new Bounds(Vector3.zero, Vector3.one * 10000);

            _primeMeshes[topology] = mesh;
            return mesh;
        }

        // -----------------------------------------------------------------------
        // CommandBuffer extension
        // -----------------------------------------------------------------------
        public static void MultiDrawIndexedIndirect(
            this CommandBuffer cmd,
            GraphicsBuffer indexBuffer,
            Material material,
            MaterialPropertyBlock properties,
            int shaderPass,
            MeshTopology topology,
            GraphicsBuffer bufferWithArgs,
            int argsStartIndex,
            int argsCount)
        {
            EnsureInitialized();

            if (_supported && argsCount > 1)
            {
                // Stage params into the ring buffer FIRST so we know which slot
                // this draw owns. The slot is encoded in the prime's argsOffset
                // — the Metal backend reads it back from inside its
                // drawIndexedPrimitives swizzle hook.
                IntPtr dataPtr = WriteParams(bufferWithArgs, indexBuffer, argsStartIndex, argsCount, topology, indexFormat: 1, flags: 0, out int slot);

                // Prime draw: sets PSO + render state on the command list.
                // On D3D11/D3D12, use a Mesh with TEXCOORD7 to force a PSO whose
                // input layout includes TEXCOORD7 (our hook patches it to
                // per-instance on VB slot 15). Zero-area triangle = no pixels.
                if (UsesPerInstanceVB)
                    cmd.DrawMesh(GetPrimeMesh(topology), Matrix4x4.identity, material, 0, shaderPass, properties);
                else
                    cmd.DrawProceduralIndirect(
                        indexBuffer: indexBuffer, matrix: Matrix4x4.identity, material: material,
                        shaderPass: shaderPass, topology: topology, bufferWithArgs: _dummyArgsBuffer,
                        argsOffset: slot * INDIRECT_DRAW_INDEXED_ARGS_SIZE, properties: properties);

                // WebGPU: no native render-event callback — the jslib backend
                // intercepts the prime draw itself and reads the ring directly.
                if (_renderEventAndDataFunc != IntPtr.Zero)
                    cmd.IssuePluginEventAndData(_renderEventAndDataFunc, _baseEventID + slot, dataPtr);
            }
            else
            {
                for (int i = 0; i < argsCount; i++)
                {
                    cmd.DrawProceduralIndirect(
                        indexBuffer: indexBuffer, matrix: Matrix4x4.identity, material: material,
                        shaderPass: shaderPass, topology: topology, bufferWithArgs: bufferWithArgs,
                        argsOffset: (argsStartIndex + i) * INDIRECT_DRAW_INDEXED_ARGS_SIZE, properties: properties);
                }
            }
        }

        public static void MultiDrawIndexedIndirect(
            this CommandBuffer cmd,
            GraphicsBuffer indexBuffer,
            Material material,
            MaterialPropertyBlock properties,
            int shaderPass,
            MeshTopology topology,
            GraphicsBuffer bufferWithArgs,
            int argsStartIndex,
            GraphicsBuffer drawCountBuffer, // Buffer updated dynamically by GPU compute
            int drawCountStartIndex, // Byte offset inside count buffer (usually 0)
            int maxDrawCount // Fallback upper-bound limit for the hardware loop
        )
        {
            EnsureInitialized();

            if (_supported && maxDrawCount > 0)
            {
                // Same flow as the CPU-count overload: stage params (including
                // the GPU count buffer) into the ring, then issue the prime
                // draw + plugin event. Backends with native count support
                // (D3D12 ExecuteIndirect, vkCmdDrawIndexedIndirectCount,
                // glMultiDrawElementsIndirectCount, WebGPU multi-draw) read the
                // draw count from the GPU buffer; backends without it execute
                // maxDrawCount draws — unused args entries must then be zeroed
                // (instanceCount = 0) by the compute shader.
                IntPtr dataPtr = WriteParams(bufferWithArgs, indexBuffer, argsStartIndex, maxDrawCount, topology, indexFormat: 1, flags: MDI_FLAG_GPU_COUNT, out int slot,
                    countBuffer: drawCountBuffer, countOffsetBytes: drawCountStartIndex);

                if (UsesPerInstanceVB)
                    cmd.DrawMesh(GetPrimeMesh(topology), Matrix4x4.identity, material, 0, shaderPass, properties);
                else
                    cmd.DrawProceduralIndirect(
                        indexBuffer: indexBuffer, matrix: Matrix4x4.identity, material: material,
                        shaderPass: shaderPass, topology: topology, bufferWithArgs: _dummyArgsBuffer,
                        argsOffset: slot * INDIRECT_DRAW_INDEXED_ARGS_SIZE, properties: properties);

                if (_renderEventAndDataFunc != IntPtr.Zero)
                    cmd.IssuePluginEventAndData(_renderEventAndDataFunc, _baseEventID + slot, dataPtr);
            }
            else
            {
                // Fallback: the GPU count can't be read without native support —
                // draw the upper bound; unused args entries must have
                // instanceCount = 0 so the extra draws produce nothing.
                for (int i = 0; i < maxDrawCount; i++)
                {
                    cmd.DrawProceduralIndirect(
                        indexBuffer: indexBuffer, matrix: Matrix4x4.identity, material: material,
                        shaderPass: shaderPass, topology: topology, bufferWithArgs: bufferWithArgs,
                        argsOffset: (argsStartIndex + i) * INDIRECT_DRAW_INDEXED_ARGS_SIZE, properties: properties);
                }
            }
        }

    }
}
