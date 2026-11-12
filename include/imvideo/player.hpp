#pragma once

#include <imvideo/audio_sink.hpp>
#include <imvideo/frame.hpp>
#include <imvideo/source.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace imvideo {

enum class State { Idle, Opening, Playing, Paused, Ended, Error };

struct Options {
    bool autoplay = true;
    bool loop = false;
    // nullptr disables audio decoding, resampling, buffering, and output.
    std::shared_ptr<AudioSink> audio_sink;
    // With a sink, report audio decoder/device failures instead of falling back
    // to silent playback. Inputs without an audio stream are still accepted.
    bool fail_on_audio_error = false;
    // Opt in to deferred YUV display conversion by the Emscripten Renderer.
    // Other platforms and unsupported formats retain the CPU RGBA path.
    bool gpu_color_conversion = false;
    // Collect CPU stage timings for diagnostics; disabled by default.
    bool collect_statistics = false;
};

struct PlaybackStatistics {
    std::uint64_t decoded_frames = 0;
    std::uint64_t cpu_converted_frames = 0;
    std::uint64_t gpu_prepared_frames = 0;
    double decode_ms = 0.0;
    double display_ms = 0.0;
};

class Player {
public:
    Player();
    ~Player();

    Player(Player&&) noexcept;
    Player& operator=(Player&&) noexcept;
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Emscripten: own and control the Player on a worker thread. File sources
    // refer to the virtual filesystem; direct URL/RTSP inputs are unsupported.
    bool open(const Source& source, const Options& options = {});
    void close();
    void play();
    void pause();
    void stop();
    // Blocks until the demuxer accepts or rejects the request. Seconds are relative
    // to the start of the input. Does not abort an in-flight read; a network seek
    // waits for the current packet. Control methods must be called from one thread.
    bool seek(double seconds);
    bool set_speed(double speed);

    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] double position() const noexcept;
    [[nodiscard]] double duration() const noexcept;
    [[nodiscard]] bool seekable() const noexcept;
    [[nodiscard]] bool live() const noexcept;
    [[nodiscard]] bool can_set_speed() const noexcept;
    [[nodiscard]] double speed() const noexcept;
    [[nodiscard]] Frame frame() const;
    void set_volume(float volume);
    [[nodiscard]] float volume() const noexcept;
    [[nodiscard]] bool audio_enabled() const noexcept;
    // A copy of the last error. Safe to keep after later open() or close() calls.
    [[nodiscard]] std::string error() const;
    // Approximate counters for the current open session; CPU timings exclude
    // presentation waits. Returns zero counters unless collection was enabled.
    [[nodiscard]] PlaybackStatistics statistics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace imvideo
