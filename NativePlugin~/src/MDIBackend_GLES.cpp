#include "MDIBackend_GLES.h"
#include "MDILog.h"
#include <cstring>
#include <vector>

#ifdef _WIN32

// On Windows, Unity uses ANGLE or desktop GL. Resolve via GetProcAddress from the GL module.
static void* GLGetProcAddress(const char* name)
{
    // Try opengl32.dll first (desktop GL / ANGLE)
    static HMODULE glModule = nullptr;
    if (!glModule)
    {
        glModule = GetModuleHandleA("libGLESv2.dll"); // ANGLE
        if (!glModule)
            glModule = GetModuleHandleA("opengl32.dll"); // Desktop GL
    }

    if (glModule)
    {
        void* proc = (void*)GetProcAddress(glModule, name);
        if (proc) return proc;
    }

    // Fallback: try wglGetProcAddress
    typedef void* (__stdcall *PFN_wglGetProcAddress)(const char*);
    static PFN_wglGetProcAddress wglGetProc = nullptr;
    static bool wglResolved = false;
    if (!wglResolved)
    {
        wglResolved = true;
        HMODULE oglModule = GetModuleHandleA("opengl32.dll");
        if (oglModule)
            wglGetProc = (PFN_wglGetProcAddress)GetProcAddress(oglModule, "wglGetProcAddress");
    }
    if (wglGetProc)
        return wglGetProc(name);

    return nullptr;
}

#else
#include <dlfcn.h>

// On Android, use eglGetProcAddress. On desktop Linux (OpenGLCore via GLX),
// use glXGetProcAddress from libGL — Unity's GL context there is GLX-based,
// so EGL may be absent or resolve nothing.
static void* GLGetProcAddress(const char* name)
{
    typedef void (*(*PFN_getProcAddress)(const char*))();

    // Try eglGetProcAddress (Android, Wayland/EGL desktops)
    static PFN_getProcAddress eglGetProc = nullptr;
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        void* libEGL = dlopen("libEGL.so", RTLD_LAZY);
#if !defined(__ANDROID__)
        if (!libEGL)
            libEGL = dlopen("libEGL.so.1", RTLD_LAZY);
#endif
        if (libEGL)
            eglGetProc = (PFN_getProcAddress)dlsym(libEGL, "eglGetProcAddress");
    }
    if (eglGetProc)
    {
        if (void* proc = (void*)eglGetProc(name))
            return proc;
    }

#if !defined(__ANDROID__)
    // Desktop Linux fallback: glXGetProcAddress from libGL (GLX context)
    static PFN_getProcAddress glxGetProc = nullptr;
    static bool glxResolved = false;
    if (!glxResolved)
    {
        glxResolved = true;
        void* libGL = dlopen("libGL.so.1", RTLD_LAZY);
        if (!libGL)
            libGL = dlopen("libGL.so", RTLD_LAZY);
        if (libGL)
        {
            glxGetProc = (PFN_getProcAddress)dlsym(libGL, "glXGetProcAddressARB");
            if (!glxGetProc)
                glxGetProc = (PFN_getProcAddress)dlsym(libGL, "glXGetProcAddress");
        }
    }
    if (glxGetProc)
    {
        if (void* proc = (void*)glxGetProc(name))
            return proc;
    }
#endif

    // Fallback: try libGLESv2.so directly
    static void* libGLES = nullptr;
    if (!libGLES)
        libGLES = dlopen("libGLESv2.so", RTLD_LAZY);
    if (libGLES)
        return dlsym(libGLES, name);

    return nullptr;
}
#endif

// -----------------------------------------------------------------------
// GL function resolution
// -----------------------------------------------------------------------

