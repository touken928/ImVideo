// -----------------------------------------------------------------------
// video_frame.hpp  —  Thread-safe bounded video frame queue
//
// Producer: decode worker thread (pushes RGBA frames).
// Consumer: render thread (pops in Update()).
//
// v2.2:
//  - Internal buffer recycling via acquire_buffer() / recycle()
//  - Zero-copy ScopedFront for PTS inspection
// -----------------------------------------------------------------------
#pragma once

#include <deque>
#include <mutex>
#include <vector>

namespace imvideo {

struct VideoFrame {
    double           pts    = -1.0;
    int              width  = 0;
    int              height = 0;
    std::vector<uint8_t> rgba;   // RGBA pixel data (width*height*4 bytes)
};

class VideoFrameQueue {
public:
    static constexpr size_t kMaxFrames = 24;

    // --- producer (decode worker) ----------------------------------------

    /// Borrow a buffer from the pool (avoids per-frame allocation).
    /// The returned vector has size == needed (resized if recycled buffer
    /// is large enough, otherwise freshly allocated).
    std::vector<uint8_t> acquire_buffer(size_t needed) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recycled_.capacity() >= needed) {
            recycled_.resize(needed);
            return std::move(recycled_);   // recycled_ becomes empty
        }
        return std::vector<uint8_t>(needed);
    }

    bool push(VideoFrame f) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kMaxFrames) return false;
        queue_.push_back(std::move(f));
        return true;
    }

    // --- consumer (render thread) ----------------------------------------

    /// Pop the front frame (moves out the pixel buffer).
    bool pop(VideoFrame& f) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        f = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    /// Return a buffer to the pool so the worker can reuse it.
    /// Call after upload_frame().
    void recycle(VideoFrame& f) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Keep whichever has larger capacity
        if (f.rgba.capacity() > recycled_.capacity()) {
            recycled_ = std::move(f.rgba);
        }
        f.rgba.clear();
    }

    /// Return a scoped guard + pointer to the front frame.
    /// Zero-copy inspection of PTS etc.
    struct ScopedFront {
        std::unique_lock<std::mutex> lock;
        const VideoFrame*            ptr = nullptr;
    };
    ScopedFront front() {
        auto ul = std::unique_lock<std::mutex>(mutex_);
        return ScopedFront{std::move(ul),
                           queue_.empty() ? nullptr : &queue_.front()};
    }

    /// Return front PTS without touching pixel data (-1 if empty).
    double front_pts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty() ? -1.0 : queue_.front().pts;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        recycled_.clear();
        std::vector<uint8_t>().swap(recycled_);  // free memory
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex  mutex_;
    std::deque<VideoFrame> queue_;
    std::vector<uint8_t>    recycled_;   // pool buffer
};

} // namespace imvideo
