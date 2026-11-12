#pragma once

#include <imvideo/frame.hpp>

#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

struct AVStream;
struct SwsContext;
struct AVBufferPool;
namespace imvideo {

constexpr int maximum_display_dimension = 16384;

#if defined(__EMSCRIPTEN__)
constexpr std::int64_t maximum_web_pixels = 1920 * 1080;
#endif

struct DisplayGeometry {
    int width = 0;
    int height = 0;
    int rotation = 0;
};

// Prepares a display frame. The caller owns the result; deferred YUV frames
// retain their decoded storage and carry display geometry separately.
class DisplayConverter {
public:
    DisplayConverter() = default;
    ~DisplayConverter();

    DisplayConverter(const DisplayConverter&) = delete;
    DisplayConverter& operator=(const DisplayConverter&) = delete;

    void reset();
    [[nodiscard]] AVFrame* convert(const AVFrame* source, const AVStream* stream, bool prefer_gpu = false,
                                   DisplayGeometry* geometry = nullptr);
    [[nodiscard]] AVFrame* rgba(const AVFrame* source, const DisplayGeometry& geometry);

private:
    SwsContext* sws_ = nullptr;
    AVBufferPool* rgba_pool_ = nullptr;
    int pool_size_ = 0;
    int source_width_ = 0;
    int source_height_ = 0;
    int scaled_width_ = 0;
    int scaled_height_ = 0;
    int source_format_ = -1;
    int color_space_ = -1;
    int color_range_ = -1;
    std::vector<std::uint8_t> rotated_pixels_;
};

struct Frame::Impl {
    explicit Impl(AVFrame* value, std::int64_t timestamp, DisplayGeometry display = {})
        : av_frame(value), pts_value(timestamp), geometry(display) {
        if (geometry.width == 0 && av_frame) {
            geometry.width = av_frame->width;
            geometry.height = av_frame->height;
        }
    }
    ~Impl() { av_frame_free(&av_frame); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    AVFrame* av_frame = nullptr;
    std::int64_t pts_value = 0;
    DisplayGeometry geometry;
};

} // namespace imvideo
