#pragma once

#include <imgui.h>
#include <filesystem>
#include <memory>
#include <string_view>

namespace imvideo {

// -----------------------------------------------------------------------
// Player  — multi-threaded video/audio playback with OpenGL texture output.
//
//   Thread safety:
//     - Public methods (Open, OpenUrl, Close, Play, Pause, Stop, Seek,
//       SetVolume, SetSpeed, Position, Duration, IsPlaying, Texture,
//       VideoSize, LastError) are called from the same thread that owns
//       the OpenGL context (typically the render / UI thread).
//     - Update() must be called once per frame on that thread — it
//       consumes decoded frames from an internal queue and uploads to
//       the OpenGL texture.
//     - A dedicated decode worker thread reads packets, decodes video
//       (→ RGBA via swscale) and audio (→ float PCM via swresample),
//       and pushes results into bounded thread-safe queues.
//     - miniaudio callbacks read from the PCM ring buffer on a separate
//       audio thread.
//
//   Update() and the destructor assume a current OpenGL context exists;
//   they create / update / destroy the internal texture.
//
//   Playback position is driven by a monotonic wall clock. Audio is decoded
//   into a private ring buffer and consumed by miniaudio independently.
//
//   Speed control is locked to 1.0 when audio is active (no time-stretch
//   in v2). The UI is expected to disable the speed slider accordingly.
// -----------------------------------------------------------------------
class Player {
public:
    Player();
    ~Player();

    Player(Player&&) noexcept;
    Player& operator=(Player&&) noexcept;

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // ---- lifecycle -------------------------------------------------------
    bool Open(const std::filesystem::path& path);
    bool Open(std::string_view source);   // auto-detects local path vs URL
    bool OpenUrl(std::string_view url);   // explicit URL open, rejects unsupported schemes
    void Close();

    // ---- playback control ------------------------------------------------
    void Play();
    void Pause();
    void Stop();
    void Seek(double seconds);

    // ---- query -----------------------------------------------------------
    double      Position()  const;   // current playback position (seconds)
    double      Duration()  const;   // total duration (seconds), 0 if none
    bool        IsPlaying() const;
    ImTextureID Texture()   const;   // Opaque GLuint, may be 0
    ImVec2      VideoSize() const;   // native video dimensions
    const char* LastError() const;   // human-readable last error, "" if none

    // ---- runtime ---------------------------------------------------------
    void SetVolume(float volume);    // 0..1
    void SetSpeed(float speed);      // locked at 1.0 when audio is active

    // Must be called once per frame on the render thread while a GL context
    // is current.  Pops the latest decoded frame from the internal queue
    // and uploads it to the OpenGL texture.
    void Update();

private:
    friend void Image(Player&, ImVec2);
    friend bool Video(Player&, ImVec2);

    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

// -----------------------------------------------------------------------
// Free helpers  —  designed to be called inside an ImGui window.
//
//   Image(player, size)
//     Draw the current video frame as a static Image.
//
//   Video(player, size)
//     Draw a full video-player widget: image + seekable timeline +
//     play/pause/stop buttons, time display, volume & speed sliders.
//     Returns false when the user clicks "Stop" or the video ends.
// -----------------------------------------------------------------------
void Image(Player& player, ImVec2 size = {0, 0});
bool Video(Player& player, ImVec2 size = {0, 0});

} // namespace imvideo
