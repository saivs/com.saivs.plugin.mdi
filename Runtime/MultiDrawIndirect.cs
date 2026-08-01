using System;
using System.Runtime.InteropServices;
using Unity.Collections;
using Unity.Collections.LowLevel.Unsafe;
using UnityEngine;
using UnityEngine.Rendering;

namespace Saivs.Graphics.Core.MDI
{
    /// <summary>
    /// Native Multi-Draw Indirect plugin bridge (D3D11, D3D12, Vulkan, OpenGLES, OpenGL, Metal, WebGPU).
    ///
    /// Flow (identical for all APIs):
    /// 1. Prime draw with dummy args (instanceCount=0) — binds PSO, render targets, shaders.
    /// 2. IssuePluginEventAndData — plugin receives params via pinned ring buffer pointer.
    ///    - D3D11: NvAPI hardware MDI or native DrawIndexedInstancedIndirect loop.
    ///    - D3D12: ExecuteIndirect on Unity's command list via CommandRecordingState.
    ///    - Vulkan: vkCmdDrawIndexedIndirect (multi-draw if supported, loop fallback otherwise).
    ///    - Metal: drawIndexedPrimitives:indirectBuffer: dispatched from inside a method-swizzle hook.
    ///
    /// The class is split across several partial files:
    ///   • MultiDrawIndirect.cs                  — core state, init/dispose, shared helpers.
    ///   • MultiDrawIndirect_Indexed.cs          — CommandBuffer.MultiDrawIndexedIndirect + prime-mesh helpers.
    ///   • MultiDrawIndirect_Mesh.cs             — CommandBuffer.MultiDrawMeshIndirect + mesh helpers.
    ///   • MultiDrawIndirect_RenderGraphIndexed.cs — RasterCommandBuffer / UnsafeCommandBuffer indexed overloads (Unity 6).
    ///   • MultiDrawIndirect_RenderGraphMesh.cs    — RasterCommandBuffer / UnsafeCommandBuffer mesh overloads (Unity 6).
    /// </summary>
    public static partial class MultiDrawIndirect
    {
#if UNITY_WEBGL && !UNITY_EDITOR
        // Web builds link statically; the MDI_* symbols come from
        // Plugins/WebGL/MDIBackend_WebGPU.jslib.
        private const string DLL_NAME = "__Internal";
#else
        private const string DLL_NAME = "GfxPluginMDI";
#endif
        private const int INDIRECT_DRAW_INDEXED_ARGS_SIZE = 20; // 5 * sizeof(uint)
        private const int MAX_PENDING = 256;

        // Written into NativeMDIParams._pad. Native backends ignore it; the
        // WebGPU jslib backend uses it to validate ring slots when it
        // intercepts prime draws (must match MAGIC in MDIBackend_WebGPU.jslib).
        private const uint MDI_RING_MAGIC = 0x4D444921;

        // MDIParams flags — must match MDI_FLAG_* in MDIBackend.h.
        private const uint MDI_FLAG_MESH_PATH = 1u << 0;
        private const uint MDI_FLAG_GPU_COUNT = 1u << 1;

        // Must match native MDIParams layout. GPU-count fields are appended at
        // the end so the ring magic stays at u32 index 7 (see MDIBackend_WebGPU.jslib).
        [StructLayout(LayoutKind.Sequential)]
        private struct NativeMDIParams
        {
            public IntPtr argsBuffer;
            public IntPtr indexBuffer;
            public uint argsOffsetBytes;
            public uint maxDrawCount;
            public uint indexFormat;
            public uint topology;
            public uint flags;
            public uint _pad;
            public IntPtr countBuffer;    // GPU draw-count buffer (MDI_FLAG_GPU_COUNT), else zero
            public uint countOffsetBytes; // Byte offset of the uint32 count inside countBuffer
            public uint _pad2;
        }

        // Native imports
        [DllImport(DLL_NAME)] private static extern int MDI_AllocSlot();
        [DllImport(DLL_NAME)] private static extern int MDI_GetBaseEventID();
        [DllImport(DLL_NAME)] private static extern IntPtr MDI_GetRenderEventAndDataFunc();
        [DllImport(DLL_NAME)] private static extern int MDI_IsSupported();
        [DllImport(DLL_NAME)] private static extern int MDI_UsesPerInstanceVB();
        [DllImport(DLL_NAME)] private static extern int MDI_SetMaxInstanceCount(uint maxCount);
        [DllImport(DLL_NAME)] private static extern uint MDI_GetMaxInstanceCount();
        [DllImport(DLL_NAME)] private static extern void MDI_SetDummyArgsBuffer(IntPtr nativePtr);
        [DllImport(DLL_NAME)] private static extern void MDI_SetParamsRing(IntPtr basePtr);
        [DllImport(DLL_NAME)] private static extern void MDI_SetDrawIndexBuffer(IntPtr nativePtr);
        [DllImport(DLL_NAME)] private static extern void MDI_SetLogCallback(IntPtr callback);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void NativeLogDelegate(IntPtr utf8Msg);

