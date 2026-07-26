using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.Rendering;

namespace Saivs.Graphics.Test
{
    [ExecuteInEditMode]
    public class MDITestBufferManager : MonoBehaviour
    {
        [Header("Grid Settings")]
        [SerializeField] private int _gridSize = 8;
        [SerializeField] private float _spacing = 1.5f;

        [Header("Meshes")]
        [Tooltip("All meshes are merged into one combined Mesh (used by the Mesh draw modes) " +
                 "and one StructuredBuffer<MergedVertex> + index buffer (used by the Indexed draw modes). " +
                 "Each sub-draw command picks a random mesh from this array — the visible scene shows " +
                 "different meshes scattered across the grid. Meshes must be Read/Write enabled.")]
        [SerializeField] private Mesh[] _meshes;

        // Vertex layout uploaded into _vertexBuffer. Must match the HLSL `MergedVertex`
        // struct in MDIIndexedTestShader.shader (float3 position; float3 normal;).
        [StructLayout(LayoutKind.Sequential)]
        private struct MergedVertex
        {
            public Vector3 position;
            public Vector3 normal;
        }

        private struct MeshSlice
        {
            public uint startIndex;
            public uint indexCount;
        }

        private GraphicsBuffer _indexBuffer;
        private GraphicsBuffer _vertexBuffer;
        private GraphicsBuffer _drawPositionsBuffer;
        private GraphicsBuffer _argsBuffer;
        private GraphicsBuffer _drawCountBuffer;
        private MaterialPropertyBlock _mpb;
        private Mesh _combinedMesh;
        private int _drawCount;
        private int _uploadedGpuCount = -1;
        private readonly uint[] _gpuCountUpload = new uint[1];

        public GraphicsBuffer IndexBuffer => _indexBuffer;
        public GraphicsBuffer ArgsBuffer => _argsBuffer;
        public GraphicsBuffer DrawCountBuffer => _drawCountBuffer;
        public MaterialPropertyBlock MPB => _mpb;
        public int DrawCount => _drawCount;
        public Mesh CombinedMesh => _combinedMesh;
        public bool HasMeshes => _meshes != null && _meshes.Length > 0;

        // Uploads the GPU draw count (clamped to [0, DrawCount]) into
        // _drawCountBuffer. In this sample the count comes from a UI slider via
        // SetData; in a real GPU-driven setup a culling compute shader would
        // write it instead — either way the draw call reads it from the buffer.
        public void SetGpuDrawCount(int count)
        {
            if (_drawCountBuffer == null) return;

            count = Mathf.Clamp(count < 0 ? _drawCount : count, 0, _drawCount);
            if (count == _uploadedGpuCount) return;

            _gpuCountUpload[0] = (uint)count;
            _drawCountBuffer.SetData(_gpuCountUpload);
            _uploadedGpuCount = count;
        }

        private void OnEnable()
        {
            CreateBuffers();
        }

        private void OnDisable()
        {
            DisposeBuffers();
        }

        private void CreateBuffers()
        {
            if (_meshes == null || _meshes.Length == 0)
            {
                Debug.LogWarning("[MDITest] No meshes assigned on the BufferManager — assign at least one mesh.");
                return;
            }

            int instanceCount = _gridSize * _gridSize;
            if (instanceCount == 0) return;

            // Per-instance grid positions (shared by every draw mode).
            var positions = new Vector3[instanceCount];
            for (int z = 0; z < _gridSize; z++)
            {
                for (int x = 0; x < _gridSize; x++)
                {
                    int i = z * _gridSize + x;
                    positions[i] = new Vector3(
                        (x - _gridSize / 2f + 0.5f) * _spacing,
                        0f,
                        (z - _gridSize / 2f + 0.5f) * _spacing);
                }
            }

            _drawPositionsBuffer = new GraphicsBuffer(
                GraphicsBuffer.Target.Structured, instanceCount, sizeof(float) * 3);
            _drawPositionsBuffer.SetData(positions);

            // Build the merged geometry — used by both the Mesh path (via _combinedMesh)
            // and the Indexed path (via _vertexBuffer / _indexBuffer). Indices are
            // promoted to global vertex IDs so args.baseVertexIndex stays at 0.
            BuildMergedGeometry(out var slices);

            // Generate the args buffer — random mesh per sub-draw, deterministic seed.
            int subDrawCount = instanceCount / 2;
            var args = new GraphicsBuffer.IndirectDrawIndexedArgs[subDrawCount];
            Random.InitState(0);
            for (int i = 0; i < subDrawCount; i++)
            {
                int meshIdx = Random.Range(0, _meshes.Length);
                MeshSlice s = slices[meshIdx];
                args[i] = new GraphicsBuffer.IndirectDrawIndexedArgs
                {
                    indexCountPerInstance = s.indexCount,
                    instanceCount = 2,
                    startIndex = s.startIndex,
                    baseVertexIndex = 0,
                    startInstance = (uint)(i * 2),
                };
            }

            _argsBuffer = new GraphicsBuffer(
                GraphicsBuffer.Target.IndirectArguments | GraphicsBuffer.Target.Structured,
                subDrawCount,
                GraphicsBuffer.IndirectDrawIndexedArgs.size);
            _argsBuffer.SetData(args);

            _drawCount = subDrawCount;

            // Single uint32 read by the GPU as the draw count (GPU-count draw
            // mode). IndirectArguments is required so the underlying buffer is
            // created with indirect usage on Vulkan/WebGPU; Structured keeps it
            // writable from a compute shader.
            _drawCountBuffer = new GraphicsBuffer(
                GraphicsBuffer.Target.IndirectArguments | GraphicsBuffer.Target.Structured,
                1, sizeof(uint));
            _uploadedGpuCount = -1;
            SetGpuDrawCount(subDrawCount);

            _mpb = new MaterialPropertyBlock();
            _mpb.SetBuffer("_VertexBuffer", _vertexBuffer);
            _mpb.SetBuffer("_DrawPositions", _drawPositionsBuffer);
            _mpb.SetBuffer("_ArgsBuffer", _argsBuffer);
        }

