// Minimal OpenGL ES 3.1+ type definitions for MDI plugin.
// Only the types and constants needed for indirect draw calls.
// Avoids requiring the full GLES/EGL SDK.
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#define GL_APIENTRY __stdcall
#else
#define GL_APIENTRY
#endif

// Basic GL types
typedef unsigned int   GLenum;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLboolean;
typedef char           GLchar;
typedef intptr_t       GLintptr;
typedef ptrdiff_t      GLsizeiptr;

// Constants
#define GL_TRIANGLES                  0x0004
#define GL_TRIANGLE_STRIP             0x0005
#define GL_LINES                      0x0001
#define GL_LINE_STRIP                 0x0003
#define GL_POINTS                     0x0000

#define GL_UNSIGNED_SHORT             0x1403
#define GL_UNSIGNED_INT               0x1405

#define GL_DRAW_INDIRECT_BUFFER       0x8F3F
#define GL_EXTENSIONS                 0x1F03

#define GL_FALSE                      0
#define GL_TRUE                       1

// Buffer targets
#define GL_ARRAY_BUFFER               0x8892
#define GL_ELEMENT_ARRAY_BUFFER       0x8893

// Data types
#define GL_FLOAT                      0x1406
#define GL_INT                        0x1404

// Buffer usage
#define GL_STATIC_DRAW                0x88E4

// Function pointer types

// glDrawElementsIndirect — OpenGL ES 3.1 core
typedef void (GL_APIENTRY *PFNGLDRAWELEMENTSINDIRECTPROC)(
    GLenum mode, GLenum type, const void* indirect);

// glMultiDrawElementsIndirectEXT — GL_EXT_multi_draw_indirect
typedef void (GL_APIENTRY *PFNGLMULTIDRAWELEMENTSINDIRECTEXTPROC)(
    GLenum mode, GLenum type, const void* indirect,
    GLsizei drawcount, GLsizei stride);

// glMultiDrawElementsIndirectCount — GL 4.6 core / GL_ARB_indirect_parameters.
// The draw count is read from the buffer bound to GL_PARAMETER_BUFFER at
// byte offset `drawcount`, clamped to `maxdrawcount`.
#define GL_PARAMETER_BUFFER           0x80EE
typedef void (GL_APIENTRY *PFNGLMULTIDRAWELEMENTSINDIRECTCOUNTPROC)(
    GLenum mode, GLenum type, const void* indirect,
    GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride);

// glBindBuffer
typedef void (GL_APIENTRY *PFNGLBINDBUFFERPROC)(GLenum target, GLuint buffer);

// glGetString
typedef const GLchar* (GL_APIENTRY *PFNGLGETSTRINGPROC)(GLenum name);

// Identity buffer support
typedef void (GL_APIENTRY *PFNGLGENBUFFERSPROC)(GLsizei n, GLuint* buffers);
typedef void (GL_APIENTRY *PFNGLDELETEBUFFERSPROC)(GLsizei n, const GLuint* buffers);
typedef void (GL_APIENTRY *PFNGLBUFFERDATAPROC)(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
typedef void (GL_APIENTRY *PFNGLVERTEXATTRIBIPOINTERPROC)(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer);
typedef void (GL_APIENTRY *PFNGLVERTEXATTRIBDIVISORPROC)(GLuint index, GLuint divisor);
typedef void (GL_APIENTRY *PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint index);

// State query constants
#define GL_CURRENT_PROGRAM                0x8B8D
#define GL_ARRAY_BUFFER_BINDING           0x8894
#define GL_VERTEX_ARRAY_BINDING           0x85B5
#define GL_ELEMENT_ARRAY_BUFFER_BINDING   0x8895

// Vertex attribute query parameters
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED         0x8622
#define GL_VERTEX_ATTRIB_ARRAY_SIZE            0x8623
#define GL_VERTEX_ATTRIB_ARRAY_STRIDE          0x8624
#define GL_VERTEX_ATTRIB_ARRAY_TYPE            0x8625
#define GL_VERTEX_ATTRIB_ARRAY_NORMALIZED      0x886A
#define GL_VERTEX_ATTRIB_ARRAY_INTEGER         0x88FD
#define GL_VERTEX_ATTRIB_ARRAY_DIVISOR         0x88FE
#define GL_VERTEX_ATTRIB_ARRAY_POINTER         0x8645
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING  0x889F
#define GL_MAX_VERTEX_ATTRIBS                  0x8869

typedef void (GL_APIENTRY *PFNGLGETVERTEXATTRIBIVPROC)(GLuint index, GLenum pname, GLint* params);
typedef void (GL_APIENTRY *PFNGLGETVERTEXATTRIBPOINTERVPROC)(GLuint index, GLenum pname, void** pointer);
typedef void (GL_APIENTRY *PFNGLDISABLEVERTEXATTRIBARRAYPROC)(GLuint index);
typedef void (GL_APIENTRY *PFNGLVERTEXATTRIBPOINTERPROC)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);

// Error checking
#define GL_NO_ERROR                       0
typedef GLenum (GL_APIENTRY *PFNGLGETERRORPROC)();

// VAO management
typedef void (GL_APIENTRY *PFNGLGENVERTEXARRAYSPROC)(GLsizei n, GLuint* arrays);
typedef void (GL_APIENTRY *PFNGLDELETEVERTEXARRAYSPROC)(GLsizei n, const GLuint* arrays);
typedef void (GL_APIENTRY *PFNGLBINDVERTEXARRAYPROC)(GLuint array);
typedef GLboolean (GL_APIENTRY *PFNGLISVERTEXARRAYPROC)(GLuint array);

// State query and attribute location
typedef void (GL_APIENTRY *PFNGLGETINTEGERVPROC)(GLenum pname, GLint* data);
typedef GLint (GL_APIENTRY *PFNGLGETATTRIBLOCATIONPROC)(GLuint program, const GLchar* name);

// Platform-specific proc address resolver
#ifdef _WIN32
// On Windows (ANGLE/desktop GLES emulation), use wglGetProcAddress or loaded from DLL
typedef void* (*PFN_GetProcAddress)(const char* name);
#else
// On Android, use eglGetProcAddress
typedef void (*(*PFN_eglGetProcAddress)(const char* name))();
#endif
