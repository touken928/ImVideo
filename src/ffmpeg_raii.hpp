#pragma once

#include <memory>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
}

namespace imvideo {

template <typename T, void (*Release)(T**)> struct AvDeleter {
    void operator()(T* value) const noexcept { Release(&value); }
};

using AvFramePtr = std::unique_ptr<AVFrame, AvDeleter<AVFrame, av_frame_free>>;
using AvPacketPtr = std::unique_ptr<AVPacket, AvDeleter<AVPacket, av_packet_free>>;

inline AvFramePtr make_av_frame() { return AvFramePtr{av_frame_alloc()}; }
inline AvPacketPtr make_av_packet() { return AvPacketPtr{av_packet_alloc()}; }

} // namespace imvideo
