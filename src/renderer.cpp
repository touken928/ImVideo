#include <imvideo/renderer.hpp>

#include "ffmpeg_raii.hpp"
#include "frame_internal.hpp"
#include "gl_upload_state.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include "renderer_yuv.hpp"

#include <emscripten/html5_webgl.h>
#endif

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace imvideo {

struct Renderer::Impl {
    ~Impl() {
#if defined(__EMSCRIPTEN__)
        if (emscripten_is_webgl_context_lost(emscripten_webgl_get_current_context())) return;
#endif
        if (texture_id != 0) glDeleteTextures(1, &texture_id);
    }

    bool update(AVFrame* source, const DisplayGeometry& geometry) {
#if defined(__EMSCRIPTEN__)
        if (emscripten_is_webgl_context_lost(emscripten_webgl_get_current_context())) return false;
#endif
        if (!source || source->width <= 0 || source->height <= 0 || source->width > maximum_display_dimension ||
            source->height > maximum_display_dimension)
            return false;
        if (maximum_size == 0) glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_size);
        if (source->width > maximum_size || source->height > maximum_size) return false;
#if defined(__EMSCRIPTEN__)
        if (source->format == AV_PIX_FMT_YUV420P || source->format == AV_PIX_FMT_YUVJ420P) {
            if (geometry.width > maximum_size || geometry.height > maximum_size) return false;
            if (yuv.update(source, geometry, texture_id, geometry.width != width || geometry.height != height)) {
                width = geometry.width;
                height = geometry.height;
                return true;
            }
            // Keep the public RGBA-texture contract if a host's GL state cannot
            // accommodate the offscreen draw (for example transform feedback).
            const AvFramePtr rgba{fallback.rgba(source, geometry)};
            return rgba && update(rgba.get(), {rgba->width, rgba->height, 0});
        }
#else
        (void)geometry;
#endif
        if (source->format != AV_PIX_FMT_RGBA || !source->data[0] || source->linesize[0] < source->width * 4)
            return false;

        const int next_width = source->width;
        const int next_height = source->height;
        const std::uint8_t* data = source->data[0];
        int row_length = source->linesize[0] / 4;
        if (source->linesize[0] % 4 != 0) {
            pixels.resize(static_cast<std::size_t>(next_width) * next_height * 4);
            for (int row = 0; row < next_height; ++row) {
                std::memcpy(pixels.data() + static_cast<std::size_t>(row) * next_width * 4,
                            source->data[0] + static_cast<std::size_t>(row) * source->linesize[0],
                            static_cast<std::size_t>(next_width) * 4);
            }
            data = pixels.data();
            row_length = next_width;
        }
        if (!upload(next_width, next_height, data, row_length)) return false;
        width = next_width;
        height = next_height;
        return true;
    }

    bool upload(int upload_width, int upload_height, const std::uint8_t* data, int row_length) {
        clear_gl_errors();
        if (!bind_buffer) bind_buffer = load_bind_buffer();
        if (!bind_buffer) return false;
        const PixelUnpackState unpack{bind_buffer};
        GLint previous_binding = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_binding);

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
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
        if (texture_id != 0) {
            if (upload_width == width && upload_height == height) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, upload_width, upload_height, GL_RGBA, GL_UNSIGNED_BYTE, data);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, upload_width, upload_height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                             data);
            }
        }
        const bool uploaded = texture_id != 0 && glGetError() == GL_NO_ERROR;
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_binding));
        if (!uploaded) {
            if (create_texture && texture_id != 0) {
                glDeleteTextures(1, &texture_id);
                texture_id = 0;
            }
            clear_gl_errors();
            return false;
        }
        return true;
    }

    BindBufferProc bind_buffer = nullptr;
    GLuint texture_id = 0;
    int width = 0;
    int height = 0;
    GLint maximum_size = 0;
    std::vector<std::uint8_t> pixels;
#if defined(__EMSCRIPTEN__)
    YuvRenderer yuv;
    DisplayConverter fallback;
#endif
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

bool Renderer::update(const Frame& frame) {
    return frame.impl_ && frame.impl_->av_frame && impl_->update(frame.impl_->av_frame, frame.impl_->geometry);
}

void Renderer::invalidate() noexcept {
    if (!impl_) return;
    impl_->bind_buffer = nullptr;
    impl_->texture_id = 0;
    impl_->width = impl_->height = 0;
    impl_->maximum_size = 0;
    impl_->pixels.clear();
#if defined(__EMSCRIPTEN__)
    impl_->yuv.invalidate();
    impl_->fallback.reset();
#endif
}

std::uintptr_t Renderer::texture() const noexcept { return impl_->texture_id; }
int Renderer::width() const noexcept { return impl_->width; }
int Renderer::height() const noexcept { return impl_->height; }

} // namespace imvideo
