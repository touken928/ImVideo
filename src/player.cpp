#include <imvideo/player.hpp>

#include "audio_pipeline.hpp"
#include "ffmpeg_raii.hpp"
#include "frame_internal.hpp"
#include "packet_queue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#if !defined(__EMSCRIPTEN_PTHREADS__)
#error "imvideo requires Emscripten pthreads"
#endif
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace imvideo {
namespace {

constexpr double minimum_speed = 0.25;
constexpr double maximum_speed = 4.0;
constexpr double discard_slack_seconds = 0.0005;
constexpr double audio_drain_slack_seconds = 0.0001;
constexpr double audio_sync_lead_seconds = 0.04;
constexpr double maximum_video_lag_seconds = 0.08;
constexpr double audio_prefetch_lead_seconds = 0.12;
constexpr double maximum_audio_lead_seconds = 0.5;
constexpr auto audio_drain_poll_interval = std::chrono::milliseconds(5);
constexpr auto audio_lead_poll_interval = std::chrono::milliseconds(10);
constexpr auto video_reanchor_lag = std::chrono::milliseconds(50);
#if defined(__EMSCRIPTEN__)
// Keep audio-clock presentation jitter below a browser refresh interval.
constexpr auto audio_clock_poll_interval = std::chrono::milliseconds(2);
#else
constexpr auto audio_clock_poll_interval = std::chrono::milliseconds(10);
#endif

std::string ffmpeg_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

constexpr auto hardware_candidates() noexcept {
#if defined(__EMSCRIPTEN__)
    return std::array<AVHWDeviceType, 0>{};
#elif defined(_WIN32)
    return std::array{AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2};
#elif defined(__APPLE__)
    return std::array{AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
#else
    return std::array{AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA};
#endif
}

struct HardwareSelection {
    AVPixelFormat format = AV_PIX_FMT_NONE;
};

AVPixelFormat choose_hardware_format(AVCodecContext* context, const AVPixelFormat* formats) {
    const auto* selection = static_cast<HardwareSelection*>(context->opaque);
    if (!selection) return formats[0];
    for (const auto* current = formats; *current != AV_PIX_FMT_NONE; ++current) {
        if (*current == selection->format) return *current;
    }
    return formats[0];
}

struct PresentationClock {
    std::chrono::steady_clock::time_point origin{};
    double media_origin = 0.0;
    bool origin_set = false;
    std::uint64_t observed_revision = 0;
};

} // namespace

struct Player::Impl {
    ~Impl() { close(); }

    bool open(const Source& source, const Options& requested) {
        close();
        std::unique_lock life(lifecycle_mutex);
#if defined(__EMSCRIPTEN__)
        if (source.mode() != SourceMode::File)
            return fail_open("browser inputs must be downloaded to the virtual filesystem first");
#endif
        if (source.uri().empty()) return fail_open("source URI is empty");
        {
            std::lock_guard lock(error_mutex);
            error_message.clear();
        }
        loop = requested.loop;
        gpu_color_conversion = requested.gpu_color_conversion;
        collect_statistics = requested.collect_statistics;
        audio_clock_media_origin = 0.0;
        audio_epoch_pending = true;
        audio_epoch_anchored = false;
        hardware_selection.format = AV_PIX_FMT_NONE;
        {
            std::lock_guard lock(open_mutex);
            open_thread = std::this_thread::get_id();
            open_active = true;
        }
        {
            std::unique_lock lock(control_mutex);
            stop_requested = false;
            state_value = State::Opening;
        }

        format = avformat_alloc_context();
        if (!format) return fail_open("cannot allocate input context");
        format->interrupt_callback.callback = [](void* opaque) {
            const auto* self = static_cast<Impl*>(opaque);
            return self->stop_requested.load() ? 1 : 0;
        };
        format->interrupt_callback.opaque = this;
        AVDictionary* dictionary = nullptr;
        if (source.mode() == SourceMode::Rtsp) av_dict_set(&dictionary, "rtsp_transport", "tcp", 0);
        life.unlock();
        int result = avformat_open_input(&format, source.uri().c_str(), nullptr, &dictionary);
        av_dict_free(&dictionary);
        life.lock();
        if (stop_requested) return fail_open("open cancelled");
        if (result < 0) return fail_open("cannot open input: " + ffmpeg_error(result));

        life.unlock();
        result = avformat_find_stream_info(format, nullptr);
        life.lock();
        if (stop_requested) return fail_open("open cancelled");
        if (result < 0) return fail_open("cannot read stream information: " + ffmpeg_error(result));

        video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_stream < 0) return fail_open("input has no decodable video stream");
        std::string decoder_error;
        if (!open_decoder(video_stream, video_codec, true, decoder_error)) return fail_open(std::move(decoder_error));

        if (requested.audio_sink) {
            audio_stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, video_stream, nullptr, 0);
            if (audio_stream < 0 && audio_stream != AVERROR_STREAM_NOT_FOUND && requested.fail_on_audio_error)
                return fail_open("cannot select audio stream: " + ffmpeg_error(audio_stream));
            decoder_error.clear();
            if (audio_stream >= 0 && !open_decoder(audio_stream, audio_codec, false, decoder_error)) {
                if (requested.fail_on_audio_error) return fail_open(std::move(decoder_error));
                avcodec_free_context(&audio_codec);
                audio_stream = -1;
            }
            if (audio_codec &&
                !audio.open(audio_codec, requested.audio_sink, volume_value, playback_rate_value.load())) {
                if (requested.fail_on_audio_error) return fail_open("cannot open audio filter or output sink");
                avcodec_free_context(&audio_codec);
                audio_stream = -1;
            }
        }
        if (stop_requested) return fail_open("open cancelled");

        read_timeline();
        switch (source.mode()) {
        case SourceMode::Rtsp:
            live_value = true;
            break;
        case SourceMode::File:
            live_value = false;
            break;
        case SourceMode::Url:
            live_value = duration_value <= 0.0;
            break;
        }
        seekable_value = !live_value && (format->pb == nullptr || (format->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0);
        {
            std::unique_lock lock(control_mutex);
            if (stop_requested) {
                lock.unlock();
                return fail_open("open cancelled");
            }
            worker_running = true;
            seek_completed = true;
            seek_request = -1.0;
            try {
                worker = std::thread([this] { decode_loop(); });
            } catch (...) {
                worker_running = false;
                // Release the control lock before fail_open publishes the terminal state.
                lock.unlock();
                return fail_open("cannot start playback thread");
            }
            state_value = requested.autoplay ? State::Playing : State::Paused;
        }
        condition.notify_all();
        finish_open();
        return true;
    }

    bool fail_open(std::string message) {
        set_error(std::move(message));
        release_media();
        {
            std::lock_guard lock(control_mutex);
            state_value = State::Error;
        }
        condition.notify_all();
        finish_open();
        return false;
    }

    void finish_open() {
        {
            std::lock_guard lock(open_mutex);
            open_active = false;
        }
        open_finished.notify_all();
    }

    void read_timeline() {
        start_time_value = 0.0;
        if (format->start_time != AV_NOPTS_VALUE)
            start_time_value = static_cast<double>(format->start_time) / AV_TIME_BASE;
        else if (const AVStream* stream = format->streams[video_stream]; stream->start_time != AV_NOPTS_VALUE)
            start_time_value = stream->start_time * av_q2d(stream->time_base);
        if (format->duration != AV_NOPTS_VALUE)
            duration_value = static_cast<double>(format->duration) / AV_TIME_BASE;
        else
            duration_value = 0.0;
    }

    bool open_decoder(int stream_index, AVCodecContext*& context, bool hardware, std::string& error) {
        const AVCodecParameters* parameters = format->streams[stream_index]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) {
            error = "decoder is unavailable";
            return false;
        }
        context = avcodec_alloc_context3(codec);
        if (!context) {
            error = "cannot allocate decoder";
            return false;
        }
        int result = avcodec_parameters_to_context(context, parameters);
        if (result < 0) {
            error = "cannot configure decoder: " + ffmpeg_error(result);
            return false;
        }

        if (hardware) configure_hardware(codec, context);
#if defined(__EMSCRIPTEN__)
        context->thread_count = 1;
        context->max_pixels = maximum_web_pixels;
        if (static_cast<std::int64_t>(context->width) * context->height > maximum_web_pixels) {
            error = "video exceeds the browser build's 2073600-pixel limit";
            return false;
        }
#endif
        result = avcodec_open2(context, codec, nullptr);
        if (result < 0 && hardware && hardware_selection.format != AV_PIX_FMT_NONE) {
            av_buffer_unref(&context->hw_device_ctx);
            avcodec_free_context(&context);
            hardware_selection.format = AV_PIX_FMT_NONE;
            context = avcodec_alloc_context3(codec);
            if (!context) {
                error = "cannot allocate decoder";
                return false;
            }
            result = avcodec_parameters_to_context(context, parameters);
            if (result < 0) {
                error = "cannot configure decoder: " + ffmpeg_error(result);
                return false;
            }
            result = avcodec_open2(context, codec, nullptr);
        }
        if (result < 0) {
            error = "cannot open decoder: " + ffmpeg_error(result);
            return false;
        }
        return true;
    }

    void configure_hardware(const AVCodec* codec, AVCodecContext* context) {
        hardware_selection.format = AV_PIX_FMT_NONE;
        for (const auto type : hardware_candidates()) {
            for (int i = 0;; ++i) {
                const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
                if (!config) break;
                if (config->device_type != type || !(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                    continue;
                AVBufferRef* device = nullptr;
                if (av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) < 0) continue;
                hardware_selection.format = config->pix_fmt;
                context->opaque = &hardware_selection;
                context->get_format = choose_hardware_format;
                context->hw_device_ctx = av_buffer_ref(device);
                av_buffer_unref(&device);
                if (!context->hw_device_ctx) {
                    hardware_selection.format = AV_PIX_FMT_NONE;
                    continue;
                }
                return;
            }
        }
    }

    void close() {
        std::unique_lock life(lifecycle_mutex);
        bool wait_for_open = false;
        {
            std::lock_guard lock(open_mutex);
            wait_for_open = open_active && open_thread != std::this_thread::get_id();
        }
        {
            std::lock_guard lock(control_mutex);
            stop_requested = true;
        }
        condition.notify_all();
        if (wait_for_open) {
            life.unlock();
            std::unique_lock lock(open_mutex);
            open_finished.wait(lock, [this] { return !open_active; });
            lock.unlock();
            life.lock();
        }
        {
            std::lock_guard lock(control_mutex);
            stop_requested = true;
        }
        condition.notify_all();
        if (worker.joinable()) worker.join();
        release_media();
        {
            std::lock_guard lock(error_mutex);
            error_message.clear();
        }
        position_value = duration_value = start_time_value = 0.0;
        decoded_frames = cpu_converted_frames = gpu_prepared_frames = 0;
        decode_ns = display_ns = 0;
        audio_clock_media_origin = 0.0;
        audio_epoch_pending = true;
        audio_epoch_anchored = false;
        seekable_value = live_value = false;
        playback_rate_value = 1.0;
        presentation_revision = 0;
        seek_request = -1.0;
        seek_completed = true;
        seek_ok = false;
        worker_running = false;
        discard_until = -1.0;
        {
            std::lock_guard lock(control_mutex);
            state_value = State::Idle;
        }
        condition.notify_all();
    }

    void release_media() {
        audio.close();
        avcodec_free_context(&audio_codec);
        avcodec_free_context(&video_codec);
        avformat_close_input(&format);
        {
            std::lock_guard lock(frame_mutex);
            latest = {};
        }
        video_stream = audio_stream = -1;
        held_packets.release();
        prefetch_packet.reset();
        prefetch_frame.reset();
        software_frame.reset();
        input_eof = false;
        audio_exhausted = false;
        hardware_selection.format = AV_PIX_FMT_NONE;
        display.reset();
    }

    void finish_worker() {
        {
            std::lock_guard lock(control_mutex);
            worker_running = false;
            if (!seek_completed) {
                seek_ok = false;
                seek_completed = true;
            }
        }
        condition.notify_all();
    }

    void decode_loop() {
        auto packet = make_av_packet();
        auto decoded = make_av_frame();
        PresentationClock clock;
        clock.observed_revision = presentation_revision.load();
        if (!packet || !decoded) {
            set_error("cannot allocate decode buffers");
            set_state(State::Error);
            finish_worker();
            return;
        }

        while (!stop_requested) {
            {
                std::unique_lock lock(control_mutex);
                condition.wait(lock, [this] {
                    return stop_requested.load() || state_value.load() == State::Playing || seek_request >= 0.0;
                });
                if (stop_requested) break;
            }

            double relative = 0.0;
            if (take_seek(relative)) {
                apply_seek(relative, clock, true);
                continue;
            }
            if (state_value != State::Playing) continue;

            const int read_result = read_next_packet(packet.get());
            if (read_result < 0) {
                av_packet_unref(packet.get());
                if (stop_requested) break;
                if (read_result == AVERROR_EXIT || read_result == AVERROR(EAGAIN)) continue;
                if (read_result == AVERROR_EOF) {
                    finish_input(decoded.get(), clock);
                    continue;
                }
                set_error("input read failed: " + ffmpeg_error(read_result));
                set_state(State::Error);
                continue;
            }

            if (packet->stream_index == video_stream) {
                if (send_video_packet(packet.get()) >= 0) receive_video(decoded.get(), clock);
            } else if (audio_codec && packet->stream_index == audio_stream) {
                decode_audio(packet.get(), decoded.get());
            }
            av_packet_unref(packet.get());
        }
        finish_worker();
    }

    void finish_input(AVFrame* decoded, PresentationClock& clock) {
        finish_audio(decoded);
        if (state_value == State::Error) return;
        drain_video(decoded, clock);
        if (audio.opened() && !live_value) {
            // Do not pause or flush a device while its final PCM is still
            // queued. This also preserves the tail before a looping seek.
            const double target = audio.written_seconds();
            while (audio.sink()->clock_seconds() + audio_drain_slack_seconds < target) {
                std::unique_lock lock(control_mutex);
                if (stop_requested || seek_request >= 0.0) return;
                condition.wait_for(lock, audio_drain_poll_interval);
            }
        }
        if (stop_requested) return;
        if (seek_pending()) return;
        if (loop && seekable_value) {
            apply_seek(0.0, clock, false);
            return;
        }
        set_state(State::Ended);
        if (audio.opened()) audio.sink()->pause(true);
    }

    int read_next_packet(AVPacket* packet) {
        if (held_packets.pop(packet)) return 0;
        if (input_eof) return AVERROR_EOF;
        const int result = av_read_frame(format, packet);
        if (result == AVERROR_EOF) input_eof = true;
        return result;
    }

    // Separate reusable scratch storage is required: audio is decoded while
    // a video frame is still waiting for its presentation deadline.
    bool pump_audio() {
        if (held_packets.full() || input_eof || stop_requested || !audio_codec) return false;
        if (!prefetch_packet) prefetch_packet = make_av_packet();
        if (!prefetch_frame) prefetch_frame = make_av_frame();
        if (!prefetch_packet || !prefetch_frame) {
            set_error("cannot allocate audio prefetch buffers");
            set_state(State::Error);
            return false;
        }
        auto* packet = prefetch_packet.get();
        auto* decoded = prefetch_frame.get();
        bool wrote = false;
        while (!held_packets.full() && !stop_requested && !input_eof) {
            const int result = av_read_frame(format, packet);
            if (result == AVERROR_EOF) {
                input_eof = true;
                finish_audio(decoded);
                break;
            }
            if (result < 0) break;
            if (packet->stream_index == audio_stream) {
                decode_audio(packet, decoded);
                wrote = true;
                break;
            }
            // Streams which the player does not consume need not occupy the
            // bounded video queue (subtitles/attachments/alternate tracks).
            if (packet->stream_index == video_stream && !held_packets.push(packet)) {
                set_error("cannot buffer video packet");
                set_state(State::Error);
                break;
            }
            av_packet_unref(packet);
        }
        av_frame_unref(decoded);
        av_packet_unref(packet);
        return wrote;
    }

    bool seek_pending() {
        std::lock_guard lock(control_mutex);
        return seek_request >= 0.0;
    }

    bool take_seek(double& relative) {
        std::lock_guard lock(control_mutex);
        if (seek_request < 0.0) return false;
        relative = std::exchange(seek_request, -1.0);
        return true;
    }

    bool apply_seek(double relative, PresentationClock& clock, bool notify_waiter) {
        const double absolute = start_time_value + relative;
        const auto timestamp = static_cast<std::int64_t>(std::llround(absolute * AV_TIME_BASE));
        bool ok = format && av_seek_frame(format, -1, timestamp, AVSEEK_FLAG_BACKWARD) >= 0;
        if (ok) {
            if (video_codec) avcodec_flush_buffers(video_codec);
            if (audio_codec) avcodec_flush_buffers(audio_codec);
            if (audio.opened() && !audio.reset_tempo(playback_rate_value.load())) {
                set_error("cannot reset audio tempo filter after seek");
                set_state(State::Error);
                ok = false;
            }
        }
        if (ok) {
            held_packets.clear();
            input_eof = false;
            audio_exhausted = false;
            position_value = relative;
            audio_clock_media_origin = relative;
            audio_epoch_pending = true;
            audio_epoch_anchored = false;
            discard_until = absolute;
            clock.origin_set = false;
            std::lock_guard frame_lock(frame_mutex);
            latest = {};
        }
        if (notify_waiter) {
            {
                std::lock_guard lock(control_mutex);
                seek_ok = ok;
                seek_completed = true;
            }
            condition.notify_all();
        }
        return ok;
    }

    int send_video_packet(const AVPacket* packet) {
        const auto started =
            collect_statistics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const int result = avcodec_send_packet(video_codec, packet);
        if (collect_statistics)
            decode_ns.fetch_add(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                    .count()));
        return result;
    }

    void receive_video(AVFrame* decoded, PresentationClock& clock) {
        for (;;) {
            const auto started =
                collect_statistics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const int result = avcodec_receive_frame(video_codec, decoded);
            if (collect_statistics)
                decode_ns.fetch_add(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count()));
            if (result < 0) break;
            if (collect_statistics) ++decoded_frames;
            const bool keep_going = present_video_frame(decoded, clock);
            av_frame_unref(decoded);
            if (!keep_going) break;
        }
    }

    void drain_video(AVFrame* decoded, PresentationClock& clock) {
        if (!video_codec || send_video_packet(nullptr) < 0) return;
        receive_video(decoded, clock);
    }

    void drain_audio(AVFrame* decoded) {
        if (!audio_codec || avcodec_send_packet(audio_codec, nullptr) < 0) return;
        while (avcodec_receive_frame(audio_codec, decoded) >= 0) {
            if (!stop_requested) emit_audio_frame(decoded);
            av_frame_unref(decoded);
            if (state_value == State::Error) return;
        }
    }

    void finish_audio(AVFrame* decoded) {
        if (audio_exhausted || !audio_codec) return;
        drain_audio(decoded);
        if (audio.opened() && !audio.drain()) {
            set_error("cannot drain audio at end of input");
            set_state(State::Error);
        }
        audio_exhausted = true;
    }

    bool present_video_frame(AVFrame* decoded, PresentationClock& clock) {
        const bool has_pts = decoded->best_effort_timestamp != AV_NOPTS_VALUE;
        const double seconds = has_pts ? frame_seconds(decoded, format->streams[video_stream]) : 0.0;
        if (has_pts && discard_until >= 0.0 && seconds + discard_slack_seconds < discard_until) return !stop_requested;
        if (!live_value && has_pts) {
            if (!wait_until_due(clock, seconds)) return false;
        } else if (stop_requested) {
            return false;
        }
        publish_frame(decoded);
        if (has_pts) position_value = relative_media_seconds(seconds);
        return !stop_requested;
    }

    bool wait_until_due(PresentationClock& clock, double seconds) {
        std::unique_lock lock(control_mutex);
        if (stop_requested || seek_request >= 0.0) return false;
        const bool master_audio =
            audio.opened() && !live_value &&
            !(audio_exhausted && audio.sink()->clock_seconds() + audio_drain_slack_seconds >= audio.written_seconds());
        const double relative = relative_media_seconds(seconds);
        const auto blocked = [this] {
            return stop_requested.load() || state_value.load() != State::Playing || seek_request >= 0.0;
        };
        while (!blocked()) {
            const auto revision = presentation_revision.load();
            if (revision != clock.observed_revision) {
                clock.observed_revision = revision;
                clock.origin_set = false;
                lock.unlock();
                if (!synchronize_audio_rate()) return false;
                lock.lock();
                if (stop_requested || seek_request >= 0.0) return false;
                if (state_value != State::Playing || presentation_revision.load() != revision) continue;
            }
            if (!clock.origin_set) {
                clock.origin = std::chrono::steady_clock::now();
                clock.media_origin = seconds;
                clock.origin_set = true;
            }
            const double rate = std::max(playback_rate_value.load(), minimum_speed);
            if (master_audio) {
                while (!blocked() && presentation_revision.load() == revision) {
                    lock.unlock();
                    const bool rate_ok = synchronize_audio_rate();
                    lock.lock();
                    if (!rate_ok) return false;
                    if (blocked() || presentation_revision.load() != revision) break;
                    const double played = audio_clock_media_origin +
                                          audio.sink()->clock_seconds() * std::max(audio.applied_rate(), minimum_speed);
                    if (played + audio_sync_lead_seconds >= relative) break;
                    if (audio_exhausted &&
                        audio.sink()->clock_seconds() + audio_drain_slack_seconds >= audio.written_seconds()) {
                        // Audio can end before the video or a tempo filter can
                        // emit fewer samples than the remaining video spans.
                        break;
                    }
                    lock.unlock();
                    const bool wrote_audio = pump_audio();
                    lock.lock();
                    if (stop_requested || seek_request >= 0.0) return false;
                    if (!wrote_audio)
                        condition.wait_for(lock, audio_clock_poll_interval, [this, revision, &blocked] {
                            return blocked() || presentation_revision.load() != revision;
                        });
                }
            }
            // A device consumes audio in blocks. Releasing video immediately
            // when a block crosses the audio threshold produces alternating
            // short/long frame intervals. Keep a steady cadence as well as
            // honoring the audio master, including after a clock re-anchor.
            const auto paced_delay = std::chrono::duration<double>((seconds - clock.media_origin) / rate);
            const auto due =
                clock.origin + std::chrono::duration_cast<std::chrono::steady_clock::duration>(paced_delay);
            const bool catch_up =
                master_audio && relative + maximum_video_lag_seconds <
                                    audio_clock_media_origin + audio.sink()->clock_seconds() * audio.applied_rate();
            if (catch_up || (master_audio && std::chrono::steady_clock::now() > due + video_reanchor_lag)) {
                clock.origin = std::chrono::steady_clock::now();
                clock.media_origin = seconds;
            } else if (master_audio) {
                while (!blocked() && presentation_revision.load() == revision &&
                       std::chrono::steady_clock::now() < due) {
                    // Waiting for video must not starve the same thread's
                    // audio producer. Maintain a short, bounded PCM lead.
                    if (!audio_exhausted &&
                        audio.written_seconds() - audio.sink()->clock_seconds() < audio_prefetch_lead_seconds) {
                        lock.unlock();
                        const bool wrote_audio = pump_audio();
                        lock.lock();
                        if (wrote_audio) continue;
                    }
                    condition.wait_until(
                        lock, std::min(due, std::chrono::steady_clock::now() + audio_clock_poll_interval),
                        [this, revision, &blocked] { return blocked() || presentation_revision.load() != revision; });
                }
            } else {
                condition.wait_until(lock, due, [this, revision, &blocked] {
                    return blocked() || presentation_revision.load() != revision;
                });
            }
            if (stop_requested || seek_request >= 0.0) return false;
            if (presentation_revision.load() != revision) continue;
            if (state_value != State::Playing) {
                condition.wait(lock, [this] {
                    return stop_requested.load() || state_value.load() == State::Playing || seek_request >= 0.0;
                });
                if (stop_requested || seek_request >= 0.0) return false;
                clock.observed_revision = presentation_revision.load();
                clock.origin = std::chrono::steady_clock::now();
                clock.media_origin = seconds;
                clock.origin_set = true;
                continue;
            }
            return true;
        }
        return false;
    }

    void publish_frame(AVFrame* decoded) {
        const auto started =
            collect_statistics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        AVFrame* source = decoded;
        const auto pixel_format = static_cast<AVPixelFormat>(decoded->format);
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixel_format);
        if (descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0) {
            if (!software_frame) software_frame = make_av_frame();
            if (!software_frame) return;
            av_frame_unref(software_frame.get());
            if (av_hwframe_transfer_data(software_frame.get(), decoded, 0) < 0 ||
                av_frame_copy_props(software_frame.get(), decoded) < 0)
                return;
            source = software_frame.get();
        }
        DisplayGeometry geometry;
        AVFrame* display =
            this->display.convert(source, format->streams[video_stream], gpu_color_conversion, &geometry);
        if (source != decoded) av_frame_unref(software_frame.get());