        // Concatenates positions/normals/indices from every input mesh and
        // uploads them into _vertexBuffer/_indexBuffer + _combinedMesh.
        private void BuildMergedGeometry(out MeshSlice[] slices)
        {
            var verts = new List<MergedVertex>();
            var allPositions = new List<Vector3>();
            var allNormals = new List<Vector3>();
            var allIndices = new List<int>();
            slices = new MeshSlice[_meshes.Length];

            int globalVOffset = 0;

            for (int i = 0; i < _meshes.Length; i++)
            {
                Mesh m = _meshes[i];
                if (m == null)
                {
                    Debug.LogWarning($"[MDITest] _meshes[{i}] is null — skipping.");
                    slices[i] = new MeshSlice { startIndex = 0, indexCount = 0 };
                    continue;
                }
                if (!m.isReadable)
                {
                    Debug.LogError($"[MDITest] Mesh '{m.name}' is not Read/Write enabled. " +
                                   "Toggle 'Read/Write' in its import settings.");
                    slices[i] = new MeshSlice { startIndex = 0, indexCount = 0 };
                    continue;
                }

                Vector3[] pos = m.vertices;
                Vector3[] nor = (m.normals != null && m.normals.Length == pos.Length) ? m.normals : null;
                int[] idx = m.GetIndices(0);

                for (int j = 0; j < pos.Length; j++)
                {
                    verts.Add(new MergedVertex
                    {
                        position = pos[j],
                        normal = nor != null ? nor[j] : Vector3.up,
                    });
                    allPositions.Add(pos[j]);
                    allNormals.Add(nor != null ? nor[j] : Vector3.up);
                }

                int sliceStart = allIndices.Count;
                for (int k = 0; k < idx.Length; k++)
                {
                    allIndices.Add(idx[k] + globalVOffset);
                }

                slices[i] = new MeshSlice
                {
                    startIndex = (uint)sliceStart,
                    indexCount = (uint)idx.Length,
                };

                globalVOffset += pos.Length;
            }

            // Indexed path buffers.
            _vertexBuffer = new GraphicsBuffer(
                GraphicsBuffer.Target.Structured, verts.Count, Marshal.SizeOf<MergedVertex>());
            _vertexBuffer.SetData(verts);

            _indexBuffer = new GraphicsBuffer(GraphicsBuffer.Target.Index, allIndices.Count, sizeof(int));
            _indexBuffer.SetData(allIndices);

            // Mesh-path combined Mesh — same layout as the indexed buffers.
            _combinedMesh = new Mesh
            {
                name = "MDITest_CombinedMesh",
                hideFlags = HideFlags.HideAndDontSave,
                indexFormat = IndexFormat.UInt32,
            };
            _combinedMesh.SetVertices(allPositions);
            _combinedMesh.SetNormals(allNormals);
            _combinedMesh.SetIndices(allIndices, MeshTopology.Triangles, 0, calculateBounds: true);
        }

        private void DisposeBuffers()
        {
            _indexBuffer?.Dispose();
            _vertexBuffer?.Dispose();
            _drawPositionsBuffer?.Dispose();
            _argsBuffer?.Dispose();
            _drawCountBuffer?.Dispose();

            _indexBuffer = null;
            _vertexBuffer = null;
            _drawPositionsBuffer = null;
            _argsBuffer = null;
            _drawCountBuffer = null;
            _uploadedGpuCount = -1;

            if (_combinedMesh != null)
            {
                if (Application.isPlaying) Destroy(_combinedMesh);
                else                       DestroyImmediate(_combinedMesh);
                _combinedMesh = null;
            }
        }
    }
}
