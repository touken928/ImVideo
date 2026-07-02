// -----------------------------------------------------------------------
// player.cpp  —  Player::Impl  (v2.2 optimised pipeline)
//
// Optimisations from v2.1:
//   1. update() pops all due frames and keeps the LATEST for upload
//      (was: upload first due, drop later ones — showed stale frame).
//      Lookahead tightened from 50 ms → 8 ms (~½ frame at 60 fps).
//   2. process_video_packet() checks queue fullness BEFORE swscale
//      conversion; skips entirely when queue is saturated.
//   3. RGBA buffer recycling via VideoFrameQueue::acquire_buffer() /
//      recycle() — no per-frame vector resizing or reallocation.
//   4. FFmpeg video decoder threading enabled (thread_count = 0,
//      auto-detect cores + frame-level parallelism).
// -----------------------------------------------------------------------
#include "player_impl.hpp"

#include <libavutil/channel_layout.h>
#include <libavutil/rational.h>

#include <string>
#include <string_view>
#include <thread>

// -----------------------------------------------------------------------
// Internal URL/path helpers  (anonymous namespace)
// -----------------------------------------------------------------------
namespace {

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// Returns true for known URL schemes that avformat_open_input can handle
// natively (http/https/rtsp/rtmp) plus file:// which we convert locally.
bool is_network_url_scheme(std::string_view s) noexcept {
    return starts_with(s, "http://")
        || starts_with(s, "https://")
        || starts_with(s, "rtsp://")
        || starts_with(s, "rtmp://");
}

bool is_known_source_scheme(std::string_view s) noexcept {
    return starts_with(s, "file://")
        || is_network_url_scheme(s);
}

int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percent_decode(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_digit(s[i + 1]);
            int lo = hex_digit(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                r.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        r.push_back(s[i]);
    }
    return r;
}

// Convert a file:// URL to a local filesystem path.
//   file:///absolute/path   → /absolute/path
//   file://localhost/path    → /path
//   file:///path%20with%20spaces → /path with spaces
std::string file_url_to_path(std::string_view url) {
    auto s = url.substr(7); // after "file://"
    // Skip optional authority (non-empty host before the path)
    if (!s.empty() && s[0] != '/') {
        auto slash = s.find('/');
        std::string host = percent_decode(s.substr(0, slash));
        if (host == "localhost") {
            s = slash == std::string_view::npos ? std::string_view{} : s.substr(slash);
        } else {
#if defined(_WIN32)
            std::string path = "//" + host;
            if (slash != std::string_view::npos)
                path += percent_decode(s.substr(slash));
            return path;
#else
            s = slash == std::string_view::npos ? std::string_view{} : s.substr(slash);
#endif
        }
    }
    return percent_decode(s);
}

} // anonymous namespace

