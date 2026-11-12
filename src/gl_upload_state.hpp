#pragma once

#include "gl_api.hpp"

namespace imvideo {

// Both RGBA and planar uploads must ignore and restore the host's unpack/PBO
// state, including WebGL's additional image-conversion flags.
class PixelUnpackState {
public:
    explicit PixelUnpackState(BindBufferProc bind_buffer) : bind_buffer_(bind_buffer) {
        if (!bind_buffer_) return;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment_);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length_);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows_);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels_);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer_);
        bind_buffer_(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
#if defined(__EMSCRIPTEN__)
        glGetIntegerv(flip_y, &flip_);
        glGetIntegerv(premultiply_alpha, &premultiply_);
        glGetIntegerv(colorspace_conversion, &colorspace_);
        glPixelStorei(flip_y, GL_FALSE);
        glPixelStorei(premultiply_alpha, GL_FALSE);
        glPixelStorei(colorspace_conversion, GL_NONE);
#endif
    }

    ~PixelUnpackState() {
        if (!bind_buffer_) return;
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length_);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows_);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels_);
        bind_buffer_(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(buffer_));
#if defined(__EMSCRIPTEN__)
        glPixelStorei(flip_y, flip_);
        glPixelStorei(premultiply_alpha, premultiply_);
        glPixelStorei(colorspace_conversion, colorspace_);
#endif
    }

    PixelUnpackState(const PixelUnpackState&) = delete;
    PixelUnpackState& operator=(const PixelUnpackState&) = delete;

private:
    BindBufferProc bind_buffer_ = nullptr;
    GLint alignment_ = 0;
    GLint row_length_ = 0;
    GLint skip_rows_ = 0;
    GLint skip_pixels_ = 0;
    GLint buffer_ = 0;
#if defined(__EMSCRIPTEN__)
    static constexpr GLenum flip_y = 0x9240;
    static constexpr GLenum premultiply_alpha = 0x9241;
    static constexpr GLenum colorspace_conversion = 0x9243;
    GLint flip_ = 0;
    GLint premultiply_ = 0;
    GLint colorspace_ = 0;
#endif
};

} // namespace imvideo
