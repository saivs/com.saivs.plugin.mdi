// GfxPlugin Multi-Draw Indirect
// DLL must be named GfxPlugin*.dll for Unity to call UnityRenderingExtEvent.
#include "Unity/IUnityInterface.h"
#include "Unity/IUnityGraphics.h"
#include "Unity/IUnityRenderingExtensions.h"
#include "MDIBackend.h"
#include "MDIBackend_Stub.h"
#include "MDILog.h"

#ifdef _WIN32
#include "MDIBackend_D3D12.h"
#include "MDIBackend_D3D11.h"
#endif
#include "MDIBackend_Vulkan.h"
#include "MDIVulkanStateTracker.h"
#include "MDIBackend_GLES.h"
#if defined(__APPLE__)
#include "MDIBackend_Metal.h"
#endif

static IUnityInterfaces* g_unityInterfaces = nullptr;
static IUnityGraphics*   g_graphics        = nullptr;
static IMDIBackend*      g_backend         = nullptr;
static MDIBackend_Stub   g_stubBackend;
static int               g_backendSupported = 0;

// Pending params ring buffer — shared by D3D11/D3D12/Metal paths.
// Not static — Metal backend reads it from its method-swizzling hook.
MDIParams g_pending[MDI_MAX_PENDING] = {};
static volatile int g_pendingCounter = 0;

// Reserved event ID range — prevents clashes with Unity internals and other plugins
static int g_baseEventID = 0;

static IMDIBackend* CreateBackend(UnityGfxRenderer renderer)
{
    switch (renderer)
    {
#ifdef _WIN32
    case kUnityGfxRendererD3D12:
    {
        auto* backend = new MDIBackend_D3D12();
        if (backend->Initialize(g_unityInterfaces))
            return backend;
        delete backend;
        return &g_stubBackend;
    }
    case kUnityGfxRendererD3D11:
    {
        auto* backend = new MDIBackend_D3D11();
        if (backend->Initialize(g_unityInterfaces))
            return backend;
        delete backend;
        return &g_stubBackend;
    }
#endif
    case kUnityGfxRendererVulkan:
    {
        auto* backend = new MDIBackend_Vulkan();
        if (backend->Initialize(g_unityInterfaces))
        {
            // Event configuration is valid at device-init time (the UGE
            // plugin configures here too). Doing it at UnityPluginLoad is
            // not an option: the renderer is unknown there in the editor,
            // and touching the Vulkan interface under an active D3D device
            // crashes.
            backend->ConfigureEvents(g_unityInterfaces, g_baseEventID, MDI_MAX_PENDING);
            return backend;
        }
        delete backend;
        return &g_stubBackend;
    }
    case kUnityGfxRendererOpenGLES30:
    case kUnityGfxRendererOpenGLCore:
    {
        auto* backend = new MDIBackend_GLES();
        if (backend->Initialize(g_unityInterfaces))
            return backend;
        delete backend;
        return &g_stubBackend;
    }
#if defined(__APPLE__)
    case kUnityGfxRendererMetal:
    {
        auto* backend = new MDIBackend_Metal();
        if (backend->Initialize(g_unityInterfaces))
            return backend;
        delete backend;
        return &g_stubBackend;
    }
#endif
    default:
        return &g_stubBackend;
    }
}

static void DestroyBackend()
{
    if (g_backend && g_backend != &g_stubBackend)
    {
        g_backend->Shutdown();
        delete g_backend;
    }
    g_backend = &g_stubBackend;
}

static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
{
    if (eventType == kUnityGfxDeviceEventInitialize)
    {
        DestroyBackend();
        g_backend = CreateBackend(g_graphics->GetRenderer());
        g_backendSupported = (g_backend && g_backend != &g_stubBackend && g_backend->IsSupported()) ? 1 : 0;
    }
    else if (eventType == kUnityGfxDeviceEventShutdown)
    {
        DestroyBackend();
        g_backendSupported = 0;
    }
}

// -----------------------------------------------------------------------
// Unity Plugin Load / Unload
// -----------------------------------------------------------------------

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
    g_unityInterfaces = unityInterfaces;
    g_graphics = unityInterfaces->Get<IUnityGraphics>();

    // Reserve unique event IDs to avoid clashes with other plugins.
    // +1: the trailing prepare event (see MDIBackend::PrepareIndirectArgs).
    g_baseEventID = g_graphics->ReserveEventIDRange(MDI_MAX_PENDING + 1);

#ifdef _WIN32
    // D3D12: ConfigureEvent must be called during UnityPluginLoad, before device init
    if (g_graphics->GetRenderer() == kUnityGfxRendererD3D12)
    {
        MDIBackend_D3D12 configurator;
        configurator.ConfigureEvents(unityInterfaces, g_baseEventID, MDI_MAX_PENDING);
    }
#endif

    // Vulkan: interception must be registered before the Vulkan device
    // initializes. GetRenderer() reports Null when the plugin loads before
    // device init (preloaded GfxPlugin*), and the concrete API once the
    // device exists (lazy load on first P/Invoke). Register for Vulkan and
    // for Null — but NEVER when another API is already live: touching the
    // Vulkan interface on an active D3D12 editor crashes inside Unity.
    // (Event configuration happens later, at device init — see CreateBackend.)
    {
        const UnityGfxRenderer rendererAtLoad = g_graphics->GetRenderer();
        if (rendererAtLoad == kUnityGfxRendererVulkan || rendererAtLoad == kUnityGfxRendererNull)
            MDIVulkanInterceptRegister(unityInterfaces);
    }

    g_graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);

    if (g_graphics->GetRenderer() != kUnityGfxRendererNull)
        OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginUnload()
{
    if (g_graphics)
        g_graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);

    DestroyBackend();
    g_unityInterfaces = nullptr;
    g_graphics = nullptr;
}

