#pragma once

#include <imvideo/frame.hpp>

extern "C" {
#include <libavutil/frame.h>
}

namespace imvideo {

struct Frame::Impl {
    explicit Impl(AVFrame* value, std::int64_t timestamp) : av_frame(value), pts_value(timestamp) {}
    ~Impl() { av_frame_free(&av_frame); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    AVFrame* av_frame = nullptr;
    std::int64_t pts_value = 0;
};

} // namespace imvideo