bool MDIBackend_GLES::ResolveGLFunctions()
{
    _glBindBuffer = (PFNGLBINDBUFFERPROC)GLGetProcAddress("glBindBuffer");
    _glDrawElementsIndirect = (PFNGLDRAWELEMENTSINDIRECTPROC)GLGetProcAddress("glDrawElementsIndirect");

    if (!_glBindBuffer || !_glDrawElementsIndirect)
    {
        DebugLog("[MDI] GLES: failed to resolve core GL functions\n");
        return false;
    }

    // Try to get multi-draw indirect extension
    _glMultiDrawElementsIndirectEXT =
        (PFNGLMULTIDRAWELEMENTSINDIRECTEXTPROC)GLGetProcAddress("glMultiDrawElementsIndirectEXT");

    // Also try the non-EXT name (desktop GL 4.3 core)
    if (!_glMultiDrawElementsIndirectEXT)
        _glMultiDrawElementsIndirectEXT =
            (PFNGLMULTIDRAWELEMENTSINDIRECTEXTPROC)GLGetProcAddress("glMultiDrawElementsIndirect");

    // Optional: GPU-driven draw count (GL 4.6 core / GL_ARB_indirect_parameters)
    _glMultiDrawElementsIndirectCount =
        (PFNGLMULTIDRAWELEMENTSINDIRECTCOUNTPROC)GLGetProcAddress("glMultiDrawElementsIndirectCount");
    if (!_glMultiDrawElementsIndirectCount)
        _glMultiDrawElementsIndirectCount =
            (PFNGLMULTIDRAWELEMENTSINDIRECTCOUNTPROC)GLGetProcAddress("glMultiDrawElementsIndirectCountARB");
    DebugLog("[MDI] GLES: glMultiDrawElementsIndirectCount %s\n",
        _glMultiDrawElementsIndirectCount ? "supported" : "NOT supported (GPU count will use maxDrawCount)");

    // Identity buffer support
    _glGenBuffers = (PFNGLGENBUFFERSPROC)GLGetProcAddress("glGenBuffers");
    _glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)GLGetProcAddress("glDeleteBuffers");
    _glBufferData = (PFNGLBUFFERDATAPROC)GLGetProcAddress("glBufferData");
    _glVertexAttribIPointer = (PFNGLVERTEXATTRIBIPOINTERPROC)GLGetProcAddress("glVertexAttribIPointer");
    _glVertexAttribDivisor = (PFNGLVERTEXATTRIBDIVISORPROC)GLGetProcAddress("glVertexAttribDivisor");
    _glEnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)GLGetProcAddress("glEnableVertexAttribArray");
    _glGetIntegerv = (PFNGLGETINTEGERVPROC)GLGetProcAddress("glGetIntegerv");
    _glGetAttribLocation = (PFNGLGETATTRIBLOCATIONPROC)GLGetProcAddress("glGetAttribLocation");

    _glGetError = (PFNGLGETERRORPROC)GLGetProcAddress("glGetError");
    _glGenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)GLGetProcAddress("glGenVertexArrays");
    _glDeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)GLGetProcAddress("glDeleteVertexArrays");
    _glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)GLGetProcAddress("glBindVertexArray");
    _glIsVertexArray = (PFNGLISVERTEXARRAYPROC)GLGetProcAddress("glIsVertexArray");

    // Vertex attribute query / disable / non-integer pointer — needed for
    // mesh-path VAO cloning so we leave Unity's VAO untouched.
    _glVertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)GLGetProcAddress("glVertexAttribPointer");
    _glDisableVertexAttribArray = (PFNGLDISABLEVERTEXATTRIBARRAYPROC)GLGetProcAddress("glDisableVertexAttribArray");
    _glGetVertexAttribiv = (PFNGLGETVERTEXATTRIBIVPROC)GLGetProcAddress("glGetVertexAttribiv");
    _glGetVertexAttribPointerv = (PFNGLGETVERTEXATTRIBPOINTERVPROC)GLGetProcAddress("glGetVertexAttribPointerv");

    if (!_glGenBuffers || !_glDeleteBuffers || !_glBufferData ||
        !_glVertexAttribIPointer || !_glVertexAttribDivisor || !_glEnableVertexAttribArray ||
        !_glGetIntegerv || !_glGetAttribLocation ||
        !_glGenVertexArrays || !_glDeleteVertexArrays || !_glBindVertexArray ||
        !_glVertexAttribPointer || !_glDisableVertexAttribArray ||
        !_glGetVertexAttribiv || !_glGetVertexAttribPointerv)
    {
        DebugLog("[MDI] GLES: failed to resolve GL functions\n");
        return false;
    }

    return true;
}

