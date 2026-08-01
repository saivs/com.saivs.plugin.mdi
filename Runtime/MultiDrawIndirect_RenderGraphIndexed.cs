#if UNITY_6000_0_OR_NEWER
using System;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.RenderGraphModule;

namespace Saivs.Graphics.Core.MDI
{
    // Unity 6 RenderGraph variants of MultiDrawIndexedIndirect — for use inside
    // ScriptableRenderPass.RecordRenderGraph (RasterCommandBuffer, used in
    // raster passes) and inside compute / unsafe-state passes (UnsafeCommandBuffer).
    //
    // Behaviour and parameters are identical to the CommandBuffer overload —
    // see MultiDrawIndirect_Indexed.cs for the prime-mesh / fallback details.
    public static partial class MultiDrawIndirect
    {
        // -----------------------------------------------------------------------
        // Prepare event — RenderGraph variants. Call right after the compute
        // dispatch that wrote the args buffer (compute or unsafe pass), so the
        // write→indirect-read barrier is recorded outside any render pass.
        // See PrepareIndirectArgs(CommandBuffer, GraphicsBuffer).
        // -----------------------------------------------------------------------

        /// <inheritdoc cref="PrepareIndirectArgs(CommandBuffer, GraphicsBuffer)"/>
        public static void PrepareIndirectArgs(this ComputeCommandBuffer cmd, GraphicsBuffer argsBuffer)
        {
            IntPtr dataPtr = BeginPrepareIndirectArgs(argsBuffer);
            if (dataPtr == IntPtr.Zero)
                return;
            cmd.IssuePluginEventAndData(_renderEventAndDataFunc, _baseEventID + PREPARE_ARGS_EVENT, dataPtr);
        }

        /// <inheritdoc cref="PrepareIndirectArgs(CommandBuffer, GraphicsBuffer)"/>
        public static void PrepareIndirectArgs(this UnsafeCommandBuffer cmd, GraphicsBuffer argsBuffer)
        {
            IntPtr dataPtr = BeginPrepareIndirectArgs(argsBuffer);
            if (dataPtr == IntPtr.Zero)
                return;
            cmd.IssuePluginEventAndData(_renderEventAndDataFunc, _baseEventID + PREPARE_ARGS_EVENT, dataPtr);
        }

        // -----------------------------------------------------------------------
        // RasterCommandBuffer extension
        // -----------------------------------------------------------------------
        public static void MultiDrawIndexedIndirect(
            this RasterCommandBuffer cmd,
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

            if (_supported && argsCount > 1 && PrimeDrawWillRecord(material, shaderPass))
            {
                IntPtr dataPtr = WriteParams(bufferWithArgs, indexBuffer, argsStartIndex, argsCount, topology, indexFormat: 1, flags: 0, out int slot);

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
                for (int i = 0; i < argsCount; i++)
                {
                    cmd.DrawProceduralIndirect(
                        indexBuffer: indexBuffer, matrix: Matrix4x4.identity, material: material,
                        shaderPass: shaderPass, topology: topology, bufferWithArgs: bufferWithArgs,
                        argsOffset: (argsStartIndex + i) * INDIRECT_DRAW_INDEXED_ARGS_SIZE, properties: properties);
                }
            }
        }

        // -----------------------------------------------------------------------
        // UnsafeCommandBuffer extension
        // -----------------------------------------------------------------------
        public static void MultiDrawIndexedIndirect(
            this UnsafeCommandBuffer cmd,
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

            if (_supported && argsCount > 1 && PrimeDrawWillRecord(material, shaderPass))
            {
                IntPtr dataPtr = WriteParams(bufferWithArgs, indexBuffer, argsStartIndex, argsCount, topology, indexFormat: 1, flags: 0, out int slot);

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
            this RasterCommandBuffer cmd,
            GraphicsBuffer indexBuffer,
            Material material,
            MaterialPropertyBlock properties,
            int shaderPass,
            MeshTopology topology,
            GraphicsBuffer bufferWithArgs,
            int argsStartIndex,
            GraphicsBuffer drawCountBuffer, // Buffer updated dynamically by GPU compute
            int drawCountByteOffset, // Byte offset of the uint32 count inside the count buffer (usually 0, must be a multiple of 4)
            int maxDrawCount // Fallback upper-bound limit for the hardware loop
        )
        {
            EnsureInitialized();

            if (_supported && maxDrawCount > 0 && PrimeDrawWillRecord(material, shaderPass))
            {
                IntPtr dataPtr = WriteParams(bufferWithArgs, indexBuffer, argsStartIndex, maxDrawCount, topology, indexFormat: 1, flags: MDI_FLAG_GPU_COUNT, out int slot,
                    countBuffer: drawCountBuffer, countOffsetBytes: drawCountByteOffset);

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
                for (int i = 0; i < maxDrawCount; i++)
                {
                    cmd.DrawProceduralIndirect(
                        indexBuffer: indexBuffer, matrix: Matrix4x4.identity, material: material,
                        shaderPass: shaderPass, topology: topology, bufferWithArgs: bufferWithArgs,
                        argsOffset: (argsStartIndex + i) * INDIRECT_DRAW_INDEXED_ARGS_SIZE, properties: properties);
                }
            }
        }

        public static void MultiDrawIndexedIndirect(
            this UnsafeCommandBuffer cmd,
            GraphicsBuffer indexBuffer,
            Material material,
            MaterialPropertyBlock properties,
            int shaderPass,
            MeshTopology topology,
            GraphicsBuffer bufferWithArgs,
            int argsStartIndex,
            GraphicsBuffer drawCountBuffer, // Buffer updated dynamically by GPU compute
            int drawCountByteOffset, // Byte offset of the uint32 count inside the count buffer (usually 0, must be a multiple of 4)
            int maxDrawCount // Fallback upper-bound limit for the hardware loop
        )
        {
            EnsureInitialized();

            if (_supported && maxDrawCount > 0 && PrimeDrawWillRecord(material, shaderPass))
            {
                IntPtr dataPtr = WriteParams(bufferWithArgs, indexBuffer, argsStartIndex, maxDrawCount, topology, indexFormat: 1, flags: MDI_FLAG_GPU_COUNT, out int slot,
                    countBuffer: drawCountBuffer, countOffsetBytes: drawCountByteOffset);

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
#endif