        // Holding a managed reference prevents the delegate from being GC'd
        // while native code still holds its function pointer.
        private static NativeLogDelegate _logDelegate;

        [AOT.MonoPInvokeCallback(typeof(NativeLogDelegate))]
        private static void OnNativeLog(IntPtr utf8Msg)
        {
            if (utf8Msg == IntPtr.Zero) return;
            string msg = Marshal.PtrToStringAnsi(utf8Msg);
            if (!string.IsNullOrEmpty(msg))
                Debug.Log(msg.TrimEnd('\n', '\r'));
        }

        private static IntPtr _renderEventAndDataFunc;
        private static bool _initialized;
        private static bool _supported;
        private static int _baseEventID;

        // Pinned ring buffer for IssuePluginEventAndData — stable pointers for render thread
        private static NativeArray<NativeMDIParams> _paramsRing;

        // Dummy args buffer (instanceCount=0) for the zero-pixel prime draw
        private static GraphicsBuffer _dummyArgsBuffer;

        // Per-draw index buffer used by the Metal backend. Holds [0, 1, …, MAX_PENDING-1]
        // as uint32; the native swizzle re-binds it with offset = i*4 between each
        // indirect draw, so the shader's `_MDI_DrawIndex_Buffer[0]` reads `i`.
        // No-op on other platforms.
        private static GraphicsBuffer _drawIndexBuffer;
        private static readonly int s_DrawIndexBufferID = Shader.PropertyToID("_MDI_DrawIndex_Buffer");

        // Per-feature lifecycle hooks implemented in the corresponding partial files.
        // Empty (compile to no-op) if the partial isn't compiled in this build.
        static partial void InitIndexedState();
        static partial void DisposeIndexedState();
        static partial void DisposeMeshState();

        public static bool IsSupported
        {
            get
            {
                EnsureInitialized();
                return _supported;
            }
        }

        /// <summary>
        /// Maximum number of instances that can be addressed by the per-instance identity buffer
        /// on D3D11/D3D12. For any draw command, <c>startInstance + instanceCount</c> must not exceed
        /// this value. Default is 65,536. Returns 0 on APIs that don't use the identity buffer
        /// (Vulkan, OpenGL, Metal).
        /// </summary>
        public static uint MaxInstanceCount
        {
            get
            {
                EnsureInitialized();
                if (!_supported) return 0;
                try { return MDI_GetMaxInstanceCount(); }
                catch { return 0; }
            }
            set
            {
                EnsureInitialized();
                if (!_supported) return;
                try
                {
                    if (MDI_SetMaxInstanceCount(value) == 0)
                        Debug.LogError($"[MDI] Failed to resize identity buffer to {value}.");
                    else
                        Debug.Log($"[MDI] Identity buffer resized to {value} entries.");
                }
                catch (Exception e)
                {
                    Debug.LogError($"[MDI] Failed to resize identity buffer: {e.Message}");
                }
            }
        }