// -----------------------------------------------------------------------
namespace imvideo {
// -----------------------------------------------------------------------

// ========================================================================
// Construction / destruction
// ========================================================================

Player::Impl::Impl()  = default;
Player::Impl::~Impl() noexcept { close(); }

// ========================================================================
// open  (local filesystem path)
// ========================================================================

bool Player::Impl::open(const std::filesystem::path& path) {
    close();

    auto path_utf8 = path.u8string();
    const char* cpath = reinterpret_cast<const char*>(path_utf8.c_str());

    AVFormatContext* fmt = nullptr;
    int ret = avformat_open_input(&fmt, cpath, nullptr, nullptr);
    if (ret < 0) { set_av_error("avformat_open_input", ret); return false; }
    fmt_ctx_.reset(fmt);

    if (!finish_open()) { close(); return false; }
    return true;
}

// ========================================================================
// open  (string_view — auto-detect local path vs URL)
// ========================================================================

bool Player::Impl::open(std::string_view source) {
    if (source.empty()) {
        last_error_ = "Open: source is empty";
        return false;
    }

    if (!is_known_source_scheme(source)) {
        // No known URL scheme — treat as local filesystem path
        return open(std::filesystem::path(std::string(source)));
    }
    if (starts_with(source, "file://")) {
        // file:// URL → convert to local path
        return open(std::filesystem::path(file_url_to_path(source)));
    }
    // Network URL (http, https, rtsp, rtmp) — pass raw to avformat
    return open_url(source);
}

// ========================================================================
// open_url  (explicit URL open, rejects unsupported schemes)
// ========================================================================

bool Player::Impl::open_url(std::string_view url) {
    close();

    if (url.empty()) {
        last_error_ = "OpenUrl: URL is empty";
        return false;
    }
    if (!is_network_url_scheme(url)) {
        last_error_ = std::string("OpenUrl: unsupported URL scheme in \"")
                    + std::string(url) + "\"";
        return false;
    }

    // Pass the raw URL string to FFmpeg's avformat_open_input, which
    // dispatches to the appropriate URL protocol (http, rtsp, rtmp, …).
    AVFormatContext* fmt = nullptr;
    std::string url_str(url);
    int ret = avformat_open_input(&fmt, url_str.c_str(), nullptr, nullptr);
    if (ret < 0) { set_av_error("avformat_open_input", ret); return false; }
    fmt_ctx_.reset(fmt);

    if (!finish_open()) { close(); return false; }
    return true;
}

// ========================================================================
// finish_open  —  shared post-avformat_open_input setup
// ========================================================================

bool Player::Impl::finish_open() {
    int ret = avformat_find_stream_info(fmt_ctx_.get(), nullptr);
    if (ret < 0) { set_av_error("avformat_find_stream_info", ret); return false; }

    video_stream_idx_ = av_find_best_stream(fmt_ctx_.get(), AVMEDIA_TYPE_VIDEO,
                                            -1, -1, &video_codec_, 0);
    audio_stream_idx_ = av_find_best_stream(fmt_ctx_.get(), AVMEDIA_TYPE_AUDIO,
                                            -1, video_stream_idx_, &audio_codec_, 0);
    if (video_stream_idx_ < 0) {
        last_error_ = "No video stream found";
        return false;
    }

    // Open video decoder  (with FFmpeg threading — auto-detect cores)
    video_dec_ctx_.reset(avcodec_alloc_context3(video_codec_));
    if (!video_dec_ctx_) { last_error_ = "avcodec_alloc_context3 (video) failed"; return false; }
    ret = avcodec_parameters_to_context(video_dec_ctx_.get(),
                                        fmt_ctx_->streams[video_stream_idx_]->codecpar);
    if (ret < 0) { set_av_error("avcodec_parameters_to_context (video)", ret); return false; }
    video_dec_ctx_->thread_count = 0;   // auto-detect
    video_dec_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    ret = avcodec_open2(video_dec_ctx_.get(), video_codec_, nullptr);
    if (ret < 0) { set_av_error("avcodec_open2 (video)", ret); return false; }

    // Open audio decoder (optional, single-threaded)
    if (audio_stream_idx_ >= 0 && audio_codec_) {
        audio_dec_ctx_.reset(avcodec_alloc_context3(audio_codec_));
        if (audio_dec_ctx_) {
            ret = avcodec_parameters_to_context(audio_dec_ctx_.get(),
                                                fmt_ctx_->streams[audio_stream_idx_]->codecpar);
            if (ret == 0) {
                audio_dec_ctx_->thread_count = 1;
                ret = avcodec_open2(audio_dec_ctx_.get(), audio_codec_, nullptr);
                if (ret < 0) audio_dec_ctx_.reset();
            } else {
                audio_dec_ctx_.reset();
            }
        }
    }

    // Duration
    if (fmt_ctx_->duration != AV_NOPTS_VALUE)
        duration_ = static_cast<double>(fmt_ctx_->duration) / AV_TIME_BASE;
    else if (fmt_ctx_->streams[video_stream_idx_]->duration != AV_NOPTS_VALUE)
        duration_ = static_cast<double>(fmt_ctx_->streams[video_stream_idx_]->duration)
                    * av_q2d(fmt_ctx_->streams[video_stream_idx_]->time_base);

    if (AVStream* video_stream = fmt_ctx_->streams[video_stream_idx_]) {
        AVRational rate = av_guess_frame_rate(fmt_ctx_.get(), video_stream, nullptr);
        if (rate.num > 0 && rate.den > 0) {
            frame_interval_ = av_q2d(av_inv_q(rate));
        } else if (video_stream->avg_frame_rate.num > 0 && video_stream->avg_frame_rate.den > 0) {
            frame_interval_ = av_q2d(av_inv_q(video_stream->avg_frame_rate));
        } else if (video_stream->r_frame_rate.num > 0 && video_stream->r_frame_rate.den > 0) {
            frame_interval_ = av_q2d(av_inv_q(video_stream->r_frame_rate));
        }
        if (!(frame_interval_ > 0.0 && std::isfinite(frame_interval_))) {
            frame_interval_ = 1.0 / 30.0;
        }
    }

    // GL texture
    tex_w_ = video_dec_ctx_->width;
    tex_h_ = video_dec_ctx_->height;
    gl_create_texture(tex_w_, tex_h_);

    // Audio engine + resampler
    if (audio_dec_ctx_) {
        AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        SwrContext* swr = nullptr;
        swr_alloc_set_opts2(&swr,
                            &out_ch_layout, AV_SAMPLE_FMT_FLT, kAudioSampleRate,
                            &audio_dec_ctx_->ch_layout,
                            audio_dec_ctx_->sample_fmt, audio_dec_ctx_->sample_rate,
                            0, nullptr);
        if (swr) { swr_init(swr); swr_ctx_.reset(swr); }

        if (!audio_engine_.init(&audio_ring_, kAudioSampleRate, kAudioChannels, volume_)) {
            // non-fatal
        }
    }

    // Start worker
    worker_running_ = true;
    worker_ = std::thread(&Impl::worker_func, this);

    opened_ = true;
    return true;
}

// ========================================================================
// close
// ========================================================================

void Player::Impl::close() noexcept {
    opened_ = false;
    playing_ = false;
    paused_ = false;

    audio_engine_.stop();
    audio_engine_.shutdown();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_running_ = false;
        worker_playing_ = false;
    }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();

