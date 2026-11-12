#include "renderer_yuv.hpp"

#include "gl_upload_state.hpp"

#include <cstdio>
#include <emscripten/html5_webgl.h>

namespace imvideo {
namespace {

// This conversion is also usable by hosts with their own shaders, FBOs,
// scissor rectangles and blend state. It must leave that state untouched.
struct SavedState {
    GLint program = 0, vao = 0, framebuffer = 0, active_texture = 0;
    GLint textures[3]{}, samplers[3]{}, viewport[4]{};
    PixelUnpackState unpack{load_bind_buffer()};
    GLboolean color_mask[4]{};
    const GLenum capabilities[7] = {GL_BLEND,     GL_DEPTH_TEST,         GL_STENCIL_TEST, GL_SCISSOR_TEST,
                                    GL_CULL_FACE, GL_RASTERIZER_DISCARD, GL_DITHER};
    GLboolean enabled[7]{};

    SavedState() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
        for (int unit = 0; unit < 3; ++unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[unit]);
            glGetIntegerv(GL_SAMPLER_BINDING, &samplers[unit]);
            glBindSampler(unit, 0);
        }
        for (int index = 0; index < 7; ++index) {
            enabled[index] = glIsEnabled(capabilities[index]);
            if (enabled[index]) glDisable(capabilities[index]);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    ~SavedState() {
        glUseProgram(static_cast<GLuint>(program));
        glBindVertexArray(static_cast<GLuint>(vao));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
        for (int index = 0; index < 7; ++index) {
            if (enabled[index]) glEnable(capabilities[index]);
        }
        for (int unit = 0; unit < 3; ++unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(textures[unit]));
            glBindSampler(unit, static_cast<GLuint>(samplers[unit]));
        }
        glActiveTexture(static_cast<GLenum>(active_texture));
    }
};

GLuint compile(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return shader;
    char log[1024]{};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    std::fprintf(stderr, "YUV shader: %s\n", log);
    glDeleteShader(shader);
    return 0;
}

void parameters(GLint filter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

} // namespace

YuvRenderer::~YuvRenderer() {
    if (emscripten_is_webgl_context_lost(emscripten_webgl_get_current_context())) return;
    if (program_) glDeleteProgram(program_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
    glDeleteTextures(3, planes_);
}

void YuvRenderer::invalidate() noexcept {
    program_ = vao_ = framebuffer_ = 0;
    attached_texture_ = 0;
    color_key_ = rotation_ = -1;
    for (int plane = 0; plane < 3; ++plane) {
        planes_[plane] = 0;
        plane_widths_[plane] = plane_heights_[plane] = 0;
    }
}

bool YuvRenderer::initialize() {
    if (program_) return true;
    const GLuint vertex = compile(GL_VERTEX_SHADER, R"(#version 300 es
        out vec2 uv;
        void main() {
            vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
            uv = p;
            gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
        })");
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, R"(#version 300 es
        precision highp float;
        in vec2 uv;
        uniform sampler2D y_plane;
        uniform sampler2D u_plane;
        uniform sampler2D v_plane;
        uniform mat3 matrix;
        uniform vec3 offset;
        uniform int rotation;
        out vec4 color;
        void main() {
            vec2 p = uv;
            if (rotation == 1) p = vec2(uv.y, 1.0 - uv.x);
            else if (rotation == 2) p = 1.0 - uv;
            else if (rotation == 3) p = vec2(1.0 - uv.y, uv.x);
            vec3 yuv = vec3(texture(y_plane, p).r, texture(u_plane, p).r, texture(v_plane, p).r);
            color = vec4(matrix * (yuv + offset), 1.0);
        })");
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(program);
        return false;
    }
    program_ = program;
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "y_plane"), 0);
    glUniform1i(glGetUniformLocation(program_, "u_plane"), 1);
    glUniform1i(glGetUniformLocation(program_, "v_plane"), 2);
    matrix_location_ = glGetUniformLocation(program_, "matrix");
    offset_location_ = glGetUniformLocation(program_, "offset");
    rotation_location_ = glGetUniformLocation(program_, "rotation");
    glGenVertexArrays(1, &vao_);
    glGenFramebuffers(1, &framebuffer_);
    glGenTextures(3, planes_);
    if (vao_ && framebuffer_ && planes_[0] && planes_[1] && planes_[2]) {
        for (int plane = 0; plane < 3; ++plane) {
            glActiveTexture(GL_TEXTURE0 + plane);
            glBindTexture(GL_TEXTURE_2D, planes_[plane]);
            parameters(plane == 0 ? GL_LINEAR : GL_NEAREST);
        }
        return true;
    }
    glDeleteProgram(program_);
    glDeleteVertexArrays(1, &vao_);
    glDeleteFramebuffers(1, &framebuffer_);
    glDeleteTextures(3, planes_);
    invalidate();
    return false;
}

