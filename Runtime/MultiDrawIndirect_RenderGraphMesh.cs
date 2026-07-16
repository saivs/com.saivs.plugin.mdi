#if UNITY_6000_0_OR_NEWER
using System;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;

namespace Saivs.Graphics.Core.MDI
{
    // Unity 6 RenderGraph variants of MultiDrawMeshIndirect — for use inside
    // ScriptableRenderPass.RecordRenderGraph (RasterCommandBuffer) and
    // unsafe-state passes (UnsafeCommandBuffer).
    //
    // Behaviour and parameters are identical to the CommandBuffer overload —
    // see MultiDrawIndirect_Mesh.cs for backend support details.
    public static partial class MultiDrawIndirect
    {
        // -----------------------------------------------------------------------
        // RasterCommandBuffer extension
        // -----------------------------------------------------------------------
        public static void MultiDrawMeshIndirect(
            this RasterCommandBuffer cmd,
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

        // -----------------------------------------------------------------------
        // UnsafeCommandBuffer extension
        // -----------------------------------------------------------------------
        public static void MultiDrawMeshIndirect(
            this UnsafeCommandBuffer cmd,
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
#endif
