#include "ffmpeg_raii.hpp"
#include "frame_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace imvideo {
namespace {

int normalize_rotation(double angle) {
    if (!std::isfinite(angle)) return 0;
    int rounded = static_cast<int>(std::lround(angle)) % 360;
    if (rounded < 0) rounded += 360;
    if (rounded == 90 || rounded == 180 || rounded == 270) return rounded;
    return 0;
}

void configure_scaler_colors(SwsContext* scaler, const AVFrame* frame) {
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    const bool rgb = descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    int source_range = rgb || format == AV_PIX_FMT_YUVJ420P ? 1 : 0;
    if (frame->color_range == AVCOL_RANGE_JPEG) source_range = 1;
    if (frame->color_range == AVCOL_RANGE_MPEG) source_range = 0;
    int space = frame->colorspace;
    if (space == AVCOL_SPC_UNSPECIFIED || space == AVCOL_SPC_RGB)
        space = frame->height > 576 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    const int* source = sws_getCoefficients(space);
    const int* destination = sws_getCoefficients(SWS_CS_DEFAULT);
    sws_setColorspaceDetails(scaler, source, source_range, destination, 1, 0, 1 << 16, 1 << 16);
}

bool rotation_from_matrix(const std::uint8_t* data, std::size_t size, int& rotation) {
    if (!data || size < 9 * sizeof(std::int32_t)) return false;
    std::int32_t matrix[9];
    std::memcpy(matrix, data, sizeof(matrix));
    const auto angle = av_display_rotation_get(matrix);
    // The matrix angle is counterclockwise. Metadata and display use clockwise degrees.
    rotation = normalize_rotation(-angle);
    return true;
}

int display_rotation_degrees(const AVFrame* frame, const AVStream* stream) {
    if (const AVFrameSideData* side = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX)) {
        int rotation = 0;
        if (rotation_from_matrix(side->data, side->size, rotation)) return rotation;
    }
    if (stream && stream->codecpar) {
        for (int index = 0; index < stream->codecpar->nb_coded_side_data; ++index) {
            const AVPacketSideData& data = stream->codecpar->coded_side_data[index];
            if (data.type != AV_PKT_DATA_DISPLAYMATRIX) continue;
            int rotation = 0;
            if (rotation_from_matrix(data.data, data.size, rotation) && rotation != 0) return rotation;
        }
    }
    if (stream) {
        if (const AVDictionaryEntry* rotate = av_dict_get(stream->metadata, "rotate", nullptr, 0))
            return normalize_rotation(std::atoi(rotate->value));
    }
    return 0;
}

void rotate_rgba(const std::uint8_t* source, int source_width, int source_height, int source_stride,
                 std::uint8_t* destination, int destination_stride, int clockwise_degrees) {
    if (clockwise_degrees == 180) {
        for (int y = 0; y < source_height; ++y) {
            const auto* row = source + static_cast<std::size_t>(y) * source_stride;
            auto* output = destination + static_cast<std::size_t>(source_height - 1 - y) * destination_stride;
            for (int x = 0; x < source_width; ++x)
                std::memcpy(output + static_cast<std::size_t>(source_width - 1 - x) * 4,
                            row + static_cast<std::size_t>(x) * 4, 4);
        }
        return;
    }
    const bool right = clockwise_degrees == 90;
    for (int y = 0; y < source_height; ++y) {
        for (int x = 0; x < source_width; ++x) {
            const int output_x = right ? source_height - 1 - y : y;
            const int output_y = right ? x : source_width - 1 - x;
            std::memcpy(destination + static_cast<std::size_t>(output_y) * destination_stride +
                            static_cast<std::size_t>(output_x) * 4,
                        source + static_cast<std::size_t>(y) * source_stride + static_cast<std::size_t>(x) * 4, 4);
        }
    }
}

} // namespace

DisplayConverter::~DisplayConverter() { reset(); }

void DisplayConverter::reset() {
    sws_freeContext(sws_);
    sws_ = nullptr;
    av_buffer_pool_uninit(&rgba_pool_);
    pool_size_ = 0;
    source_width_ = source_height_ = scaled_width_ = scaled_height_ = 0;
    source_format_ = color_space_ = color_range_ = -1;
    rotated_pixels_.clear();
}

AVFrame* DisplayConverter::convert(const AVFrame* source, const AVStream* stream, bool prefer_gpu,
                                   DisplayGeometry* geometry) {
    if (!source || source->width <= 0 || source->height <= 0) return nullptr;
    int scaled_width = source->width;
    int scaled_height = source->height;
    AVRational sar = source->sample_aspect_ratio;
    if ((sar.num <= 0 || sar.den <= 0) && stream) sar = stream->sample_aspect_ratio;
    if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) {
        const auto width = std::max<std::int64_t>(1, av_rescale(source->width, sar.num, sar.den));
        if (width > maximum_display_dimension) return nullptr;
        scaled_width = static_cast<int>(width);
    }
    const int rotation = display_rotation_degrees(source, stream);
    const bool swap_axes = rotation == 90 || rotation == 270;
    const int output_width = swap_axes ? scaled_height : scaled_width;
    const int output_height = swap_axes ? scaled_width : scaled_height;
    if (output_width > maximum_display_dimension || output_height > maximum_display_dimension) return nullptr;