bool MDIBackend_GLES::CheckMultiDrawIndirectExtension()
{
    if (_glMultiDrawElementsIndirectEXT)
    {
        // Function pointer resolved — extension is available
        DebugLog("[MDI] GLES: GL_EXT_multi_draw_indirect supported (hardware MDI)\n");
        return true;
    }

    DebugLog("[MDI] GLES: GL_EXT_multi_draw_indirect NOT supported (will use loop fallback)\n");
    return false;
}

// -----------------------------------------------------------------------
// Initialize / Shutdown
// -----------------------------------------------------------------------

void MDIBackend_GLES::CreateInstanceIDBuffer()
{
    std::vector<uint32_t> data(_maxInstanceCount);
    for (uint32_t i = 0; i < _maxInstanceCount; ++i)
        data[i] = i;

    // Save Unity's current GL_ARRAY_BUFFER binding so we can restore it
    GLint prevArrayBuffer = 0;
    _glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);

    _glGenBuffers(1, &_instanceIDBuffer);
    _glBindBuffer(GL_ARRAY_BUFFER, _instanceIDBuffer);
    _glBufferData(GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(_maxInstanceCount * sizeof(uint32_t)),
        data.data(), GL_STATIC_DRAW);

    // Restore Unity's previous binding
    _glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevArrayBuffer));

    DebugLog("[MDI] GLES Identity buffer ready: %u entries, %u bytes\n",
             _maxInstanceCount, _maxInstanceCount * (uint32_t)sizeof(uint32_t));
}

void MDIBackend_GLES::BindInstanceIDAttribute()
{
    GLint program = 0;
    _glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (program == 0) return;

    // Cache location per program — avoids glGetAttribLocation every frame
    GLint location = -1;
    if (static_cast<GLuint>(program) == _cachedProgram)
    {
        location = _cachedTexcoord7Location;
    }
    else
    {
        static const char* candidates[] = {
            "in_TEXCOORD7",
            "vs_TEXCOORD7",
            "TEXCOORD7",
        };

        for (const char* name : candidates)
        {
            location = _glGetAttribLocation(static_cast<GLuint>(program), name);
            if (location >= 0) break;
        }

        _cachedProgram = static_cast<GLuint>(program);
        _cachedTexcoord7Location = location;
    }

    if (location < 0) return;

    _glBindBuffer(GL_ARRAY_BUFFER, _instanceIDBuffer);
    _glVertexAttribIPointer(static_cast<GLuint>(location), 1, GL_UNSIGNED_INT, sizeof(uint32_t), nullptr);
    _glVertexAttribDivisor(static_cast<GLuint>(location), 1);
    _glEnableVertexAttribArray(static_cast<GLuint>(location));
    _glBindBuffer(GL_ARRAY_BUFFER, 0);
}

bool MDIBackend_GLES::ResizeInstanceIDBuffer(uint32_t newMaxCount)
{
    if (newMaxCount == 0) return false;
    if (newMaxCount == _maxInstanceCount && _instanceIDBuffer) return true;

    if (_instanceIDBuffer) { _glDeleteBuffers(1, &_instanceIDBuffer); _instanceIDBuffer = 0; }

    _maxInstanceCount = newMaxCount;
    CreateInstanceIDBuffer();

    // Cached mesh-path VAO clones reference the old _instanceIDBuffer in their
    // TEXCOORD7 binding — invalidate so they get rebuilt against the new buffer.
    InvalidateMeshVAOCache();

    // Force BindInstanceIDAttribute to re-resolve attribute location (and to
    // re-bind the new identity buffer on the indexed-path _mdiVAO next call).
    _cachedProgram = 0;
    _cachedTexcoord7Location = -1;

    return _instanceIDBuffer != 0;
}