// -----------------------------------------------------------------------
// Unity Rendering Extensions — stub (required export for GfxPlugin prefix)
// -----------------------------------------------------------------------

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityRenderingExtEvent(UnityRenderingExtEventType event, void* data)
{
    (void)event;
    (void)data;
}

// -----------------------------------------------------------------------
// Render event callback — single UnityRenderingEventAndData for both APIs
// -----------------------------------------------------------------------

// Both D3D11 and D3D12 use IssuePluginEventAndData with this callback.
// - D3D11: params arrive via data pointer (pinned NativeArray from C#)
// - D3D12: params arrive via data pointer (same mechanism)
// The eventID encodes the slot: slot = (eventID - g_baseEventID) % MDI_MAX_PENDING
static void UNITY_INTERFACE_API OnRenderEventAndData(int eventID, void* data)
{
    if (!g_backendSupported || !g_backend)
        return;

    int localEvent = eventID - g_baseEventID;

    // Trailing prepare event: records the write→indirect-read barrier for an
    // args buffer, outside any render pass. The ring slot carries only the
    // buffer pointer for this one.
    if (localEvent == MDI_MAX_PENDING)
    {
        if (data)
            g_backend->PrepareIndirectArgs(static_cast<const MDIParams*>(data)->argsBuffer);
        return;
    }

    int slot = localEvent % MDI_MAX_PENDING;
    if (slot < 0) slot += MDI_MAX_PENDING;

    // Copy params from data pointer into pending slot
    if (data)
        g_pending[slot] = *static_cast<const MDIParams*>(data);

    if (g_pending[slot].argsBuffer && g_pending[slot].maxDrawCount > 0)
        g_backend->ExecuteMDI(g_pending[slot]);

    // Clear slot
    g_pending[slot].argsBuffer = nullptr;
    g_pending[slot].maxDrawCount = 0;
}

// -----------------------------------------------------------------------
// Exported C API
// -----------------------------------------------------------------------

extern "C" int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
MDI_AllocSlot()
{
    return (g_pendingCounter++) % MDI_MAX_PENDING;
}

#if defined(__APPLE__)
extern "C" void MDIMetal_SetDummyArgsBuffer(void* nativePtr);
extern "C" void MDIMetal_SetParamsRing(const MDIParams* basePtr);
extern "C" void MDIMetal_SetDrawIndexBuffer(void* nativePtr);
#endif

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
MDI_SetDummyArgsBuffer(void* nativePtr)
{
#if defined(__APPLE__)
    MDIMetal_SetDummyArgsBuffer(nativePtr);
#else
    (void)nativePtr;
#endif
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
MDI_SetParamsRing(const MDIParams* basePtr)
{
#if defined(__APPLE__)
    MDIMetal_SetParamsRing(basePtr);
#else
    (void)basePtr;
#endif
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
MDI_SetDrawIndexBuffer(void* nativePtr)
{
#if defined(__APPLE__)
    MDIMetal_SetDrawIndexBuffer(nativePtr);
#else
    (void)nativePtr;
#endif
}

extern "C" int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
MDI_GetBaseEventID()
{
    return g_baseEventID;
}

// Returns UnityRenderingEventAndData callback pointer
extern "C" UNITY_INTERFACE_EXPORT UnityRenderingEventAndData UNITY_INTERFACE_API
MDI_GetRenderEventAndDataFunc()
{
    return OnRenderEventAndData;
}

extern "C" int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
MDI_IsSupported()
{
    return g_backendSupported;
}

extern "C" int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
MDI_IsD3D12()
{
    return (g_graphics && g_graphics->GetRenderer() == kUnityGfxRendererD3D12) ? 1 : 0;
}

extern "C" int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
MDI_UsesPerInstanceVB()
{
    if (!g_graphics) return 0;
    auto renderer = g_graphics->GetRenderer();
    // Only D3D11/D3D12 need the GetPrimeMesh() path to trigger PSO hooks.
    // OpenGL uses its own VAO for identity buffer binding — no PSO hook needed.
    // Vulkan doesn't need identity buffer at all (SV_InstanceID includes firstInstance).
    return (renderer == kUnityGfxRendererD3D11 || renderer == kUnityGfxRendererD3D12) ? 1 : 0;
}

extern "C" int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
MDI_SetMaxInstanceCount(uint32_t maxCount)
{
    if (!g_backend) return 0;
    return g_backend->ResizeInstanceIDBuffer(maxCount) ? 1 : 0;
}

extern "C" uint32_t UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
MDI_GetMaxInstanceCount()
{
    if (!g_backend) return 0;
    return g_backend->GetMaxInstanceCount();
}

// Routes plugin logs into Unity's Debug.Log via a C# callback. Pass nullptr
// to fall back to the platform default (OutputDebugStringA on Windows).
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
MDI_SetLogCallback(MDILogCallback callback)
{
    g_mdiLogCallback = callback;
}
