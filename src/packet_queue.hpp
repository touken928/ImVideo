#pragma once

#include "ffmpeg_raii.hpp"

#include <array>
#include <cstddef>

namespace imvideo {

// A decoder-thread queue. Packet headers are allocated lazily, reused across
// read-ahead/seek cycles, and released with the current media session.
class PacketQueue {
public:
    static constexpr std::size_t capacity = 64;

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool full() const noexcept { return size_ == capacity; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    bool push(AVPacket* packet) {
        if (full()) return false;
        auto& slot = packets_[(head_ + size_) % capacity];
        if (!slot) slot = make_av_packet();
        if (!slot) return false;
        av_packet_move_ref(slot.get(), packet);
        ++size_;
        return true;
    }

    bool pop(AVPacket* packet) {
        if (empty()) return false;
        av_packet_unref(packet);
        av_packet_move_ref(packet, packets_[head_].get());
        head_ = (head_ + 1) % capacity;
        --size_;
        return true;
    }

    void clear() noexcept {
        while (!empty()) {
            av_packet_unref(packets_[head_].get());
            head_ = (head_ + 1) % capacity;
            --size_;
        }
        head_ = 0;
    }

    void release() noexcept {
        for (auto& packet : packets_) packet.reset();
        head_ = size_ = 0;
    }

private:
    std::array<AvPacketPtr, capacity> packets_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

} // namespace imvideo