    gl_destroy_texture();
    worker_sws_ctx_.reset();
    video_queue_.clear();
    audio_ring_.clear();
    swr_ctx_.reset();
    audio_dec_ctx_.reset();
    video_dec_ctx_.reset();
    fmt_ctx_.reset();
    video_stream_idx_ = -1;
    audio_stream_idx_ = -1;
    video_codec_ = nullptr;
    audio_codec_ = nullptr;
    tex_w_ = tex_h_ = 0;
    duration_ = 0.0;
    frame_interval_ = 1.0 / 30.0;
    base_pos_ = pause_pos_ = 0.0;
    eof_.store(false, std::memory_order_relaxed);
    seek_pending_.store(false, std::memory_order_relaxed);
}

// ========================================================================
// Play / Pause / Stop / Seek
// ========================================================================

void Player::Impl::play() {
    if (!opened_) return;
    if (playing_ && !paused_) return;

    if (paused_) {
        base_time_ = Clock::now();
        base_pos_  = pause_pos_;
        paused_ = false;
        playing_ = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            worker_playing_ = true;
            worker_paused_ = false;
        }
        cv_.notify_one();
        if (audio_engine_.is_valid()) audio_engine_.start();
    } else {
        double pos = position();
        base_time_ = Clock::now();
        base_pos_  = pos;
        paused_ = false;
        playing_ = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            worker_playing_ = true;
            worker_paused_ = false;
        }
        cv_.notify_one();
        if (audio_engine_.is_valid()) {
            audio_engine_.reset_frames_consumed();
            audio_engine_.start();
        }
    }
}

