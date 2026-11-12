#pragma once

#include "frame_internal.hpp"
#include "gl_api.hpp"

namespace imvideo {

class YuvRenderer {
public:
    ~YuvRenderer();
    bool update(const AVFrame* frame, const DisplayGeometry& geometry, GLuint& texture, bool allocate_target);
    void invalidate() noexcept;

private:
    bool initialize();
    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint framebuffer_ = 0;
    GLuint attached_texture_ = 0;
    GLuint planes_[3]{};
    int plane_widths_[3]{};
    int plane_heights_[3]{};
    GLint matrix_location_ = -1;
    GLint offset_location_ = -1;
    GLint rotation_location_ = -1;
    int color_key_ = -1;
    int rotation_ = -1;
};

} // namespace imvideo
