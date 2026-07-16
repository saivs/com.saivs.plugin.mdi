using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace Saivs.Graphics.Core.MDI
{
    // CommandBuffer.MultiDrawMeshIndirect — vertex data comes from the user's
    // Mesh through the input assembler, so the shader can use the standard
    // POSITION / NORMAL / TEXCOORD0 semantics. True MDI is routed to the
    // native plugin on every supported backend:
    //   • Metal / Vulkan / WebGPU — MDI.hlsl resolves the global instance ID
    //     through SV_InstanceID, so the user mesh doesn't need TEXCOORD7.
    //   • D3D11 / D3D12 — the native CreateInputLayout / CreateGraphicsPipelineState
    //     hooks reflect the VS bytecode and append a per-instance TEXCOORD7
    //     element on slot 15 when the user shader declares MDI_INSTANCE_ID_PARAMETER,
    //     so the user mesh also doesn't need to carry TEXCOORD7.
    //   • OpenGL ES / OpenGL Core — the native plugin uses Unity's mesh VAO
    //     directly and adds a per-instance TEXCOORD7 attribute pointing at
    //     the identity buffer. The mesh's existing per-vertex bindings stay
    //     intact.
    public static partial class MultiDrawIndirect
    {
        // -----------------------------------------------------------------------
        // Mesh-only state
        // -----------------------------------------------------------------------

        // Cache of index buffers per mesh. Mesh.GetIndexBuffer() returns a
        // new GraphicsBuffer the caller must Dispose — calling it every frame
        // leaks the wrapper. We allocate once per mesh and dispose all on
        // shutdown. Key is mesh.GetInstanceID().
        private static readonly Dictionary<int, GraphicsBuffer> _meshIndexBuffers = new Dictionary<int, GraphicsBuffer>();
        private static bool _meshFallbackWarned;

        // True when the current backend's MDI.hlsl branch routes the global
        // instance ID through SV_InstanceID. These backends accept arbitrary
        // user meshes because the shader has no TEXCOORD7 input requirement.
        private static bool MeshApiSupportedNatively
        {
            get
            {
                var api = SystemInfo.graphicsDeviceType;
                return api == GraphicsDeviceType.Metal
                    || api == GraphicsDeviceType.Vulkan
                    || api == GraphicsDeviceType.WebGPU
                    || api == GraphicsDeviceType.Direct3D11
                    || api == GraphicsDeviceType.Direct3D12
                    || api == GraphicsDeviceType.OpenGLES3
                    || api == GraphicsDeviceType.OpenGLCore;
            }
        }

        static partial void DisposeMeshState()
        {
            foreach (var kv in _meshIndexBuffers)
                kv.Value?.Dispose();
            _meshIndexBuffers.Clear();
            _meshFallbackWarned = false;
        }

        // Index format codes — must match the values consumed by native backends.
        // 0 = UInt16, 1 = UInt32.
        private static uint EncodeIndexFormat(IndexFormat fmt)
            => fmt == IndexFormat.UInt16 ? 0u : 1u;

        private static GraphicsBuffer EnsureMeshIndexBuffer(Mesh mesh)
        {
            int id = mesh.GetInstanceID();
            if (_meshIndexBuffers.TryGetValue(id, out var cached))
                return cached;

            mesh.indexBufferTarget |= GraphicsBuffer.Target.Raw;
            var buffer = mesh.GetIndexBuffer();
            _meshIndexBuffers[id] = buffer;
            return buffer;
        }

        private static void WarnMeshFallbackOnce()
        {
            if (_meshFallbackWarned) return;
            _meshFallbackWarned = true;
            Debug.LogWarning(
                $"[MDI] MultiDrawMeshIndirect: true MDI is currently supported on " +
                $"Metal, Vulkan, WebGPU, Direct3D11, Direct3D12, OpenGL ES and OpenGL Core. " +
                $"Current backend ({SystemInfo.graphicsDeviceType}) falls back to a per-draw " +
                $"DrawMeshInstancedIndirect loop, and the user shader must not require TEXCOORD7 " +
                $"in its vertex input layout (i.e. should not include the default " +
                $"MDI_INSTANCE_ID_PARAMETER macro from MDI.hlsl on these APIs).");
        }

        // -----------------------------------------------------------------------
        // CommandBuffer extension
        // -----------------------------------------------------------------------
        public static void MultiDrawMeshIndirect(
            this CommandBuffer cmd,
            Mesh mesh,
            Material material,
            MaterialPropertyBlock properties,
            int shaderPass,
            GraphicsBuffer bufferWithArgs,
            int argsStartIndex,
            int argsCount)
        {
            EnsureInitialized();

            if (_supported && argsCount > 1 && MeshApiSupportedNatively)
            {
                var meshIndexBuffer = EnsureMeshIndexBuffer(mesh);
                IntPtr dataPtr = WriteParams(
                    bufferWithArgs, meshIndexBuffer, argsStartIndex, argsCount,
                    mesh.GetTopology(0),
                    EncodeIndexFormat(mesh.indexFormat),
                    flags: MDI_FLAG_MESH_PATH,
                    out int slot);

                cmd.DrawMeshInstancedIndirect(mesh, 0, material, shaderPass,
                    _dummyArgsBuffer, slot * INDIRECT_DRAW_INDEXED_ARGS_SIZE, properties);

                if (_renderEventAndDataFunc != IntPtr.Zero)
                    cmd.IssuePluginEventAndData(_renderEventAndDataFunc, _baseEventID + slot, dataPtr);
            }
            else
            {
                if (_supported && !MeshApiSupportedNatively) WarnMeshFallbackOnce();
                for (int i = 0; i < argsCount; i++)
                {
                    cmd.DrawMeshInstancedIndirect(mesh, 0, material, shaderPass,
                        bufferWithArgs, (argsStartIndex + i) * INDIRECT_DRAW_INDEXED_ARGS_SIZE, properties);
                }
            }
        }
    }
}