void Player::Impl::pause() {
    if (!opened_ || paused_) return;
    pause_pos_ = position();
    paused_ = true;
    playing_ = false;
    audio_engine_.stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_playing_ = false;
        worker_paused_ = true;
    }
}

void Player::Impl::stop() {
    if (!opened_) return;
    playing_ = false;
    paused_ = false;
    audio_engine_.stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        seek_target_ = 0.0;
        seek_in_progress_ = false;
        seek_pending_.store(true, std::memory_order_release);
        worker_playing_ = true;
        worker_paused_ = false;
    }
    cv_.notify_one();

    while (seek_pending_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    VideoFrame f;
    if (video_queue_.pop(f)) { upload_frame(f); video_queue_.recycle(f); }

    base_time_ = Clock::now();
    base_pos_  = 0.0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_playing_ = false;
        worker_paused_ = true;
    }
}

void Player::Impl::seek(double seconds) {
    if (!opened_) return;
    seconds = std::clamp(seconds, 0.0, duration_);

    audio_engine_.stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        seek_target_ = seconds;
        seek_in_progress_ = false;
        seek_pending_.store(true, std::memory_order_release);
        worker_playing_ = true;
    }
    cv_.notify_one();

    while (seek_pending_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    base_time_ = Clock::now();
    base_pos_  = seconds;
    pause_pos_ = seconds;
    eof_.store(false, std::memory_order_relaxed);

    VideoFrame f;
    if (video_queue_.pop(f)) { upload_frame(f); video_queue_.recycle(f); }

    if (audio_engine_.is_valid()) {
        audio_engine_.reset_frames_consumed();
        if (playing_) audio_engine_.start();
    }

    if (paused_) {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_playing_ = false;
        worker_paused_ = true;
    }
}

// ========================================================================
// position  (stable wall clock)
// ========================================================================

double Player::Impl::position() const {
    if (!opened_) return 0.0;
    if (paused_) return pause_pos_;
    if (!playing_) return base_pos_;

    auto elapsed = std::chrono::duration<double>(Clock::now() - base_time_).count();
    return base_pos_ + elapsed * speed_;
}

// ========================================================================
// Volume / Speed / Texture
// ========================================================================

void Player::Impl::set_volume(float v) {
    volume_ = std::clamp(v, 0.0f, 1.0f);
    audio_engine_.set_volume(volume_);
}

void Player::Impl::set_speed(float s) {
    if (audio_engine_.is_valid()) speed_ = 1.0f;
    else                          speed_ = std::max(0.0625f, s);
}

ImTextureID Player::Impl::texture() const {
    return static_cast<ImTextureID>(static_cast<ImU64>(tex_id_));
}

// ========================================================================
// Update  (render thread)
//
//  1. Pop ALL frames with pts ≤ clock + 8 ms.
//  2. Upload the LATEST popped frame (closest to clock).
//  3. Tight lookahead minimises displayed-frame age.
// ========================================================================

void Player::Impl::update() {
    if (!opened_) return;

    const double pos = position();
    const double kLookahead = std::clamp(frame_interval_ * 0.5, 0.008, 0.025);

    // Pop all due frames; keep the latest one for upload.
    VideoFrame best;
    bool have_best = false;

    while (true) {
        const double front_pts = video_queue_.front_pts();
        if (front_pts < 0.0) break;
        if (front_pts > pos + kLookahead) break;  // future frame — stop

        VideoFrame f;
        if (!video_queue_.pop(f)) break;

        // Replace best with this newer due frame (move, no copy).
        if (have_best) {
            video_queue_.recycle(best);
        }
        best = std::move(f);
        have_best = true;
    }

    if (have_best && best.pts >= 0.0) {
        upload_frame(best);
        video_queue_.recycle(best);   // return buffer to pool
    }

    // EOS check
    if (eof_.load(std::memory_order_acquire)
        && video_queue_.size() == 0
        && pos >= duration_ - 0.05) {
        playing_ = false;
    }
}