void MDIBackend_GLES::InvalidateMeshVAOCache()
{
    if (!_glDeleteVertexArrays) return;
    for (auto& e : _meshVAOCache)
    {
        if (e.cloneVAO != 0)
        {
            _glDeleteVertexArrays(1, &e.cloneVAO);
        }
        e = MeshVAOCacheEntry{};
    }
    _meshVAOCacheNextSlot = 0;
}

bool MDIBackend_GLES::Initialize(IUnityInterfaces* unityInterfaces)
{
    (void)unityInterfaces;

    if (!ResolveGLFunctions())
        return false;

    _multiDrawIndirectSupported = CheckMultiDrawIndirectExtension();

    // Query max vertex attribs (clamped to 32 to keep stack arrays sane).
    GLint maxAttribs = 16;
    _glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxAttribs);
    if (maxAttribs <= 0)  maxAttribs = 16;
    if (maxAttribs > 32)  maxAttribs = 32;
    _maxVertexAttribs = maxAttribs;

    CreateInstanceIDBuffer();

    _initialized = true;
    DebugLog("[MDI] GLES backend initialized (MDI: %s, maxAttribs: %d)\n",
        _multiDrawIndirectSupported ? "hardware" : "loop fallback",
        _maxVertexAttribs);
    return true;
}

void MDIBackend_GLES::Shutdown()
{
    if (_mdiVAO && _glDeleteVertexArrays)
    {
        _glDeleteVertexArrays(1, &_mdiVAO);
        _mdiVAO = 0;
    }
    InvalidateMeshVAOCache();
    if (_instanceIDBuffer && _glDeleteBuffers)
    {
        _glDeleteBuffers(1, &_instanceIDBuffer);
        _instanceIDBuffer = 0;
    }
    _glDrawElementsIndirect = nullptr;
    _glMultiDrawElementsIndirectEXT = nullptr;
    _glBindBuffer = nullptr;
    _glGenBuffers = nullptr;
    _glDeleteBuffers = nullptr;
    _glBufferData = nullptr;
    _glVertexAttribIPointer = nullptr;
    _glVertexAttribDivisor = nullptr;
    _glEnableVertexAttribArray = nullptr;
    _glGetIntegerv = nullptr;
    _glGetAttribLocation = nullptr;
    _glGenVertexArrays = nullptr;
    _glDeleteVertexArrays = nullptr;
    _glBindVertexArray = nullptr;
    _glIsVertexArray = nullptr;
    _initialized = false;
    _multiDrawIndirectSupported = false;
    DebugLog("[MDI] GLES backend shutdown\n");
}

// -----------------------------------------------------------------------
// ExecuteMDI
// -----------------------------------------------------------------------

