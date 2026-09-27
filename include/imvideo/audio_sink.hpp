#pragma once

#include <cstddef>

namespace imvideo {

// Optional audio destination. Player retains shared ownership until close()
// finishes. open() and close() run on the control thread. write() and flush()
// run on the decode thread. pause() and set_volume() may run concurrently with
// those calls and must be thread-safe. Sink callbacks must not call into Player.
//
// When audio is open, clock_seconds() is the presentation master. It must
// advance as audio is consumed and return to zero after flush(). A clock that
// stays at zero holds video. clock_seconds() must be non-blocking. write()
// should bound its own queue; Player also avoids writing more than about half
// a second ahead of the clock.
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