// ========================================================================
// Worker thread
// ========================================================================

void Player::Impl::worker_func() {
    AvFramePtr video_frame(av_frame_alloc());
    AvFramePtr audio_frame(av_frame_alloc());
    AVPacket* pkt = av_packet_alloc();

    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !worker_running_
                    || (worker_playing_ && !worker_paused_)
                    || seek_pending_.load(std::memory_order_relaxed);
            });
            if (!worker_running_) break;
        }

        while (true) {
            if (!worker_running_) goto done;
            if (seek_pending_.load(std::memory_order_acquire)) {
                worker_perform_seek();
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!worker_playing_ || worker_paused_) break;
            }

            int ret = av_read_frame(fmt_ctx_.get(), pkt);
            if (ret < 0) {
                eof_.store(true, std::memory_order_release);
                break;
            }

            if (pkt->stream_index == video_stream_idx_) {
                process_video_packet(pkt, video_frame.get());
            } else if (pkt->stream_index == audio_stream_idx_
                       && audio_dec_ctx_ && audio_engine_.is_valid()) {
                process_audio_packet(pkt, audio_frame.get());
            }
            av_packet_unref(pkt);
        }

        if (eof_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(mutex_);
            worker_playing_ = false;
            worker_paused_ = true;
        }
    }

done:
    av_packet_free(&pkt);
}

// ========================================================================
// Worker seek
// ========================================================================

void Player::Impl::worker_perform_seek() {
    if (!fmt_ctx_) return;
    if (seek_in_progress_) return;
    seek_in_progress_ = true;

    double seconds = seek_target_;

    if (video_dec_ctx_) avcodec_flush_buffers(video_dec_ctx_.get());
    if (audio_dec_ctx_) avcodec_flush_buffers(audio_dec_ctx_.get());

    int stream_for_seek = (audio_stream_idx_ >= 0) ? audio_stream_idx_ : video_stream_idx_;
    AVRational tb = fmt_ctx_->streams[stream_for_seek]->time_base;
    int64_t ts = static_cast<int64_t>(seconds / av_q2d(tb));
    av_seek_frame(fmt_ctx_.get(), stream_for_seek, ts, AVSEEK_FLAG_BACKWARD);

    video_queue_.clear();
    audio_ring_.clear();

    // Decode one video frame after seek  (uses recycled buffer from pool)
    {
        AvFramePtr frame(av_frame_alloc());
        for (int tries = 0; tries < 300; ++tries) {
            AVPacket* p = av_packet_alloc();
            int r = av_read_frame(fmt_ctx_.get(), p);
            if (r < 0) { av_packet_free(&p); break; }
            if (p->stream_index == video_stream_idx_) {
                r = avcodec_send_packet(video_dec_ctx_.get(), p);
                av_packet_free(&p);
                if (r < 0) continue;
                r = avcodec_receive_frame(video_dec_ctx_.get(), frame.get());
                if (r == 0) {
                    int w = video_dec_ctx_->width;
                    int h = video_dec_ctx_->height;
                    AVPixelFormat src_fmt = video_dec_ctx_->pix_fmt;

                    // Ensure cached sws context
                    if (!worker_sws_ctx_ || worker_sws_w_ != w ||
                        worker_sws_h_ != h ||
                        worker_sws_fmt_ != static_cast<int>(src_fmt)) {
                        worker_sws_ctx_.reset(
                            sws_getContext(w, h, src_fmt, w, h, AV_PIX_FMT_RGBA,
                                           SWS_BILINEAR, nullptr, nullptr, nullptr));
                        if (worker_sws_ctx_) {
                            worker_sws_w_   = w;
                            worker_sws_h_   = h;
                            worker_sws_fmt_ = static_cast<int>(src_fmt);
                        }
                    }

                    if (!worker_sws_ctx_) break;

                    size_t needed = static_cast<size_t>(w * h) * 4;
                    std::vector<uint8_t> buf = video_queue_.acquire_buffer(needed);

                    uint8_t* dst[1]       = { buf.data() };
                    int      dst_stride[1] = { w * 4 };
                    sws_scale(worker_sws_ctx_.get(),
                              frame->data, frame->linesize, 0, h, dst, dst_stride);

                    VideoFrame vf;
                    vf.pts    = frame_pts(frame.get(), video_stream_idx_);
                    vf.width  = w;
                    vf.height = h;
                    vf.rgba   = std::move(buf);
                    video_queue_.push(std::move(vf));
                    break;
                }
            } else {
                av_packet_free(&p);
            }
        }
    }

    if (audio_engine_.is_valid()) audio_engine_.reset_frames_consumed();

    seek_pending_.store(false, std::memory_order_release);
    seek_in_progress_ = false;
}