#if defined(__EMSCRIPTEN__)
        if (!display) {
            set_error("cannot convert video within the browser display limits");
            set_state(State::Error);
        }
#endif
        if (!display) return;
        if (collect_statistics) {
            display_ns.fetch_add(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                    .count()));
            if (display->format == AV_PIX_FMT_RGBA)
                ++cpu_converted_frames;
            else
                ++gpu_prepared_frames;
        }
        Frame next;
        next.impl_ = std::make_shared<Frame::Impl>(display, decoded->best_effort_timestamp, geometry);
        std::lock_guard lock(frame_mutex);
        latest = std::move(next);
    }

    static double frame_seconds(const AVFrame* frame, const AVStream* stream) {
        if (frame->best_effort_timestamp == AV_NOPTS_VALUE) return 0.0;
        return frame->best_effort_timestamp * av_q2d(stream->time_base);
    }

    double relative_media_seconds(double absolute) const {
        const double relative = absolute - start_time_value;
        if (relative < 0.0 && relative > -0.05) return 0.0;
        return std::max(0.0, relative);
    }

    void decode_audio(AVPacket* packet, AVFrame* decoded) {
        if (avcodec_send_packet(audio_codec, packet) < 0) return;
        while (avcodec_receive_frame(audio_codec, decoded) >= 0) {
            emit_audio_frame(decoded);
            av_frame_unref(decoded);
            if (state_value == State::Error) return;
        }
    }

    void emit_audio_frame(AVFrame* decoded) {
        if (discard_audio(decoded)) return;
        if (!synchronize_audio_rate()) return;
        if (decoded->best_effort_timestamp != AV_NOPTS_VALUE && audio_stream >= 0) {
            const double seconds = frame_seconds(decoded, format->streams[audio_stream]);
            const double relative = relative_media_seconds(seconds);
            anchor_audio_epoch(relative);
            if (!wait_for_audio_lead(relative, presentation_revision.load())) return;
        }
        if (!audio.consume(decoded)) {
            set_error("cannot resample filtered audio");
            set_state(State::Error);
        }
    }

    bool wait_for_audio_lead(double relative, std::uint64_t synced_revision) {
        if (!audio.opened() || live_value) return true;
        std::unique_lock lock(control_mutex);
        const auto blocked = [this] {
            return stop_requested.load() || seek_request >= 0.0;
        };
        while (!blocked()) {
            if (state_value != State::Playing) {
                condition.wait(lock, [this, &blocked] { return blocked() || state_value.load() == State::Playing; });
                continue;
            }
            if (presentation_revision.load() != synced_revision) {
                const auto requested_revision = presentation_revision.load();
                lock.unlock();
                if (!synchronize_audio_rate()) return false;
                anchor_audio_epoch(relative);
                lock.lock();
                if (blocked()) break;
                synced_revision = requested_revision;
                continue;
            }
            if (state_value != State::Playing) continue;
            if (audio_epoch_pending && !audio.wrote_samples()) return true;
            const double played = audio_clock_media_origin +
                                  audio.sink()->clock_seconds() * std::max(audio.applied_rate(), minimum_speed);
            if (relative <= played + maximum_audio_lead_seconds) return true;
            condition.wait_for(lock, audio_lead_poll_interval, blocked);
        }
        return !stop_requested && seek_request < 0.0;
    }

    void anchor_audio_epoch(double relative_seconds) {
        if (!audio_epoch_pending || audio_epoch_anchored) return;
        audio_clock_media_origin = std::max(audio_clock_media_origin, relative_seconds);
        audio_epoch_anchored = true;
    }

    bool discard_audio(const AVFrame* frame) const {
        if (discard_until < 0.0 || audio_stream < 0 || frame->best_effort_timestamp == AV_NOPTS_VALUE) return false;
        const double seconds = frame->best_effort_timestamp * av_q2d(format->streams[audio_stream]->time_base);
        return seconds + discard_slack_seconds < discard_until;
    }

    bool synchronize_audio_rate() {
        const double requested_rate = playback_rate_value.load();
        if (audio.opened() && requested_rate != audio.applied_rate()) {
            // Preserve the media clock using the rate that was active before the flush.
            const double played = audio_clock_media_origin +
                                  audio.sink()->clock_seconds() * std::max(audio.applied_rate(), minimum_speed);
            if (!audio.reset_tempo(requested_rate)) {
                set_error("cannot configure audio tempo filter");
                set_state(State::Error);
                return false;
            }
            audio_clock_media_origin = std::max(audio_clock_media_origin, played);
            audio_epoch_pending = true;
            audio_epoch_anchored = false;
        }
        return true;
    }

    void set_state(State state) {
        {
            std::lock_guard lock(control_mutex);
            state_value = state;
        }
        condition.notify_all();
    }

    void set_error(std::string message) {
        std::lock_guard lock(error_mutex);
        error_message = std::move(message);
    }

    AVFormatContext* format = nullptr;
    AVCodecContext* video_codec = nullptr;
    AVCodecContext* audio_codec = nullptr;
    int video_stream = -1;
    int audio_stream = -1;
    HardwareSelection hardware_selection;
    AudioPipeline audio;
    DisplayConverter display;
    bool loop = false;
    bool gpu_color_conversion = false;
    bool collect_statistics = false;
    std::atomic<std::uint64_t> decoded_frames{0};
    std::atomic<std::uint64_t> cpu_converted_frames{0};
    std::atomic<std::uint64_t> gpu_prepared_frames{0};
    std::atomic<std::uint64_t> decode_ns{0};
    std::atomic<std::uint64_t> display_ns{0};
    std::thread worker;
    std::condition_variable condition;
    std::mutex lifecycle_mutex;
    std::mutex open_mutex;
    std::condition_variable open_finished;
    bool open_active = false;
    std::thread::id open_thread{};
    std::mutex control_mutex;
    std::mutex frame_mutex;
    mutable std::mutex error_mutex;
    Frame latest;
    std::string error_message;
    std::atomic<State> state_value{State::Idle};
    std::atomic<bool> stop_requested{false};
    std::atomic<double> position_value{0.0};
    double duration_value = 0.0;
    double start_time_value = 0.0;
    bool seekable_value = false;
    bool live_value = false;
    std::atomic<float> volume_value{1.0F};
    std::atomic<double> playback_rate_value{1.0};
    std::atomic<std::uint64_t> presentation_revision{0};
    double seek_request = -1.0;
    bool seek_completed = true;
    bool seek_ok = false;
    bool worker_running = false;
    double discard_until = -1.0;
    double audio_clock_media_origin = 0.0;
    bool audio_epoch_pending = true;
    bool audio_epoch_anchored = false;
    PacketQueue held_packets;
    AvPacketPtr prefetch_packet;
    AvFramePtr prefetch_frame;
    AvFramePtr software_frame;
    bool input_eof = false;
    bool audio_exhausted = false;
};

