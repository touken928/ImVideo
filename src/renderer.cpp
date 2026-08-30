#include <imvideo/renderer.hpp>

#include "frame_internal.hpp"

#include <cstdint>
#include <vector>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#elif defined(_WIN32)
#include <windows.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#endif

// The Windows SDK ships an OpenGL 1.1 header, while this core enum was added
// in OpenGL 1.2.  Keep the public renderer compatible with that system header.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace imvideo {

struct Renderer::Impl {
    ~Impl() {
        if (texture_id != 0) glDeleteTextures(1, &texture_id);
        sws_freeContext(sws);
        av_frame_free(&software_frame);
    }

    bool update(AVFrame* source) {
        AVFrame* input = source;
        const auto format = static_cast<AVPixelFormat>(source->format);
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
        if (descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0) {
            if (!software_frame) software_frame = av_frame_alloc();
            av_frame_unref(software_frame);
            if (av_hwframe_transfer_data(software_frame, source, 0) < 0) return false;
            input = software_frame;
        }

        if (input->width <= 0 || input->height <= 0) return false;
        const auto input_format = static_cast<AVPixelFormat>(input->format);
        sws = sws_getCachedContext(sws, input->width, input->height, input_format,
                                   input->width, input->height, AV_PIX_FMT_RGBA,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return false;

        width = input->width;
        height = input->height;
        pixels.resize(static_cast<std::size_t>(width) * height * 4);
        std::uint8_t* output[] = {pixels.data()};
        int strides[] = {width * 4};
        if (sws_scale(sws, input->data, input->linesize, 0, height, output, strides) <= 0) return false;

        if (texture_id == 0) {
            glGenTextures(1, &texture_id);
            glBindTexture(GL_TEXTURE_2D, texture_id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_2D, texture_id);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (allocated_width != width || allocated_height != height) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            allocated_width = width;
            allocated_height = height;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        return glGetError() == GL_NO_ERROR;
    }

    GLuint texture_id = 0;
    int width = 0;
    int height = 0;
    int allocated_width = 0;
    int allocated_height = 0;
    SwsContext* sws = nullptr;
    AVFrame* software_frame = nullptr;
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