// ========================================================================
// Process video packet  (worker thread)
//
// Key optimisations:
//   - Checks queue fullness BEFORE swscale (avoids wasted conversion).
//   - Uses queue.acquire_buffer() to get recycled RGBA buffer.
//   - Caches SwsContext across frames.
// ========================================================================

void Player::Impl::process_video_packet(AVPacket* pkt, AVFrame* frame) {
    int ret = avcodec_send_packet(video_dec_ctx_.get(), pkt);
    if (ret < 0) return;

    while (true) {
        ret = avcodec_receive_frame(video_dec_ctx_.get(), frame);
        if (ret != 0) break;

        double pts = frame_pts(frame, video_stream_idx_);
        if (pts < 0) continue;

        int w = video_dec_ctx_->width;
        int h = video_dec_ctx_->height;
        AVPixelFormat src_fmt = video_dec_ctx_->pix_fmt;

        while (video_queue_.size() >= VideoFrameQueue::kMaxFrames) {
            if (!worker_running_ || seek_pending_.load(std::memory_order_acquire)) return;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!worker_playing_ || worker_paused_) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // --- Cached SwsContext ---
        if (!worker_sws_ctx_ || worker_sws_w_ != w || worker_sws_h_ != h ||
            worker_sws_fmt_ != static_cast<int>(src_fmt)) {
            worker_sws_ctx_.reset(
                sws_getContext(w, h, src_fmt, w, h, AV_PIX_FMT_RGBA,
                               SWS_BILINEAR, nullptr, nullptr, nullptr));
            if (worker_sws_ctx_) {
                worker_sws_w_   = w;
                worker_sws_h_   = h;
                worker_sws_fmt_ = static_cast<int>(src_fmt);
            } else {
                continue;
            }
        }

        // --- Get a recycled buffer from the pool (no new allocation) ---
        size_t needed = static_cast<size_t>(w * h) * 4;
        std::vector<uint8_t> buf = video_queue_.acquire_buffer(needed);

        uint8_t* dst[1]       = { buf.data() };
        int      dst_stride[1] = { w * 4 };
        sws_scale(worker_sws_ctx_.get(),
                  frame->data, frame->linesize, 0, h, dst, dst_stride);

        VideoFrame vf;
        vf.pts    = pts;
        vf.width  = w;
        vf.height = h;
        vf.rgba   = std::move(buf);
        video_queue_.push(std::move(vf));
    }
}

// ========================================================================
// Process audio packet  (worker thread)
// ========================================================================