void MDIBackend_GLES::ExecuteMDI(const MDIParams& params)
{
    if (!_initialized || !params.argsBuffer || params.maxDrawCount == 0)
        return;

    GLuint argsBufferGL = static_cast<GLuint>(reinterpret_cast<uintptr_t>(params.argsBuffer));
    GLenum indexType = (params.indexFormat == 1) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
    const uint32_t stride = 20; // 5 * sizeof(uint32_t)

    const bool meshPath = (params.flags & MDI_FLAG_MESH_PATH) != 0;

    // Lazy (re)creation of identity buffer
    if (_instanceIDBuffer == 0)
        CreateInstanceIDBuffer();

    // Both paths route through their own dedicated VAO and restore Unity's
    // VAO on exit so we never pollute Unity's VAO state.
    GLint prevVAO = 0;
    _glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);

    if (meshPath)
    {
        if (prevVAO == 0)
        {
            DebugLog("[MDI] GLES mesh path: no VAO bound by Unity, skipping\n");
            return;
        }
        const GLuint unityVAO = static_cast<GLuint>(prevVAO);

        // Lightweight fingerprint of Unity's VAO state. Catches the realistic
        // invalidation cases:
        //   • mesh re-upload: slot 0 buffer / pointer / element buffer change
        //   • shader switch: GL_CURRENT_PROGRAM changes (TEXCOORD7 location may differ)
        //   • mesh recreation: unityVAO ID changes
        //   • identity-buffer resize: handled separately via InvalidateMeshVAOCache
        GLint fpProgram = 0, fpSlot0Buffer = 0, fpSlot0Stride = 0, fpElementBuffer = 0;
        void* fpSlot0Pointer = nullptr;
        _glGetIntegerv(GL_CURRENT_PROGRAM, &fpProgram);
        _glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &fpSlot0Buffer);
        _glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &fpSlot0Stride);
        _glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &fpSlot0Pointer);
        _glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &fpElementBuffer);

        MeshVAOCacheEntry* hit = nullptr;
        for (auto& e : _meshVAOCache)
        {
            if (e.unityVAOId == unityVAO && e.cloneVAO != 0 &&
                e.fpProgram        == fpProgram &&
                e.fpSlot0Buffer    == fpSlot0Buffer &&
                e.fpSlot0Stride    == fpSlot0Stride &&
                e.fpSlot0Pointer   == fpSlot0Pointer &&
                e.fpElementBuffer  == fpElementBuffer &&
                e.fpInstanceIDBuf  == _instanceIDBuffer)
            {
                hit = &e;
                break;
            }
        }

        if (hit && _glIsVertexArray(hit->cloneVAO))
        {
            // Fast path: TEXCOORD7 + cloned bindings are already on this VAO.
            _glBindVertexArray(hit->cloneVAO);
        }
        else
        {
            // Slow path: clone Unity's VAO bindings into a clone VAO.
            MeshVAOCacheEntry& slot = _meshVAOCache[_meshVAOCacheNextSlot];
            _meshVAOCacheNextSlot = (_meshVAOCacheNextSlot + 1) % kMeshVAOCacheSize;

            struct AttribSnapshot
            {
                GLint  enabled;
                GLint  size;
                GLint  stride;
                GLint  type;
                GLint  normalized;
                GLint  isInteger;
                GLint  divisor;
                GLint  buffer;
                void*  pointer;
            };

            // No zero-init — `enabled` is set first thing in the loop and gates
            // reads of all other fields.
            AttribSnapshot snapshots[32];
            const int attribCount = _maxVertexAttribs;

            for (int i = 0; i < attribCount; ++i)
            {
                _glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &snapshots[i].enabled);
                if (!snapshots[i].enabled) continue;

                _glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &snapshots[i].size);
                _glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &snapshots[i].stride);
                _glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &snapshots[i].type);
                _glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &snapshots[i].normalized);
                _glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &snapshots[i].isInteger);
                _glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &snapshots[i].divisor);
                _glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &snapshots[i].buffer);
                _glGetVertexAttribPointerv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &snapshots[i].pointer);
            }

            // Reuse the slot's existing clone VAO if still alive, otherwise create a new one.
            if (slot.cloneVAO == 0 || !_glIsVertexArray(slot.cloneVAO))
            {
                if (slot.cloneVAO != 0) _glDeleteVertexArrays(1, &slot.cloneVAO);
                _glGenVertexArrays(1, &slot.cloneVAO);
            }
            _glBindVertexArray(slot.cloneVAO);

            for (int i = 0; i < attribCount; ++i)
            {
                if (!snapshots[i].enabled)
                {
                    _glDisableVertexAttribArray((GLuint)i);
                    continue;
                }

                _glBindBuffer(GL_ARRAY_BUFFER, (GLuint)snapshots[i].buffer);
                if (snapshots[i].isInteger)
                {
                    _glVertexAttribIPointer((GLuint)i,
                        snapshots[i].size, (GLenum)snapshots[i].type,
                        snapshots[i].stride, snapshots[i].pointer);
                }
                else
                {
                    _glVertexAttribPointer((GLuint)i,
                        snapshots[i].size, (GLenum)snapshots[i].type,
                        (GLboolean)snapshots[i].normalized,
                        snapshots[i].stride, snapshots[i].pointer);
                }
                _glVertexAttribDivisor((GLuint)i, (GLuint)snapshots[i].divisor);
                _glEnableVertexAttribArray((GLuint)i);
            }
            _glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)fpElementBuffer);

            if (_instanceIDBuffer)
                BindInstanceIDAttribute();

            slot.unityVAOId       = unityVAO;
            slot.fpProgram        = fpProgram;
            slot.fpSlot0Buffer    = fpSlot0Buffer;
            slot.fpSlot0Stride    = fpSlot0Stride;
            slot.fpSlot0Pointer   = fpSlot0Pointer;
            slot.fpElementBuffer  = fpElementBuffer;
            slot.fpInstanceIDBuf  = _instanceIDBuffer;
        }
    }
    else
    {
        // Indexed (procedural) path: shader pulls vertex data via SV_VertexID,
        // so a minimal VAO suffices. Use our cached _mdiVAO.
        if (_mdiVAO != 0 && !_glIsVertexArray(_mdiVAO))
        {
            _mdiVAO = 0;
            DebugLog("[MDI] GLES: VAO invalidated, recreating\n");
        }
        if (_mdiVAO == 0)
            _glGenVertexArrays(1, &_mdiVAO);

        _glBindVertexArray(_mdiVAO);

        if (params.indexBuffer)
        {
            GLuint indexBufferGL = static_cast<GLuint>(reinterpret_cast<uintptr_t>(params.indexBuffer));
            _glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBufferGL);
        }

        if (_instanceIDBuffer)
            BindInstanceIDAttribute();
    }

    // Bind the indirect args buffer (global GL state, not VAO state).
    _glBindBuffer(GL_DRAW_INDIRECT_BUFFER, argsBufferGL);

    const auto drawMode = GetDrawMode(params.topology);

    const bool useGpuCount = (params.flags & MDI_FLAG_GPU_COUNT) != 0 &&
                             params.countBuffer != nullptr &&
                             _glMultiDrawElementsIndirectCount != nullptr;

    if (useGpuCount)
    {
        // GPU-driven draw count: hardware reads the actual count from the
        // GL_PARAMETER_BUFFER binding, clamped to maxDrawCount.
        GLuint countBufferGL = static_cast<GLuint>(reinterpret_cast<uintptr_t>(params.countBuffer));
        _glBindBuffer(GL_PARAMETER_BUFFER, countBufferGL);
        _glMultiDrawElementsIndirectCount(
            drawMode,
            indexType,
            reinterpret_cast<const void*>(static_cast<uintptr_t>(params.argsOffsetBytes)),
            static_cast<GLintptr>(params.countOffsetBytes),
            static_cast<GLsizei>(params.maxDrawCount),
            static_cast<GLsizei>(stride)
        );
        _glBindBuffer(GL_PARAMETER_BUFFER, 0);
    }
    else if (_multiDrawIndirectSupported)
    {
        _glMultiDrawElementsIndirectEXT(
            drawMode,
            indexType,
            reinterpret_cast<const void*>(static_cast<uintptr_t>(params.argsOffsetBytes)),
            static_cast<GLsizei>(params.maxDrawCount),
            static_cast<GLsizei>(stride)
        );
    }
    else
    {
        uintptr_t offset = params.argsOffsetBytes;
        for (uint32_t i = 0; i < params.maxDrawCount; ++i)
        {
            _glDrawElementsIndirect(
                drawMode,
                indexType,
                reinterpret_cast<const void*>(offset)
            );
            offset += stride;
        }
    }

    // Restore state — leaves Unity's VAO and GL_DRAW_INDIRECT_BUFFER as they were.
    _glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    _glBindVertexArray(static_cast<GLuint>(prevVAO));
}

GLuint MDIBackend_GLES::GetDrawMode(const uint32_t topology)
{
    switch (topology)
    {
        // MeshTopology.Triangles
        default:
        case 0:
            return GL_TRIANGLES;
            // MeshTopology.Lines
        case 3:
            return GL_LINES;
        case 4:
            return GL_LINE_STRIP;
        case 5:
            return GL_POINTS;
    }
}

bool MDIBackend_GLES::IsSupported() const
{
    return _initialized;
}
