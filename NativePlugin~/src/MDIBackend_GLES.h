#pragma once

#include "MDIBackend.h"
#include "gles/gles_minimal.h"

class MDIBackend_GLES final : public IMDIBackend
{
public:
    bool Initialize(IUnityInterfaces* unityInterfaces) override;
    void Shutdown() override;
    void ExecuteMDI(const MDIParams& params) override;
    bool IsSupported() const override;
    bool ResizeInstanceIDBuffer(uint32_t newMaxCount) override;
    uint32_t GetMaxInstanceCount() const override { return _maxInstanceCount; }

private:
    bool ResolveGLFunctions();
    bool CheckMultiDrawIndirectExtension();
    void CreateInstanceIDBuffer();
    void BindInstanceIDAttribute();

    static GLuint GetDrawMode(uint32_t topology);

    PFNGLDRAWELEMENTSINDIRECTPROC _glDrawElementsIndirect = nullptr;
    PFNGLMULTIDRAWELEMENTSINDIRECTEXTPROC _glMultiDrawElementsIndirectEXT = nullptr;
    PFNGLMULTIDRAWELEMENTSINDIRECTCOUNTPROC _glMultiDrawElementsIndirectCount = nullptr;
    PFNGLBINDBUFFERPROC _glBindBuffer = nullptr;
    PFNGLGENBUFFERSPROC _glGenBuffers = nullptr;
    PFNGLDELETEBUFFERSPROC _glDeleteBuffers = nullptr;
    PFNGLBUFFERDATAPROC _glBufferData = nullptr;
    PFNGLVERTEXATTRIBIPOINTERPROC _glVertexAttribIPointer = nullptr;
    PFNGLVERTEXATTRIBPOINTERPROC _glVertexAttribPointer = nullptr;
    PFNGLVERTEXATTRIBDIVISORPROC _glVertexAttribDivisor = nullptr;
    PFNGLENABLEVERTEXATTRIBARRAYPROC _glEnableVertexAttribArray = nullptr;
    PFNGLDISABLEVERTEXATTRIBARRAYPROC _glDisableVertexAttribArray = nullptr;
    PFNGLGETVERTEXATTRIBIVPROC _glGetVertexAttribiv = nullptr;
    PFNGLGETVERTEXATTRIBPOINTERVPROC _glGetVertexAttribPointerv = nullptr;
    PFNGLGETINTEGERVPROC _glGetIntegerv = nullptr;
    PFNGLGETATTRIBLOCATIONPROC _glGetAttribLocation = nullptr;
    PFNGLGETERRORPROC _glGetError = nullptr;
    PFNGLGENVERTEXARRAYSPROC _glGenVertexArrays = nullptr;
    PFNGLDELETEVERTEXARRAYSPROC _glDeleteVertexArrays = nullptr;
    PFNGLBINDVERTEXARRAYPROC _glBindVertexArray = nullptr;
    PFNGLISVERTEXARRAYPROC _glIsVertexArray = nullptr;

    // Per-instance identity buffer [0, 1, 2, ..., _maxInstanceCount-1]
    GLuint _instanceIDBuffer = 0;

    // Cached VAO for indexed (procedural) MDI draws — validated via glIsVertexArray.
    GLuint _mdiVAO = 0;

    // Mesh-path VAO clone cache. We clone Unity's mesh VAO into our own VAO
    // (so we can add TEXCOORD7 without polluting Unity's state) and reuse it
    // across calls when Unity's VAO hasn't changed. Lightweight fingerprint
    // (slot 0 binding + element buffer + program + identity buffer ID)
    // detects mesh re-upload / shader switch / identity-buffer resize.
    struct MeshVAOCacheEntry
    {
        GLuint  unityVAOId       = 0;
        GLuint  cloneVAO         = 0;
        GLint   fpProgram        = 0;
        GLint   fpSlot0Buffer    = 0;
        GLint   fpSlot0Stride    = 0;
        void*   fpSlot0Pointer   = nullptr;
        GLint   fpElementBuffer  = 0;
        GLuint  fpInstanceIDBuf  = 0;
    };
    static constexpr int kMeshVAOCacheSize = 4;
    MeshVAOCacheEntry _meshVAOCache[kMeshVAOCacheSize];
    int _meshVAOCacheNextSlot = 0;

    void InvalidateMeshVAOCache();

    // Max attribute slots; queried once and clamped to a sane upper bound.
    int _maxVertexAttribs = 16;

    // Cached TEXCOORD7 attribute location per shader program
    GLuint _cachedProgram = 0;
    GLint _cachedTexcoord7Location = -1;
    uint32_t _maxInstanceCount = kDefaultMaxInstanceCount;
    static constexpr uint32_t kDefaultMaxInstanceCount = 65536;

    bool _initialized = false;
    bool _multiDrawIndirectSupported = false;
};
