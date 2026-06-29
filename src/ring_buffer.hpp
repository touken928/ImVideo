// -----------------------------------------------------------------------
// ring_buffer.hpp  —  Lock-free SPSC ring buffer for PCM audio samples
//
// Single-producer (decode worker), single-consumer (miniaudio callback).
// Capacity must be a power of two.
// -----------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace imvideo {

template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(size_t capacity_pow2)
        : buffer_(capacity_pow2, T{})
        , capacity_(capacity_pow2)
        , mask_(capacity_pow2 - 1)
    {
    }

    // ---- producers (decode thread) --------------------------------------

    size_t write(const T* data, size_t count) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t used = tail - head;
        const size_t avail = capacity_ - used;
        const size_t to_write = std::min(count, avail);

        if (to_write == 0) return 0;

        const size_t offset  = tail & mask_;
        const size_t first   = std::min(to_write, capacity_ - offset);
        std::memcpy(&buffer_[offset], data, first * sizeof(T));
        if (first < to_write) {
            std::memcpy(&buffer_[0], data + first, (to_write - first) * sizeof(T));
        }

        tail_.store(tail + to_write, std::memory_order_release);
        return to_write;
    }

    // ---- consumers (audio callback) -------------------------------------

    size_t read(T* data, size_t count) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t used = tail - head;
        const size_t to_read = std::min(count, used);

        if (to_read == 0) return 0;

        const size_t offset = head & mask_;
        const size_t first  = std::min(to_read, capacity_ - offset);
        std::memcpy(data, &buffer_[offset], first * sizeof(T));
        if (first < to_read) {
            std::memcpy(data + first, &buffer_[0], (to_read - first) * sizeof(T));
        }

        head_.store(head + to_read, std::memory_order_release);
        return to_read;
    }

    // ---- shared ---------------------------------------------------------

    size_t available() const noexcept {
        return tail_.load(std::memory_order_acquire)
             - head_.load(std::memory_order_acquire);
    }

    size_t free_space() const noexcept {
        return capacity_ - available();
    }

    void clear() noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        head_.store(tail, std::memory_order_release);
    }

private:
    std::vector<T>      buffer_;
    size_t              capacity_;
    size_t              mask_;
    std::atomic<size_t> head_{0};   // read  cursor (consumer)
    std::atomic<size_t> tail_{0};   // write cursor (producer)
};

} // namespace imvideo
