using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

namespace Saivs.Graphics.Test
{
    [ExecuteInEditMode]
    public class MDITestController : MonoBehaviour
    {
        public enum DrawMode
        {
            // Indexed track — vertices come from a StructuredBuffer<MergedVertex>
            // plus a custom GraphicsBuffer index buffer. Renders the merged geometry.
            MultiDrawIndexedIndirect,
            ProceduralIndirectLoop,
            RenderPrimitivesIndexedIndirect,

            // Mesh track — vertices come through the input assembler from the
            // combined Mesh asset. Same geometry, different draw API.
            MultiDrawMeshIndirect,
            DrawMeshInstancedIndirect,
            RenderMeshIndirect,

            // GPU-count track — the draw count is read by the GPU from a count
            // buffer (here fed by the on-screen slider; in a real setup a
            // culling compute shader writes it). On backends without a native
            // count API (D3D11, Metal, loop fallbacks) maxDrawCount draws run
            // regardless, so the slider has no visual effect there — which
            // makes it a handy diagnostic for the native count path.
            // Appended last to keep serialized DrawMode values stable.
            MultiDrawIndexedIndirectGpuCount,
        }

        [Header("Rendering")]
        [Tooltip("Material used by every Indexed-track mode and by the cmd.* mesh modes (passes 0–4 of MDIIndexedTestShader).")]
        [UnityEngine.Serialization.FormerlySerializedAs("_material")]
        [SerializeField] private Material _indexedMaterial;

        [Tooltip("Material used by Graphics.RenderMeshIndirect — needs a UniversalForward pass with mesh attributes (MDIMeshTest.mat).")]
        [SerializeField] private Material _meshMaterial;

        [SerializeField] private DrawMode _drawMode = DrawMode.MultiDrawIndexedIndirect;
        [SerializeField] private MDITestBufferManager _bufferManager;

        [Tooltip("Draw count uploaded into the GPU count buffer for the GpuCount mode. -1 = all sub-draws. Driven by the on-screen slider at runtime.")]
        [SerializeField] private int _gpuDrawCount = -1;

        private MDIRenderPass _mdiRenderPass;
        private bool _missingMeshMatWarned;

        public DrawMode CurrentDrawMode => _drawMode;
        public MDITestBufferManager BufferManager => _bufferManager;

        private void OnEnable()
        {
            _mdiRenderPass = new MDIRenderPass();
            RenderPipelineManager.beginCameraRendering += OnBeginCameraRendering;
        }
        
        private void OnDisable()
        {
            RenderPipelineManager.beginCameraRendering -= OnBeginCameraRendering;
        }

        private void Update()
        {
            if (Input.GetKeyDown(KeyCode.Space))
            {
                NextDrawMode();
            }
        }
        public void SetMaxFps500()
        {
            Application.targetFrameRate = 500;
        }

        public void SetMaxFps60()
        {
            Application.targetFrameRate = 60;
        }

        public void SetMaxFps120()
        {
            Application.targetFrameRate = 120;
        }
        public void SetMaxFpsUnlimited()
        {
            Application.targetFrameRate = -1;
        }
        public void NextDrawMode()
        {
            _drawMode = _drawMode switch
            {
                DrawMode.MultiDrawIndexedIndirect         => DrawMode.MultiDrawIndexedIndirectGpuCount,
                DrawMode.MultiDrawIndexedIndirectGpuCount => DrawMode.ProceduralIndirectLoop,
                DrawMode.ProceduralIndirectLoop           => DrawMode.RenderPrimitivesIndexedIndirect,
                DrawMode.RenderPrimitivesIndexedIndirect  => DrawMode.MultiDrawMeshIndirect,
                DrawMode.MultiDrawMeshIndirect            => DrawMode.DrawMeshInstancedIndirect,
                DrawMode.DrawMeshInstancedIndirect        => DrawMode.RenderMeshIndirect,
                _                                         => DrawMode.MultiDrawIndexedIndirect,
            };
        }