void Player::Impl::process_audio_packet(AVPacket* pkt, AVFrame* frame) {
    int ret = avcodec_send_packet(audio_dec_ctx_.get(), pkt);
    if (ret < 0) return;

    while (true) {
        ret = avcodec_receive_frame(audio_dec_ctx_.get(), frame);
        if (ret != 0) break;
        if (!swr_ctx_) continue;

        int out_samples = swr_get_out_samples(swr_ctx_.get(), frame->nb_samples);
        if (out_samples <= 0) continue;

        std::vector<float> pcm_buf(static_cast<size_t>(out_samples) * kAudioChannels);

        uint8_t* out_buf[1] = { reinterpret_cast<uint8_t*>(pcm_buf.data()) };
        int converted = swr_convert(swr_ctx_.get(),
                                    out_buf, out_samples,
                                    const_cast<const uint8_t**>(frame->data),
                                    frame->nb_samples);
        if (converted <= 0) continue;

        size_t total = static_cast<size_t>(converted) * kAudioChannels;
        size_t written = 0;

        while (written < total) {
            if (!worker_running_ || seek_pending_.load(std::memory_order_acquire)) return;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!worker_playing_ || worker_paused_) return;
            }

            size_t n = audio_ring_.write(pcm_buf.data() + written, total - written);
            written += n;

            if (written < total) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
}

// ========================================================================
// GL helpers  (render thread only)
// ========================================================================

void Player::Impl::gl_create_texture(int w, int h) {
    glGenTextures(1, &tex_id_);
    glBindTexture(GL_TEXTURE_2D, tex_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Player::Impl::gl_destroy_texture() {
    if (tex_id_) {
        glDeleteTextures(1, &tex_id_);
        tex_id_ = 0;
    }
}

void Player::Impl::upload_frame(const VideoFrame& f) {
    glBindTexture(GL_TEXTURE_2D, tex_id_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.width, f.height,
                    GL_RGBA, GL_UNSIGNED_BYTE, f.rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ========================================================================
// frame_pts
// ========================================================================

double Player::Impl::frame_pts(const AVFrame* f, int stream_idx) const {
    if (!f) return -1.0;
    AVRational tb = fmt_ctx_->streams[stream_idx]->time_base;
    int64_t pts = f->pts;
    if (pts == AV_NOPTS_VALUE) pts = f->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) return -1.0;
    return static_cast<double>(pts) * av_q2d(tb);
}

// ========================================================================
// set_av_error
// ========================================================================

void Player::Impl::set_av_error(const char* ctx, int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    last_error_ = std::string(ctx) + ": " + buf;
}

// ========================================================================
// Public API forwarders
// ========================================================================

Player::Player() : pimpl_(std::make_unique<Impl>()) {}
Player::~Player() = default;
Player::Player(Player&&) noexcept = default;
Player& Player::operator=(Player&&) noexcept = default;

bool   Player::Open(const std::filesystem::path& p)  { return pimpl_->open(p); }
bool   Player::Open(std::string_view s)              { return pimpl_->open(s); }
bool   Player::OpenUrl(std::string_view u)           { return pimpl_->open_url(u); }
void   Player::Close()                                { pimpl_->close(); }
void   Player::Play()                                 { pimpl_->play(); }
void   Player::Pause()                                { pimpl_->pause(); }
void   Player::Stop()                                 { pimpl_->stop(); }
void   Player::Seek(double s)                         { pimpl_->seek(s); }
double Player::Position() const                       { return pimpl_->position(); }
double Player::Duration() const                       { return pimpl_->duration(); }
bool   Player::IsPlaying() const                      { return pimpl_->is_playing(); }
void   Player::SetVolume(float v)                     { pimpl_->set_volume(v); }
void   Player::SetSpeed(float s)                      { pimpl_->set_speed(s); }
void   Player::Update()                               { pimpl_->update(); }
ImTextureID Player::Texture() const                   { return pimpl_->texture(); }
ImVec2 Player::VideoSize() const                      { return pimpl_->video_size(); }
const char* Player::LastError() const                 { return pimpl_->last_error(); }

// -----------------------------------------------------------------------
} // namespace imvideo
// -----------------------------------------------------------------------
