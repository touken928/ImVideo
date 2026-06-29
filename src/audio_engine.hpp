// -----------------------------------------------------------------------
// audio_engine.hpp  —  Per-player miniaudio device wrapper
//
// Each Player with an audio stream creates one AudioEngine that:
//   - owns a miniaudio playback device
//   - provides a C-linkage data callback that reads from a SpscRingBuffer
//   - tracks the number of frames consumed (audio clock master)
// -----------------------------------------------------------------------
#pragma once

#include "ring_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

// Only include miniaudio implementation in audio_engine.cpp.
// Here we use a forward declaration for the opaque device type.
struct ma_device;   // defined in miniaudio.h

namespace imvideo {

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&) = delete;
    AudioEngine& operator=(AudioEngine&&) = delete;

    /// Initialize the playback device.
    /// @param ring          Ring buffer the callback reads from.
    /// @param sample_rate   Target output sample rate (e.g. 48000).
    /// @param channels      Target output channels (e.g. 2 for stereo).
    /// @param volume        Initial volume (0..1).
    /// @returns true on success.
    bool init(SpscRingBuffer<float>* ring,
              unsigned int sample_rate,
              unsigned int channels,
              float volume);

    void shutdown();

    /// Start the device (playback begins shortly after).
    bool start();

    /// Stop the device.
    void stop();

    bool is_valid() const noexcept { return valid_; }
    bool is_started() const noexcept { return started_.load(std::memory_order_acquire); }

    /// Frames consumed since last reset (monotonically increasing).
    /// Thread-safe — written by audio callback, read by render thread.
    uint64_t frames_consumed() const noexcept { return frames_consumed_.load(std::memory_order_acquire); }

    /// Reset the consumed counter (called after seek).
    void reset_frames_consumed() noexcept { frames_consumed_.store(0, std::memory_order_release); }

    void set_volume(float v);

    unsigned int sample_rate() const noexcept { return sr_; }
    unsigned int channels()   const noexcept { return ch_; }

    const char* last_error() const noexcept { return err_.c_str(); }

private:
    // The miniaudio data callback — reads from the ring buffer.
    // Avoids miniaudio types in the header so we don't need to
    // include miniaudio.h here.
    static void data_callback_static(ma_device* pDevice, void* pOutput,
                                     const void* pInput, unsigned int frameCount);

    std::unique_ptr<ma_device> device_;
    SpscRingBuffer<float>*     ring_     = nullptr;
    std::atomic<uint64_t>      frames_consumed_{0};
    unsigned int               sr_       = 0;
    unsigned int               ch_       = 0;
    float                      volume_   = 1.0f;
    bool                       valid_    = false;
    std::atomic<bool>          started_{false};
    std::string                err_;
};

} // namespace imvideo