        private void OnBeginCameraRendering(ScriptableRenderContext context, Camera camera)
        {
            if (_bufferManager == null || _bufferManager.ArgsBuffer == null) return;
            if (_indexedMaterial == null) return;
            if (!_bufferManager.HasMeshes) return;

            switch (_drawMode)
            {
                case DrawMode.RenderPrimitivesIndexedIndirect:
                    DrawRenderPrimitivesIndexedIndirect();
                    break;

                case DrawMode.RenderMeshIndirect:
                    DrawRenderMeshIndirect();
                    break;

                default:
                    EnqueueRenderPass(camera);
                    break;
            }
        }

        private void DrawRenderPrimitivesIndexedIndirect()
        {
            var rp = new RenderParams(_indexedMaterial)
            {
                worldBounds = new Bounds(Vector3.zero, 10000f * Vector3.one),
                matProps = _bufferManager.MPB,
                shadowCastingMode = ShadowCastingMode.Off,
                receiveShadows = false,
                reflectionProbeUsage = ReflectionProbeUsage.Off,
                lightProbeUsage = LightProbeUsage.Off,
            };

            UnityEngine.Graphics.RenderPrimitivesIndexedIndirect(
                rp,
                MeshTopology.Triangles,
                _bufferManager.IndexBuffer,
                _bufferManager.ArgsBuffer,
                _bufferManager.DrawCount,
                0);
        }

        private void DrawRenderMeshIndirect()
        {
            if (_meshMaterial == null)
            {
                if (!_missingMeshMatWarned)
                {
                    _missingMeshMatWarned = true;
                    Debug.LogWarning("[MDITest] RenderMeshIndirect requires Mesh Material (e.g. MDIMeshTest.mat) " +
                                     "with its own UniversalForward pass. Assign it on the controller.");
                }
                return;
            }

            var rp = new RenderParams(_meshMaterial)
            {
                worldBounds = new Bounds(Vector3.zero, 10000f * Vector3.one),
                matProps = _bufferManager.MPB,
                shadowCastingMode = ShadowCastingMode.Off,
                receiveShadows = false,
                reflectionProbeUsage = ReflectionProbeUsage.Off,
                lightProbeUsage = LightProbeUsage.Off,
            };

            UnityEngine.Graphics.RenderMeshIndirect(
                rp,
                _bufferManager.CombinedMesh,
                _bufferManager.ArgsBuffer,
                _bufferManager.DrawCount,
                0);
        }

        private void EnqueueRenderPass(Camera camera)
        {
            if (camera.GetUniversalAdditionalCameraData().scriptableRenderer is not UniversalRenderer urpRenderer)
                return;

            if (_drawMode == DrawMode.MultiDrawIndexedIndirectGpuCount)
                _bufferManager.SetGpuDrawCount(_gpuDrawCount);

            _mdiRenderPass.SetRenderData(
                _bufferManager.IndexBuffer,
                _bufferManager.ArgsBuffer,
                _indexedMaterial,
                _bufferManager.MPB,
                _bufferManager.DrawCount,
                _drawMode,
                _bufferManager.CombinedMesh,
                _bufferManager.DrawCountBuffer);

            urpRenderer.EnqueuePass(_mdiRenderPass);
        }

        // Runtime slider for the GPU-count mode — drag to change how many of
        // the sub-draws the GPU executes. The value only ever reaches the GPU
        // through the count buffer, so a responding slider proves the native
        // count path is active on the current backend.
        private void OnGUI()
        {
            if (_drawMode != DrawMode.MultiDrawIndexedIndirectGpuCount) return;
            if (_bufferManager == null || _bufferManager.DrawCountBuffer == null) return;

            int max = _bufferManager.DrawCount;
            int current = _gpuDrawCount < 0 ? max : Mathf.Clamp(_gpuDrawCount, 0, max);

            var sliderRect = new Rect(10f, Screen.height - 40f, 320f, 22f);
            var labelRect  = new Rect(10f, Screen.height - 64f, 320f, 20f);

            GUI.Label(labelRect, $"GPU draw count: {current} / {max}");
            _gpuDrawCount = Mathf.RoundToInt(
                GUI.HorizontalSlider(sliderRect, current, 0f, max));
        }
    }
}
