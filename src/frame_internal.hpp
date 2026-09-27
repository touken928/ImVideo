#pragma once

#include <imvideo/frame.hpp>

extern "C" {
#include <libavutil/frame.h>
}

struct AVStream;
struct SwsContext;
namespace imvideo {

// Converts a decoded frame to RGBA in display orientation. The caller owns the result.
class DisplayConverter {
public:
    DisplayConverter() = default;
    ~DisplayConverter();

    DisplayConverter(const DisplayConverter&) = delete;
    DisplayConverter& operator=(const DisplayConverter&) = delete;

    void reset();
    [[nodiscard]] AVFrame* convert(const AVFrame* source, const AVStream* stream);

private:
    SwsContext* sws_ = nullptr;
};

struct Frame::Impl {
    explicit Impl(AVFrame* value, std::int64_t timestamp) : av_frame(value), pts_value(timestamp) {}
    ~Impl() { av_frame_free(&av_frame); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    AVFrame* av_frame = nullptr;
    std::int64_t pts_value = 0;
};

} // namespace imvideo