#if defined(__EMSCRIPTEN__)
    if (static_cast<std::int64_t>(output_width) * output_height > maximum_web_pixels) return nullptr;
#endif
    const DisplayGeometry display{output_width, output_height, rotation};
#if defined(__EMSCRIPTEN__)
    const auto format = static_cast<AVPixelFormat>(source->format);
    const bool supported_color = source->colorspace == AVCOL_SPC_UNSPECIFIED || source->colorspace == AVCOL_SPC_BT709 ||
                                 source->colorspace == AVCOL_SPC_BT470BG || source->colorspace == AVCOL_SPC_SMPTE170M;
    if (prefer_gpu && supported_color && scaled_width == source->width && source->width % 2 == 0 &&
        source->height % 2 == 0 && (format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P)) {
        bool valid = true;
        for (int plane = 0; plane < 3; ++plane) {
            const int width = plane == 0 ? source->width : (source->width + 1) / 2;
            valid = valid && source->data[plane] && source->linesize[plane] >= width;
        }
        if (valid) {
            AVFrame* retained = av_frame_clone(source);
            if (retained && geometry) *geometry = display;
            return retained;
        }
    }
#else
    (void)prefer_gpu;
#endif
    AVFrame* output = rgba(source, display);
    if (output && geometry) *geometry = {output_width, output_height, 0};
    return output;
}

AVFrame* DisplayConverter::rgba(const AVFrame* source, const DisplayGeometry& geometry) {
    if (!source || geometry.width <= 0 || geometry.height <= 0) return nullptr;
    const int rotation = geometry.rotation;
    const bool swap_axes = rotation == 90 || rotation == 270;
    const int scaled_width = swap_axes ? geometry.height : geometry.width;
    const int scaled_height = swap_axes ? geometry.width : geometry.height;
    const bool changed = !sws_ || source_width_ != source->width || source_height_ != source->height ||
                         scaled_width_ != scaled_width || scaled_height_ != scaled_height ||
                         source_format_ != source->format;

    // FFmpeg can normalize formats/flags internally (YUVJ and odd dimensions).
    // Querying its cache again can recreate the context and lose configured
    // colors even when our input is unchanged. Track the requested shape here.
    if (changed)
        sws_ =
            sws_getCachedContext(sws_, source->width, source->height, static_cast<AVPixelFormat>(source->format),
                                 scaled_width, scaled_height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return nullptr;
    if (changed || color_space_ != source->colorspace || color_range_ != source->color_range) {
        configure_scaler_colors(sws_, source);
        source_width_ = source->width;
        source_height_ = source->height;
        scaled_width_ = scaled_width;
        scaled_height_ = scaled_height;
        source_format_ = source->format;
        color_space_ = source->colorspace;
        color_range_ = source->color_range;
    }

    auto output = make_av_frame();
    if (!output) return nullptr;
    output->format = AV_PIX_FMT_RGBA;
    output->width = geometry.width;
    output->height = geometry.height;
    const int bytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, output->width, output->height, 32);
    if (bytes <= 0) { return nullptr; }
    if (!rgba_pool_ || pool_size_ != bytes) {
        av_buffer_pool_uninit(&rgba_pool_);
        rgba_pool_ = av_buffer_pool_init(static_cast<std::size_t>(bytes), nullptr);
        pool_size_ = bytes;
    }
    output->buf[0] = rgba_pool_ ? av_buffer_pool_get(rgba_pool_) : nullptr;
    if (!output->buf[0] || av_image_fill_arrays(output->data, output->linesize, output->buf[0]->data, AV_PIX_FMT_RGBA,
                                                output->width, output->height, 32) < 0) {
        return nullptr;
    }
    if (rotation == 0) {
        std::uint8_t* destination[] = {output->data[0]};
        int strides[] = {output->linesize[0]};
        if (sws_scale(sws_, source->data, source->linesize, 0, source->height, destination, strides) <= 0) {
            return nullptr;
        }
        return output.release();
    }

    rotated_pixels_.resize(static_cast<std::size_t>(scaled_width) * scaled_height * 4);
    std::uint8_t* destination[] = {rotated_pixels_.data()};
    int strides[] = {scaled_width * 4};
    if (sws_scale(sws_, source->data, source->linesize, 0, source->height, destination, strides) <= 0) {
        return nullptr;
    }
    rotate_rgba(rotated_pixels_.data(), scaled_width, scaled_height, scaled_width * 4, output->data[0],
                output->linesize[0], rotation);
    return output.release();
}

} // namespace imvideo