Player::Player() : impl_(std::make_unique<Impl>()) {}
Player::~Player() = default;
Player::Player(Player&&) noexcept = default;
Player& Player::operator=(Player&&) noexcept = default;
bool Player::open(const Source& source, const Options& options) { return impl_->open(source, options); }
void Player::close() { impl_->close(); }
void Player::play() {
    if (impl_->state_value == State::Ended && impl_->seekable_value) seek(0.0);
    bool resume_audio = false;
    {
        std::lock_guard lock(impl_->control_mutex);
        const auto state = impl_->state_value.load();
        if (state != State::Paused && state != State::Ended) return;
        ++impl_->presentation_revision;
        impl_->state_value = State::Playing;
        resume_audio = impl_->audio.opened();
    }
    impl_->condition.notify_all();
    if (resume_audio) impl_->audio.sink()->pause(false);
}
void Player::pause() {
    bool pause_audio = false;
    {
        std::lock_guard lock(impl_->control_mutex);
        if (impl_->state_value != State::Playing) return;
        impl_->state_value = State::Paused;
        pause_audio = impl_->audio.opened();
    }
    impl_->condition.notify_all();
    if (pause_audio) impl_->audio.sink()->pause(true);
}
void Player::stop() {
    const auto current = impl_->state_value.load();
    if (current == State::Idle || current == State::Opening || current == State::Error) return;
    if (impl_->seekable_value) seek(0.0);
    bool pause_audio = false;
    {
        std::lock_guard lock(impl_->control_mutex);
        const auto state = impl_->state_value.load();
        if (state == State::Idle || state == State::Opening || state == State::Error) return;
        impl_->state_value = State::Paused;
        pause_audio = impl_->audio.opened();
    }
    impl_->condition.notify_all();
    if (pause_audio) impl_->audio.sink()->pause(true);
}
bool Player::seek(double seconds) {
    if (!std::isfinite(seconds)) return false;
    std::unique_lock lock(impl_->control_mutex);
    const auto state = impl_->state_value.load();
    if (!impl_->seekable_value || !impl_->worker_running || state == State::Idle || state == State::Opening ||
        state == State::Error)
        return false;
    impl_->seek_request = std::clamp(seconds, 0.0, std::max(0.0, impl_->duration_value));
    impl_->seek_completed = false;
    impl_->seek_ok = false;
    impl_->condition.notify_all();
    impl_->condition.wait(
        lock, [&] { return impl_->seek_completed || !impl_->worker_running || impl_->stop_requested.load(); });
    return impl_->seek_completed && impl_->seek_ok;
}
bool Player::set_speed(double speed) {
    if (!can_set_speed() || !std::isfinite(speed) || speed < minimum_speed || speed > maximum_speed) return false;
    {
        std::lock_guard lock(impl_->control_mutex);
        if (impl_->playback_rate_value.exchange(speed) != speed) ++impl_->presentation_revision;
    }
    impl_->condition.notify_all();
    return true;
}
State Player::state() const noexcept { return impl_->state_value; }
double Player::position() const noexcept { return impl_->position_value; }
double Player::duration() const noexcept { return impl_->duration_value; }
bool Player::seekable() const noexcept { return impl_->seekable_value; }
bool Player::live() const noexcept { return impl_->live_value; }
bool Player::can_set_speed() const noexcept { return impl_->seekable_value && !impl_->live_value; }
double Player::speed() const noexcept { return impl_->playback_rate_value.load(); }
Frame Player::frame() const {
    std::lock_guard lock(impl_->frame_mutex);
    return impl_->latest;
}
void Player::set_volume(float volume) {
    impl_->volume_value = std::isnan(volume) ? 0.0F : std::clamp(volume, 0.0F, 1.0F);
    impl_->audio.set_volume(impl_->volume_value);
}
float Player::volume() const noexcept { return impl_->volume_value.load(); }
bool Player::audio_enabled() const noexcept { return impl_->audio_codec != nullptr; }
std::string Player::error() const {
    std::lock_guard lock(impl_->error_mutex);
    return impl_->error_message;
}

PlaybackStatistics Player::statistics() const noexcept {
    return {impl_->decoded_frames.load(), impl_->cpu_converted_frames.load(), impl_->gpu_prepared_frames.load(),
            static_cast<double>(impl_->decode_ns.load()) / 1000000.0,
            static_cast<double>(impl_->display_ns.load()) / 1000000.0};
}

} // namespace imvideo
