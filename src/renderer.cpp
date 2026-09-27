#include <imvideo/renderer.hpp>

#include "frame_internal.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__APPLE__)
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

// These unpack-state enums and pixel buffer objects postdate the Windows SDK's
// OpenGL 1.1 header. Define their specified values without requiring glext.h.
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

#if defined(_WIN32)
using BindBufferProc = void(APIENTRY*)(GLenum, GLuint);

BindBufferProc get_bind_buffer_proc() {
    auto proc = reinterpret_cast<BindBufferProc>(wglGetProcAddress("glBindBuffer"));
    if (proc == nullptr || proc == reinterpret_cast<BindBufferProc>(1) ||
        proc == reinterpret_cast<BindBufferProc>(2) || proc == reinterpret_cast<BindBufferProc>(3) ||
        proc == reinterpret_cast<BindBufferProc>(-1)) {
        return nullptr;
    }
    return proc;
}
#else
using BindBufferProc = void(*)(GLenum, GLuint);
extern "C" void glBindBuffer(GLenum, GLuint);

BindBufferProc get_bind_buffer_proc() { return &glBindBuffer; }
#endif

// The Windows SDK ships an OpenGL 1.1 header, while this core enum was added
// in OpenGL 1.2.  Keep the public renderer compatible with that system header.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace imvideo {

struct Renderer::Impl {
    ~Impl() {
        if (texture_id != 0) glDeleteTextures(1, &texture_id);
    }

    bool update(AVFrame* source) {
        if (!source || source->format != AV_PIX_FMT_RGBA || source->width <= 0 || source->height <= 0 ||
            source->width > 16384 || source->height > 16384)
            return false;
        if (!source->data[0] || source->linesize[0] < source->width * 4)
            return false;

        const int next_width = source->width;
        const int next_height = source->height;
        pixels.resize(static_cast<std::size_t>(next_width) * static_cast<std::size_t>(next_height) * 4);
        for (int row = 0; row < next_height; ++row) {
            std::memcpy(pixels.data() + static_cast<std::size_t>(row) * next_width * 4,
                        source->data[0] + static_cast<std::size_t>(row) * source->linesize[0],
                        static_cast<std::size_t>(next_width) * 4);
        }
        if (!upload(next_width, next_height)) return false;
        width = next_width;
        height = next_height;
        return true;
    }

    bool upload(int upload_width, int upload_height) {
        while (glGetError() != GL_NO_ERROR) {}
        const BindBufferProc bind_buffer = get_bind_buffer_proc();
        if (!bind_buffer) return false;
        GLint previous_binding = 0;
        GLint previous_alignment = 4;
        GLint previous_row_length = 0;
        GLint previous_skip_rows = 0;
        GLint previous_skip_pixels = 0;
        GLint previous_unpack_buffer = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_binding);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_alignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &previous_row_length);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &previous_skip_rows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &previous_skip_pixels);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &previous_unpack_buffer);
        bind_buffer(GL_PIXEL_UNPACK_BUFFER, 0);

        const bool create_texture = texture_id == 0;
        if (create_texture) glGenTextures(1, &texture_id);
        if (texture_id != 0) {
            glBindTexture(GL_TEXTURE_2D, texture_id);
            if (create_texture) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        if (texture_id != 0) {
            if (upload_width == width && upload_height == height) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, upload_width, upload_height, GL_RGBA,
                                GL_UNSIGNED_BYTE, pixels.data());
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, upload_width, upload_height, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, pixels.data());
            }
        }
        const bool uploaded = texture_id != 0 && glGetError() == GL_NO_ERROR;
        glPixelStorei(GL_UNPACK_ALIGNMENT, previous_alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, previous_row_length);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, previous_skip_rows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, previous_skip_pixels);
        bind_buffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(previous_unpack_buffer));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_binding));
        if (!uploaded) {
            if (create_texture && texture_id != 0) {
                glDeleteTextures(1, &texture_id);
                texture_id = 0;
            }
            while (glGetError() != GL_NO_ERROR) {}
            return false;
        }
        return true;
    }

    GLuint texture_id = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

bool Renderer::update(const Frame& frame) {
    return frame.impl_ && frame.impl_->av_frame && impl_->update(frame.impl_->av_frame);
}

std::uintptr_t Renderer::texture() const noexcept { return impl_->texture_id; }
int Renderer::width() const noexcept { return impl_->width; }
int Renderer::height() const noexcept { return impl_->height; }

} // namespace imvideo
