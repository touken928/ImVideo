// -----------------------------------------------------------------------
// player_impl.hpp  —  private shared header (v2.1)
//
// Full definition of Player::Impl with multi-threaded decode worker,
// audio engine integration, bounded frame/audio queues, and a stable wall clock.
//
// Key design for v2.1:
//   - VideoFrameQueue::front_ptr() returns const* + lock guard (zero copy)
//   - Worker caches SwsContext in `worker_sws_ctx_` (no per-frame alloc)
//   - Audio writes use interruptible backpressure: no normal-playback drops
//   - Seek → worker handles synchronously, renderer waits via spin
//   - Wall clock is the single playback clock; audio never blocks video timing
// -----------------------------------------------------------------------
#pragma once

#include "imvideo/imvideo.h"

// OpenGL — private to TUs that include this header.
#if defined(__APPLE__)
#   define GL_SILENCE_DEPRECATION
#   include <OpenGL/gl3.h>
#elif defined(_WIN32)
#   include <GL/gl.h>
#   include <GL/glext.h>   // GL_CLAMP_TO_EDGE etc. (mingw-w64)
#else
#   include <GL/gl.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include "audio_engine.hpp"
#include "ring_buffer.hpp"
#include "video_frame.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// -----------------------------------------------------------------------
namespace imvideo {
// -----------------------------------------------------------------------

// RAII deleters for FFmpeg resources
struct AvFormatCtxDeleter {
    void operator()(AVFormatContext* p) const noexcept { if (p) avformat_close_input(&p); }
};
using AvFormatCtxPtr = std::unique_ptr<AVFormatContext, AvFormatCtxDeleter>;

struct AvCodecCtxDeleter {
    void operator()(AVCodecContext* p) const noexcept { if (p) avcodec_free_context(&p); }
};
using AvCodecCtxPtr = std::unique_ptr<AVCodecContext, AvCodecCtxDeleter>;

struct AvFrameDeleter {
    void operator()(AVFrame* p) const noexcept { if (p) av_frame_free(&p); }
};
using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;

struct SwsCtxDeleter {
    void operator()(SwsContext* p) const noexcept { if (p) sws_freeContext(p); }
};
using SwsCtxPtr = std::unique_ptr<SwsContext, SwsCtxDeleter>;

struct SwrCtxDeleter {
    void operator()(SwrContext* p) const noexcept { if (p) swr_free(&p); }
};
using SwrCtxPtr = std::unique_ptr<SwrContext, SwrCtxDeleter>;

// -----------------------------------------------------------------------
// Configuration constants
// -----------------------------------------------------------------------
constexpr size_t kAudioRingCapacity = 1u << 18;   // float samples, power-of-2
constexpr int    kAudioSampleRate   = 48000;
constexpr int    kAudioChannels     = 2;

// -----------------------------------------------------------------------
// Player::Impl
// -----------------------------------------------------------------------
class Player::Impl {
public:
    Impl();
    ~Impl() noexcept;

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // ---- public API (called from render thread) -------------------------
    bool   open(const std::filesystem::path& path);
    void   close() noexcept;
    void   play();
    void   pause();
    void   stop();
    void   seek(double seconds);
    double position() const;
    double duration() const                { return opened_ ? duration_ : 0.0; }
    bool   is_playing() const              { return playing_; }
    bool   has_audio() const               { return audio_dec_ctx_ && audio_engine_.is_valid(); }
    bool   has_video() const               { return video_stream_idx_ >= 0; }
    ImTextureID texture() const;
    ImVec2 video_size() const              { return ImVec2(static_cast<float>(tex_w_),
                                                           static_cast<float>(tex_h_)); }
    const char* last_error() const         { return last_error_.c_str(); }
    void   set_volume(float v);
    void   set_speed(float s);
    void   update();

private:
    friend class Player;    // for pimpl_ access
    friend void Image(Player&, ImVec2);
    friend bool Video(Player&, ImVec2);

    // ---- FFmpeg resources (used by worker only) -------------------------
    AvFormatCtxPtr fmt_ctx_;

    // Video
    AvCodecCtxPtr  video_dec_ctx_;
    int            video_stream_idx_ = -1;
    const AVCodec* video_codec_     = nullptr;

    // Audio
    AvCodecCtxPtr  audio_dec_ctx_;
    int            audio_stream_idx_ = -1;
    const AVCodec* audio_codec_     = nullptr;
    SwrCtxPtr      swr_ctx_;

    // ---- Worker-cached SwsContext (no per-frame alloc) ------------------
    SwsCtxPtr      worker_sws_ctx_;
    int            worker_sws_w_   = 0;
    int            worker_sws_h_   = 0;
    int            worker_sws_fmt_ = 0;   // AVPixelFormat as int

    // ---- OpenGL (render thread only) ------------------------------------
    GLuint         tex_id_  = 0;
    int            tex_w_   = 0;
    int            tex_h_   = 0;

    // ---- Thread-safe queues ---------------------------------------------
    VideoFrameQueue       video_queue_;
    SpscRingBuffer<float> audio_ring_{kAudioRingCapacity * kAudioChannels};

    // ---- Audio engine ---------------------------------------------------
    AudioEngine    audio_engine_;

    // ---- Playback state (render-thread view) ----------------------------
    bool           opened_    = false;
    bool           playing_   = false;

    // ---- Worker thread --------------------------------------------------
    std::thread             worker_;

    // ---- State shared with worker (mutex + atomic) ----------------------
    std::mutex              mutex_;
    std::condition_variable cv_;
    bool                    worker_running_   = false;
    bool                    worker_playing_   = false;
    bool                    worker_paused_    = false;
    std::atomic<bool>       seek_pending_{false};
    bool                    seek_in_progress_ = false;  // guard against double-seek
    double                  seek_target_ = 0.0;
    std::atomic<bool>       eof_{false};

    // ---- Clock state (render thread, atomic inputs from audio) ----------
    using Clock = std::chrono::steady_clock;
    Clock::time_point       base_time_;
    double                  base_pos_       = 0.0;
    double                  pause_pos_      = 0.0;
    double                  duration_       = 0.0;
    double                  frame_interval_ = 1.0 / 30.0;
    bool                    paused_         = false;
    float                   volume_         = 1.0f;
    float                   speed_          = 1.0f;

    // ---- Error ----------------------------------------------------------
    std::string             last_error_;

    // ---- Internal helpers -----------------------------------------------
    void set_av_error(const char* ctx, int err);

    // -- Worker thread --
    void worker_func();
    void worker_perform_seek();

    // -- FFmpeg helpers (called from worker) --
    double frame_pts(const AVFrame* f, int stream_idx) const;
    void   process_video_packet(AVPacket* pkt, AVFrame* frame);
    void   process_audio_packet(AVPacket* pkt, AVFrame* frame);

    // -- GL helpers (render thread only) --
    void upload_frame(const VideoFrame& f);
    void gl_create_texture(int w, int h);
    void gl_destroy_texture();

    // -- Clock helpers --
    static double now_seconds() {
        return std::chrono::duration<double>(
            Clock::now().time_since_epoch()).count();
    }
};

// -----------------------------------------------------------------------
} // namespace imvideo
// -----------------------------------------------------------------------
