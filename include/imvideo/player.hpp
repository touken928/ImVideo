#pragma once

#include <imvideo/frame.hpp>
#include <imvideo/source.hpp>
#include <imvideo/audio_sink.hpp>

#include <memory>
#include <string_view>

namespace imvideo {

enum class State { Idle, Opening, Playing, Paused, Ended, Error };

struct Options {
    bool autoplay = true;
    bool loop = false;
    // nullptr disables audio decoding, resampling, buffering, and output.
    std::shared_ptr<AudioSink> audio_sink;
};

class Player {
public:
    Player();
    ~Player();

    Player(Player&&) noexcept;
    Player& operator=(Player&&) noexcept;
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool open(const Source& source, const Options& options = {});
    void close();
    void play();
    void pause();
    void stop();
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
    [[nodiscard]] std::string_view error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace imvideo