bool YuvRenderer::update(const AVFrame* frame, const DisplayGeometry& geometry, GLuint& texture, bool allocate_target) {
    if (frame->width % 2 != 0 || frame->height % 2 != 0) return false;
    const bool swapped = geometry.rotation == 90 || geometry.rotation == 270;
    if (geometry.width != (swapped ? frame->height : frame->width) ||
        geometry.height != (swapped ? frame->width : frame->height))
        return false;
    GLboolean feedback = GL_FALSE;
    glGetBooleanv(GL_TRANSFORM_FEEDBACK_ACTIVE, &feedback);
    if (feedback) return false; // A host's active transform feedback must not capture this draw.
    for (int plane = 0; plane < 3; ++plane) {
        const int width = plane == 0 ? frame->width : (frame->width + 1) / 2;
        if (!frame->data[plane] || frame->linesize[plane] < width) return false;
    }
    clear_gl_errors();
    SavedState state;
    if (!initialize()) return false;
    glActiveTexture(GL_TEXTURE0);
    if (texture == 0) {
        glGenTextures(1, &texture);
        allocate_target = true;
        glBindTexture(GL_TEXTURE_2D, texture);
        parameters(GL_LINEAR);
    } else
        glBindTexture(GL_TEXTURE_2D, texture);
    if (allocate_target) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, geometry.width, geometry.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer_);
    if (attached_texture_ != texture || allocate_target) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
        attached_texture_ = texture;
    }
    for (int plane = 0; plane < 3; ++plane) {
        const int width = plane == 0 ? frame->width : (frame->width + 1) / 2;
        const int height = plane == 0 ? frame->height : (frame->height + 1) / 2;
        glActiveTexture(GL_TEXTURE0 + plane);
        glBindTexture(GL_TEXTURE_2D, planes_[plane]);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[plane]);
        if (plane_widths_[plane] != width || plane_heights_[plane] != height) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, frame->data[plane]);
            plane_widths_[plane] = width;
            plane_heights_[plane] = height;
        } else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, frame->data[plane]);
    }
    const bool full_range = frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P;
    const bool bt709 =
        frame->colorspace == AVCOL_SPC_BT709 || (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height > 576);
    const float kr = bt709 ? 0.2126F : 0.299F;
    const float kb = bt709 ? 0.0722F : 0.114F;
    const float kg = 1.0F - kr - kb;
    const float y_scale = full_range ? 1.0F : 255.0F / 219.0F;
    const float c_scale = full_range ? 1.0F : 255.0F / 224.0F;
    const float matrix[] = {y_scale,
                            y_scale,
                            y_scale,
                            0.0F,
                            -2.0F * kb * (1.0F - kb) / kg * c_scale,
                            2.0F * (1.0F - kb) * c_scale,
                            2.0F * (1.0F - kr) * c_scale,
                            -2.0F * kr * (1.0F - kr) / kg * c_scale,
                            0.0F};
    glUseProgram(program_);
    const int color_key = (full_range ? 1 : 0) | (bt709 ? 2 : 0);
    if (color_key != color_key_) {
        glUniformMatrix3fv(matrix_location_, 1, GL_FALSE, matrix);
        glUniform3f(offset_location_, full_range ? 0.0F : -16.0F / 255.0F, -128.0F / 255.0F, -128.0F / 255.0F);
        color_key_ = color_key;
    }
    if (geometry.rotation != rotation_) {
        glUniform1i(rotation_location_, geometry.rotation / 90);
        rotation_ = geometry.rotation;
    }
    glBindVertexArray(vao_);
    glViewport(0, 0, geometry.width, geometry.height);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return glGetError() == GL_NO_ERROR;
}

} // namespace imvideo
