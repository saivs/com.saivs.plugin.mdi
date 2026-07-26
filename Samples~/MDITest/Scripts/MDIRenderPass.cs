using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;
using UnityEngine.Rendering.RenderGraphModule;
using Saivs.Graphics.Core.MDI;

namespace Saivs.Graphics.Test
{
    public class MDIRenderPass : ScriptableRenderPass
    {
        // Pass indices in MDIIndexedTestShader.shader:
        //   0 — RenderPrimitivesForward          (UniversalForward, indexed)
        //   1 — MDIIndexedForward                (indexed, MDI plugin)
        //   2 — ProceduralLoopForward            (indexed, managed loop)
        //   3 — MDIMeshForward                   (mesh,    MDI plugin)
        //   4 — DrawMeshInstancedIndirectForward (mesh,    managed loop)
        private const int PASS_MDI_INDEXED = 1;
        private const int PASS_PROCEDURAL_LOOP = 2;
        private const int PASS_MDI_MESH = 3;
        private const int PASS_MESH_LOOP = 4;

        private static readonly int DrawCallIndexID = Shader.PropertyToID("_DrawCallIndex");

        private GraphicsBuffer _indexBuffer;
        private GraphicsBuffer _argsBuffer;
        private Material _material;
        private MaterialPropertyBlock _mpb;
        private int _drawCount;
        private MDITestController.DrawMode _drawMode;
        private Mesh _combinedMesh;
        private GraphicsBuffer _drawCountBuffer;

        public MDIRenderPass()
        {
            renderPassEvent = RenderPassEvent.AfterRenderingOpaques;
        }

        public void SetRenderData(
            GraphicsBuffer indexBuffer,
            GraphicsBuffer argsBuffer,
            Material material,
            MaterialPropertyBlock mpb,
            int drawCount,
            MDITestController.DrawMode drawMode,
            Mesh combinedMesh,
            GraphicsBuffer drawCountBuffer)
        {
            _indexBuffer = indexBuffer;
            _argsBuffer = argsBuffer;
            _material = material;
            _mpb = mpb;
            _drawCount = drawCount;
            _drawMode = drawMode;
            _combinedMesh = combinedMesh;
            _drawCountBuffer = drawCountBuffer;
        }

        private class PassData
        {
            public GraphicsBuffer indexBuffer;
            public GraphicsBuffer argsBuffer;
            public Material material;
            public MaterialPropertyBlock mpb;
            public int drawCount;
            public MDITestController.DrawMode drawMode;
            public Mesh combinedMesh;
            public GraphicsBuffer drawCountBuffer;
        }

        public override void RecordRenderGraph(RenderGraph renderGraph, ContextContainer frameData)
        {
            if (_argsBuffer == null || _material == null) return;

            var resourceData = frameData.Get<UniversalResourceData>();

            using var builder = renderGraph.AddRasterRenderPass<PassData>("MDI Render Pass", out var passData);

            passData.indexBuffer = _indexBuffer;
            passData.argsBuffer = _argsBuffer;
            passData.material = _material;
            passData.mpb = _mpb;
            passData.drawCount = _drawCount;
            passData.drawMode = _drawMode;
            passData.combinedMesh = _combinedMesh;
            passData.drawCountBuffer = _drawCountBuffer;

            builder.SetRenderAttachment(resourceData.activeColorTexture, 0);
            builder.SetRenderAttachmentDepth(resourceData.activeDepthTexture);
            builder.AllowGlobalStateModification(true);

            builder.SetRenderFunc((PassData data, RasterGraphContext ctx) =>
            {
                Render(ctx.cmd, data);
            });
        }

        private static void Render(RasterCommandBuffer cmd, PassData data)
        {
            switch (data.drawMode)
            {
                // -------- Indexed track --------
                case MDITestController.DrawMode.MultiDrawIndexedIndirect:
                    cmd.MultiDrawIndexedIndirect(
                        indexBuffer: data.indexBuffer,
                        material: data.material,
                        properties: data.mpb,
                        shaderPass: PASS_MDI_INDEXED,
                        topology: MeshTopology.Triangles,
                        bufferWithArgs: data.argsBuffer,
                        argsStartIndex: 0,
                        argsCount: data.drawCount);
                    break;

                case MDITestController.DrawMode.MultiDrawIndexedIndirectGpuCount:
                    // The GPU reads the actual draw count from drawCountBuffer
                    // (clamped to maxDrawCount). On backends without a native
                    // count API the full maxDrawCount executes instead — see
                    // the DrawMode comment in MDITestController.
                    cmd.MultiDrawIndexedIndirect(
                        indexBuffer: data.indexBuffer,
                        material: data.material,
                        properties: data.mpb,
                        shaderPass: PASS_MDI_INDEXED,
                        topology: MeshTopology.Triangles,
                        bufferWithArgs: data.argsBuffer,
                        argsStartIndex: 0,
                        drawCountBuffer: data.drawCountBuffer,
                        drawCountByteOffset: 0,
                        maxDrawCount: data.drawCount);
                    break;

                case MDITestController.DrawMode.ProceduralIndirectLoop:
                    for (int i = 0; i < data.drawCount; i++)
                    {
                        cmd.SetGlobalInt(DrawCallIndexID, i);
                        cmd.DrawProceduralIndirect(
                            indexBuffer: data.indexBuffer,
                            matrix: Matrix4x4.identity,
                            material: data.material,
                            shaderPass: PASS_PROCEDURAL_LOOP,
                            topology: MeshTopology.Triangles,
                            bufferWithArgs: data.argsBuffer,
                            argsOffset: i * GraphicsBuffer.IndirectDrawIndexedArgs.size,
                            properties: data.mpb);
                    }
                    break;

                // -------- Mesh track --------
                case MDITestController.DrawMode.MultiDrawMeshIndirect:
                    cmd.MultiDrawMeshIndirect(
                        mesh: data.combinedMesh,
                        material: data.material,
                        properties: data.mpb,
                        shaderPass: PASS_MDI_MESH,
                        bufferWithArgs: data.argsBuffer,
                        argsStartIndex: 0,
                        argsCount: data.drawCount);
                    break;

                case MDITestController.DrawMode.DrawMeshInstancedIndirect:
                    for (int i = 0; i < data.drawCount; i++)
                    {
                        cmd.SetGlobalInt(DrawCallIndexID, i);
                        cmd.DrawMeshInstancedIndirect(
                            mesh: data.combinedMesh,
                            submeshIndex: 0,
                            material: data.material,
                            shaderPass: PASS_MESH_LOOP,
                            bufferWithArgs: data.argsBuffer,
                            argsOffset: i * GraphicsBuffer.IndirectDrawIndexedArgs.size,
                            properties: data.mpb);
                    }
                    break;

                // RenderPrimitivesIndexedIndirect / RenderMeshIndirect are dispatched
                // directly from the controller via Graphics.* and never reach here.
            }
        }
    }
}
