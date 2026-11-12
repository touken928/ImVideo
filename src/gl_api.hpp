#pragma once

#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#elif defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#endif

// The Windows SDK's GL 1.1 header omits these later core enums.
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_UNPACK_SKIP_ROWS
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#endif
#ifndef GL_UNPACK_SKIP_PIXELS
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER_BINDING
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
extern "C" void glBindBuffer(GLenum, GLuint);
#endif

namespace imvideo {

#if defined(_WIN32)
using BindBufferProc = void(APIENTRY*)(GLenum, GLuint);
#else
using BindBufferProc = void (*)(GLenum, GLuint);
#endif

inline BindBufferProc load_bind_buffer() noexcept {
#if defined(_WIN32)
    auto proc = reinterpret_cast<BindBufferProc>(wglGetProcAddress("glBindBuffer"));
    if (proc == nullptr || proc == reinterpret_cast<BindBufferProc>(1) || proc == reinterpret_cast<BindBufferProc>(2) ||
        proc == reinterpret_cast<BindBufferProc>(3) || proc == reinterpret_cast<BindBufferProc>(-1))
        return nullptr;
    return proc;
#else
    return &glBindBuffer;
#endif
}

inline void clear_gl_errors() noexcept {
    while (glGetError() != GL_NO_ERROR) {}
}

} // namespace imvideo
