#include "frame_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/display.h>
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

int rotation_from_matrix(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 9 * sizeof(std::int32_t)) return 0;
    const auto angle = av_display_rotation_get(reinterpret_cast<const std::int32_t*>(data));
    // The matrix angle is counterclockwise. Metadata and display use clockwise degrees.
    return normalize_rotation(-angle);
}

int display_rotation_degrees(const AVFrame* frame, const AVStream* stream) {
    if (const AVFrameSideData* side = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX)) {
        const int rotation = rotation_from_matrix(side->data, side->size);
        if (rotation != 0) return rotation;
    }
    if (stream && stream->codecpar) {
        for (int index = 0; index < stream->codecpar->nb_coded_side_data; ++index) {
            const AVPacketSideData& data = stream->codecpar->coded_side_data[index];
            if (data.type != AV_PKT_DATA_DISPLAYMATRIX) continue;
            const int rotation = rotation_from_matrix(data.data, data.size);
            if (rotation != 0) return rotation;
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

void configure_scaler_colors(SwsContext* scaler, const AVFrame* frame) {
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    const bool rgb = descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    int source_range = rgb ? 1 : 0;
    if (frame->color_range == AVCOL_RANGE_JPEG) source_range = 1;
    if (frame->color_range == AVCOL_RANGE_MPEG) source_range = 0;
    int space = frame->colorspace;
    if (space == AVCOL_SPC_UNSPECIFIED || space == AVCOL_SPC_RGB)
        space = frame->height > 576 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    const int* source = sws_getCoefficients(space);
    const int* destination = sws_getCoefficients(SWS_CS_DEFAULT);
    sws_setColorspaceDetails(scaler, source, source_range, destination, 1, 0, 1 << 16, 1 << 16);
}

DisplayConverter::~DisplayConverter() { reset(); }

void DisplayConverter::reset() {
    sws_freeContext(sws_);
    sws_ = nullptr;
}

AVFrame* DisplayConverter::convert(const AVFrame* source, const AVStream* stream) {
    if (!source || source->width <= 0 || source->height <= 0) return nullptr;
    int scaled_width = source->width;
    int scaled_height = source->height;
    AVRational sar = source->sample_aspect_ratio;
    if ((sar.num <= 0 || sar.den <= 0) && stream) sar = stream->sample_aspect_ratio;
    if (sar.num > 0 && sar.den > 0 && sar.num != sar.den)
        scaled_width = std::max(1, static_cast<int>(av_rescale(source->width, sar.num, sar.den)));
    const int rotation = display_rotation_degrees(source, stream);
    const bool swap_axes = rotation == 90 || rotation == 270;
    const int output_width = swap_axes ? scaled_height : scaled_width;
    const int output_height = swap_axes ? scaled_width : scaled_height;
    if (output_width > 16384 || output_height > 16384) return nullptr;
    if (scaled_width > (std::numeric_limits<int>::max() / 4) / std::max(scaled_height, 1)) return nullptr;

    sws_ = sws_getCachedContext(sws_, source->width, source->height, static_cast<AVPixelFormat>(source->format),
                                scaled_width, scaled_height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return nullptr;
    configure_scaler_colors(sws_, source);

    AVFrame* output = av_frame_alloc();
    if (!output) return nullptr;
    output->format = AV_PIX_FMT_RGBA;
    output->width = output_width;
    output->height = output_height;
    if (av_frame_get_buffer(output, 32) < 0) {
        av_frame_free(&output);
        return nullptr;
    }
    if (rotation == 0) {
        std::uint8_t* destination[] = {output->data[0]};
        int strides[] = {output->linesize[0]};
        if (sws_scale(sws_, source->data, source->linesize, 0, source->height, destination, strides) <= 0) {
            av_frame_free(&output);
            return nullptr;
        }
        return output;
    }

    std::vector<std::uint8_t> scaled(static_cast<std::size_t>(scaled_width) * scaled_height * 4);
    std::uint8_t* destination[] = {scaled.data()};
    int strides[] = {scaled_width * 4};
    if (sws_scale(sws_, source->data, source->linesize, 0, source->height, destination, strides) <= 0) {
        av_frame_free(&output);
        return nullptr;
    }
    rotate_rgba(scaled.data(), scaled_width, scaled_height, scaled_width * 4, output->data[0], output->linesize[0],
                rotation);
    return output;
}

} // namespace imvideo