        private static void EnsureInitialized()
        {
            if (_initialized) return;
            _initialized = true;

            // Auto-cleanup: without these hooks, domain reload (entering Play
            // Mode, recompiling) silently nulls our static caches while leaving
            // native GraphicsBuffer / NativeArray allocations orphaned.
            Application.quitting += Dispose;
#if UNITY_EDITOR
            UnityEditor.AssemblyReloadEvents.beforeAssemblyReload += Dispose;
            UnityEditor.EditorApplication.playModeStateChanged += OnPlayModeStateChanged;
#endif

            try
            {
                // Route native plugin logs into Unity console BEFORE anything
                // else — otherwise the backend's init/hook diagnostics go to
                // OutputDebugString and are invisible without DebugView.
                try
                {
                    _logDelegate = OnNativeLog;
                    MDI_SetLogCallback(Marshal.GetFunctionPointerForDelegate(_logDelegate));
                }
                catch (EntryPointNotFoundException) { /* older native plugin — ignore */ }

                _supported = MDI_IsSupported() != 0;

                InitIndexedState();

                if (_supported)
                {
                    _baseEventID = MDI_GetBaseEventID();
                    _renderEventAndDataFunc = MDI_GetRenderEventAndDataFunc();
                    _paramsRing = new NativeArray<NativeMDIParams>(MAX_PENDING, Allocator.Persistent);

                    // MAX_PENDING entries so we can encode the ring-buffer slot
                    // into argsOffset for each prime draw — the Metal backend
                    // recovers the slot in its method-swizzling hook.
                    _dummyArgsBuffer = new GraphicsBuffer(
                        GraphicsBuffer.Target.IndirectArguments, MAX_PENDING,
                        GraphicsBuffer.IndirectDrawIndexedArgs.size);
                    _dummyArgsBuffer.SetData(new GraphicsBuffer.IndirectDrawIndexedArgs[MAX_PENDING]);

                    // Per-draw-index buffer for the Metal backend. Pre-fill with
                    // [0, 1, …, MAX_PENDING-1]; the native swizzle re-binds with
                    // offset = i*4 between each draw inside the prime.
                    var drawIndices = new uint[MAX_PENDING];
                    for (int i = 0; i < MAX_PENDING; i++) drawIndices[i] = (uint)i;
                    _drawIndexBuffer = new GraphicsBuffer(
                        GraphicsBuffer.Target.Structured, MAX_PENDING, sizeof(uint));
                    _drawIndexBuffer.SetData(drawIndices);
                    Shader.SetGlobalBuffer(s_DrawIndexBufferID, _drawIndexBuffer);

                    try
                    {
                        MDI_SetDummyArgsBuffer(_dummyArgsBuffer.GetNativeBufferPtr());
                        MDI_SetDrawIndexBuffer(_drawIndexBuffer.GetNativeBufferPtr());
                        unsafe { MDI_SetParamsRing((IntPtr)_paramsRing.GetUnsafeReadOnlyPtr()); }
                    }
                    catch (EntryPointNotFoundException) { /* older native plugin — ignore */ }

                    var api = SystemInfo.graphicsDeviceType;
                    Debug.Log($"[MDI] Initialized: {api}, baseEventID={_baseEventID}");
                }
            }
            catch (DllNotFoundException)
            {
                _supported = false;
                Debug.LogWarning("[MDI] GfxPluginMDI native plugin not found. Falling back to DrawProceduralIndirect loop.");
            }
            catch (EntryPointNotFoundException)
            {
                _supported = false;
                Debug.LogWarning("[MDI] GfxPluginMDI native plugin is outdated. Falling back to DrawProceduralIndirect loop.");
            }
            catch (Exception e)
            {
                _supported = false;
                Debug.LogError($"[MDI] Native plugin initialization failed: {e.Message}. Falling back to DrawProceduralIndirect loop.");
            }
        }

        public static void Dispose()
        {
            if (_paramsRing.IsCreated)
                _paramsRing.Dispose();
            _dummyArgsBuffer?.Dispose();
            _dummyArgsBuffer = null;
            _drawIndexBuffer?.Dispose();
            _drawIndexBuffer = null;

            DisposeIndexedState();
            DisposeMeshState();

            Application.quitting -= Dispose;
#if UNITY_EDITOR
            UnityEditor.AssemblyReloadEvents.beforeAssemblyReload -= Dispose;
            UnityEditor.EditorApplication.playModeStateChanged -= OnPlayModeStateChanged;
#endif

            _initialized = false;
        }

#if UNITY_EDITOR
        // Dispose on every Play Mode boundary. Even with "Reload Domain"
        // disabled in Enter Play Mode Settings — when assembly reload doesn't
        // fire — we still want to drop edit-mode mesh index buffers, since
        // edit-mode mesh assets may be unloaded once Play Mode starts.
        private static void OnPlayModeStateChanged(UnityEditor.PlayModeStateChange change)
        {
            if (change == UnityEditor.PlayModeStateChange.ExitingEditMode ||
                change == UnityEditor.PlayModeStateChange.ExitingPlayMode)
                Dispose();
        }
#endif

