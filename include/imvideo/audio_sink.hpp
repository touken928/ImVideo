#pragma once

#include <cstddef>

namespace imvideo {

// Optional audio destination. Calls are made from Player's decode thread;
// Player retains shared ownership until close() finishes.
class AudioSink {
public:
    virtual ~AudioSink() = default;

    virtual bool open(int sample_rate, int channels) = 0;
    virtual void close() = 0;
    virtual void write(const float* samples, std::size_t frames) = 0;
    virtual void pause(bool paused) = 0;
    virtual void flush() = 0;
    virtual void set_volume(float volume) = 0;
    [[nodiscard]] virtual double clock_seconds() const noexcept = 0;
};

} // namespace imvideo