        // Write params to the pinned ring buffer; return a stable pointer for the
        // render thread. Used by both the indexed and mesh draw paths.
        private static unsafe IntPtr WriteParams(
            GraphicsBuffer bufferWithArgs,
            GraphicsBuffer indexBuffer,
            int argsStartIndex,
            int argsCount,
            MeshTopology topology,
            uint indexFormat,
            uint flags,
            out int slot,
            GraphicsBuffer countBuffer = null,
            int countOffsetBytes = 0)
        {
            slot = MDI_AllocSlot();

            // D3D12/Vulkan/GL all require the uint32 count to sit on a 4-byte
            // boundary. Rounding silently would make the GPU read the count
            // from the wrong address — surface the caller's mistake instead.
            if ((countOffsetBytes & 3) != 0)
            {
                Debug.LogError(
                    $"[MDI] drawCountByteOffset must be a multiple of 4, got {countOffsetBytes} — rounding down to {countOffsetBytes & ~3}.");
                countOffsetBytes &= ~3;
            }

            _paramsRing[slot] = new NativeMDIParams
            {
                argsBuffer = bufferWithArgs.GetNativeBufferPtr(),
                indexBuffer = indexBuffer.GetNativeBufferPtr(),
                argsOffsetBytes = (uint)(argsStartIndex * INDIRECT_DRAW_INDEXED_ARGS_SIZE),
                maxDrawCount = (uint)argsCount,
                indexFormat = indexFormat,
                topology = (uint)topology,
                flags = flags,
                _pad = MDI_RING_MAGIC,
                countBuffer = countBuffer != null ? countBuffer.GetNativeBufferPtr() : IntPtr.Zero,
                countOffsetBytes = (uint)countOffsetBytes,
                _pad2 = 0,
            };

            return (IntPtr)((NativeMDIParams*)_paramsRing.GetUnsafeReadOnlyPtr() + slot);
        }

        // -----------------------------------------------------------------
        // Prepare event — write→indirect-read barrier, outside render passes
        // -----------------------------------------------------------------

        // Event index of the prepare event within the plugin's range — one
        // past the per-ring-slot draw events (matches native MDI_MAX_PENDING).
        private const int PREPARE_ARGS_EVENT = MAX_PENDING;

        /// <summary>
        /// Records the barrier that makes GPU writes to <paramref name="argsBuffer"/>
        /// visible to indirect-draw fetch. Call it right AFTER the compute
        /// dispatch (or upload) that wrote the args and BEFORE the render pass
        /// that consumes them via MultiDraw*Indirect — once per buffer per
        /// frame is enough. On Vulkan this is the only place the barrier can
        /// be recorded without splitting a render pass (a split corrupts
        /// cached state and stalls the GPU); the draw itself never records
        /// barriers. No-op on backends that don't need it and when the native
        /// plugin is unavailable.
        /// </summary>
        public static void PrepareIndirectArgs(this CommandBuffer cmd, GraphicsBuffer argsBuffer)
        {
            IntPtr dataPtr = BeginPrepareIndirectArgs(argsBuffer);
            if (dataPtr == IntPtr.Zero)
                return;
            cmd.IssuePluginEventAndData(_renderEventAndDataFunc, _baseEventID + PREPARE_ARGS_EVENT, dataPtr);
        }

        // Stages the buffer into a ring slot; returns IntPtr.Zero when there
        // is nothing to do (unsupported, or no native event function).
        private static unsafe IntPtr BeginPrepareIndirectArgs(GraphicsBuffer argsBuffer)
        {
            EnsureInitialized();

            if (!_supported || argsBuffer == null || _renderEventAndDataFunc == IntPtr.Zero)
                return IntPtr.Zero;

            int slot = MDI_AllocSlot();
            _paramsRing[slot] = new NativeMDIParams
            {
                argsBuffer = argsBuffer.GetNativeBufferPtr(),
                _pad = MDI_RING_MAGIC,
            };
            return (IntPtr)((NativeMDIParams*)_paramsRing.GetUnsafeReadOnlyPtr() + slot);
        }

        // -----------------------------------------------------------------
        // Editor async-compile gate
        // -----------------------------------------------------------------

        // In the editor, async shader compilation makes Unity SKIP (or swap
        // for a placeholder) any draw whose variant is still compiling. The
        // prime draw is what leaves pipeline/index-buffer state on the
        // command buffer for the native multi-draw — a skipped prime means
        // the native draws inherit nothing, which is undefined behavior on
        // Vulkan (the native backend also refuses those draws). Use the loop
        // path for such frames: Unity renders the placeholder there itself,
        // and the batch returns to MDI once the pass is compiled.
        private static bool PrimeDrawWillRecord(Material material, int shaderPass)
        {
#if UNITY_EDITOR
            if (material == null)
                return false;
            int pass = shaderPass < 0 ? 0 : shaderPass;
            if (UnityEditor.ShaderUtil.IsPassCompiled(material, pass))
                return true;
            UnityEditor.ShaderUtil.CompilePass(material, pass); // kick async compile
            return false;
#else
            return true;
#endif
        }
    }
}
