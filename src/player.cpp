#include <imvideo/player.hpp>

#include "frame_internal.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace imvideo {
namespace {

constexpr double minimum_speed = 0.25;
constexpr double maximum_speed = 4.0;
constexpr double discard_slack_seconds = 0.0005;

std::string ffmpeg_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

std::vector<AVHWDeviceType> hardware_candidates() {
#if defined(_WIN32)
    return {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2};
#elif defined(__APPLE__)
    return {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
#else
    return {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA};
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

int normalize_rotation(double angle) {
    if (!std::isfinite(angle)) return 0;
    int rounded = static_cast<int>(std::lround(angle)) % 360;
    if (rounded < 0) rounded += 360;
    if (rounded == 90 || rounded == 180 || rounded == 270) return rounded;
    return 0;
}

int rotation_from_matrix(const uint8_t* data, std::size_t size) {
    if (!data || size < 9 * sizeof(std::int32_t)) return 0;
    const auto angle = av_display_rotation_get(reinterpret_cast<const std::int32_t*>(data));
    // The matrix angle is counterclockwise. Metadata and display use clockwise degrees.
    return normalize_rotation(-angle);
}

void configure_scaler_colors(SwsContext* scaler, const AVFrame* frame) {
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    const bool rgb = descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    int source_range = rgb ? 1 : 0;
    if (frame->color_range == AVCOL_RANGE_JPEG) source_range = 1;
    if (frame->color_range == AVCOL_RANGE_MPEG) source_range = 0;
    int space = frame->colorspace;
    if (space == AVCOL_SPC_UNSPECIFIED || space == AVCOL_SPC_RGB)
        space = frame->height > 576 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    const int* source = sws_getCoefficients(space);
    const int* destination = sws_getCoefficients(SWS_CS_DEFAULT);
    sws_setColorspaceDetails(scaler, source, source_range, destination, 1, 0, 1 << 16, 1 << 16);
}

void rotate_rgba(const std::uint8_t* source, int source_width, int source_height, int source_stride,
                 std::uint8_t* destination, int destination_stride, int clockwise_degrees) {
    if (clockwise_degrees == 180) {
        for (int y = 0; y < source_height; ++y) {
            const auto* row = source + static_cast<std::size_t>(y) * source_stride;
            auto* output = destination + static_cast<std::size_t>(source_height - 1 - y) * destination_stride;
            for (int x = 0; x < source_width; ++x)
                std::memcpy(output + static_cast<std::size_t>(source_width - 1 - x) * 4,
                            row + static_cast<std::size_t>(x) * 4, 4);
        }
        return;
    }
    const bool right = clockwise_degrees == 90;
    for (int y = 0; y < source_height; ++y) {
        for (int x = 0; x < source_width; ++x) {
            const int output_x = right ? source_height - 1 - y : y;
            const int output_y = right ? x : source_width - 1 - x;
            std::memcpy(destination + static_cast<std::size_t>(output_y) * destination_stride +
                            static_cast<std::size_t>(output_x) * 4,
                        source + static_cast<std::size_t>(y) * source_stride + static_cast<std::size_t>(x) * 4, 4);
        }
    }
}

} // namespace

struct Player::Impl {
    ~Impl() { close(); }

    bool open(const Source& source, const Options& requested) {
        close();
        std::unique_lock life(lifecycle_mutex);
        if (source.uri().empty()) return fail_open("source URI is empty");
        {
            std::lock_guard lock(error_mutex);
            error_message.clear();
        }
        options = requested;
        audio_clock_media_origin = 0.0;
        hardware_selection.format = AV_PIX_FMT_NONE;
        audio_sink = options.audio_sink;
        stop_requested = false;
        open_thread = std::this_thread::get_id();
        open_active = true;
        state_value = State::Opening;

        format = avformat_alloc_context();
        if (!format) return fail_open("cannot allocate input context");
        format->interrupt_callback.callback = [](void* opaque) {
            const auto* self = static_cast<Impl*>(opaque);
            return self->stop_requested.load() || self->interrupt_seek.load() ? 1 : 0;
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
        if (!open_decoder(video_stream, video_codec, true)) return fail_open_after_decoder();

        if (audio_sink) {
            audio_stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, video_stream, nullptr, 0);
            if (audio_stream >= 0 && !open_decoder(audio_stream, audio_codec, false)) disable_audio();
            if (audio_codec && !open_audio_sink()) disable_audio();
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
        state_value = options.autoplay ? State::Playing : State::Paused;
        {
            std::lock_guard lock(control_mutex);
            worker_running = true;
            seek_completed = true;
            seek_request = -1.0;
        }
        try {
            worker = std::thread([this] { decode_loop(); });
        } catch (...) {
            worker_running = false;
            return fail_open("cannot start playback thread");
        }
        finish_open();
        return true;
    }

    bool fail_open(std::string message) {
        fail(std::move(message));
        finish_open();
        return false;
    }

    bool fail_open_after_decoder() {
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

    bool open_decoder(int stream_index, AVCodecContext*& context, bool hardware) {
        const AVCodecParameters* parameters = format->streams[stream_index]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) return fail("decoder is unavailable");
        context = avcodec_alloc_context3(codec);
        if (!context) return fail("cannot allocate decoder");
        int result = avcodec_parameters_to_context(context, parameters);
        if (result < 0) return fail("cannot configure decoder: " + ffmpeg_error(result));

        if (hardware) configure_hardware(codec, context);
        result = avcodec_open2(context, codec, nullptr);
        if (result < 0 && hardware_selection.format != AV_PIX_FMT_NONE) {
            av_buffer_unref(&context->hw_device_ctx);
            avcodec_free_context(&context);
            hardware_selection.format = AV_PIX_FMT_NONE;
            context = avcodec_alloc_context3(codec);
            if (!context) return fail("cannot allocate decoder");
            result = avcodec_parameters_to_context(context, parameters);
            if (result < 0) return fail("cannot configure decoder: " + ffmpeg_error(result));
            result = avcodec_open2(context, codec, nullptr);
        }
        if (result < 0) return fail("cannot open decoder: " + ffmpeg_error(result));
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

    bool open_audio_sink() {
        output_rate = audio_codec->sample_rate > 0 ? static_cast<std::uint32_t>(audio_codec->sample_rate) : 48000U;
        output_channels = 2;
        filtered_audio = av_frame_alloc();
        if (!filtered_audio || !configure_audio_filter(playback_rate_value)) return false;
        if (!audio_sink->open(static_cast<int>(output_rate), static_cast<int>(output_channels))) return false;
        audio_sink_opened = true;
        audio_sink->set_volume(volume_value);
        return true;
    }

    void disable_audio() {
        if (audio_sink_opened && audio_sink) audio_sink->close();
        audio_sink_opened = false;
        avfilter_graph_free(&audio_filter_graph);
        audio_filter_source = nullptr;
        audio_filter_sink = nullptr;
        av_frame_free(&filtered_audio);
        avcodec_free_context(&audio_codec);
        audio_stream = -1;
    }

    bool configure_audio_filter(double rate) {
        avfilter_graph_free(&audio_filter_graph);
        audio_filter_source = nullptr;
        audio_filter_sink = nullptr;

        audio_filter_graph = avfilter_graph_alloc();
        if (!audio_filter_graph) return false;

        char layout[256]{};
        if (av_channel_layout_describe(&audio_codec->ch_layout, layout, sizeof(layout)) < 0) return false;
        const char* sample_format = av_get_sample_fmt_name(audio_codec->sample_fmt);
        if (!sample_format) return false;

        char arguments[512]{};
        std::snprintf(arguments, sizeof(arguments), "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                      audio_codec->sample_rate, audio_codec->sample_rate, sample_format, layout);
        const AVFilter* source_filter = avfilter_get_by_name("abuffer");
        const AVFilter* sink_filter = avfilter_get_by_name("abuffersink");
        if (!source_filter || !sink_filter ||
            avfilter_graph_create_filter(&audio_filter_source, source_filter, "audio_source", arguments, nullptr,
                                         audio_filter_graph) < 0)
            return false;

        AVFilterContext* previous = audio_filter_source;
        double remaining = rate;
        int index = 0;
        while (remaining < 0.5 || remaining > 2.0) {
            const double stage_rate = remaining < 0.5 ? 0.5 : 2.0;
            remaining /= stage_rate;
            if (!append_atempo(previous, stage_rate, index++)) return false;
        }
        if (!append_atempo(previous, remaining, index)) return false;

        if (avfilter_graph_create_filter(&audio_filter_sink, sink_filter, "audio_sink", nullptr, nullptr,
                                         audio_filter_graph) < 0 ||
            avfilter_link(previous, 0, audio_filter_sink, 0) < 0 ||
            avfilter_graph_config(audio_filter_graph, nullptr) < 0)
            return false;
        applied_playback_rate = rate;
        return true;
    }

    bool append_atempo(AVFilterContext*& previous, double rate, int index) {
        AVFilterContext* filter = nullptr;
        const AVFilter* atempo = avfilter_get_by_name("atempo");
        if (!atempo) return false;
        const std::string name = "atempo_" + std::to_string(index);
        char rate_text[32]{};
        const auto formatted =
            std::to_chars(rate_text, rate_text + sizeof(rate_text) - 1, rate, std::chars_format::general, 6);
        if (formatted.ec != std::errc{}) return false;
        *formatted.ptr = '\0';
        const std::string value = std::string("tempo=") + rate_text;
        if (avfilter_graph_create_filter(&filter, atempo, name.c_str(), value.c_str(), nullptr, audio_filter_graph) <
                0 ||
            avfilter_link(previous, 0, filter, 0) < 0)
            return false;
        previous = filter;
        return true;
    }

    void close() {
        const bool wait_for_open = [this] {
            std::lock_guard lock(lifecycle_mutex);
            stop_requested = true;
            return open_active.load() && open_thread != std::this_thread::get_id();
        }();
        condition.notify_all();
        if (wait_for_open) {
            std::unique_lock done(open_mutex);
            open_finished.wait(done, [this] { return !open_active.load(); });
        }
        std::lock_guard life(lifecycle_mutex);
        stop_requested = true;
        condition.notify_all();
        if (worker.joinable()) worker.join();
        release_media();
        {
            std::lock_guard lock(error_mutex);
            error_message.clear();
        }
        position_value = duration_value = start_time_value = 0.0;
        audio_clock_media_origin = 0.0;
        seekable_value = live_value = false;
        playback_rate_value = 1.0;
        presentation_revision = 0;
        applied_playback_rate = 1.0;
        seek_request = -1.0;
        seek_completed = true;
        seek_ok = false;
        worker_running = false;
        discard_until = -1.0;
        state_value = State::Idle;
    }

    void release_media() {
        if (audio_sink_opened && audio_sink) audio_sink->close();
        swr_free(&resampler);
        av_channel_layout_uninit(&resampler_input_layout);
        resampler_input_format = AV_SAMPLE_FMT_NONE;
        resampler_input_rate = 0;
        avfilter_graph_free(&audio_filter_graph);
        audio_filter_source = nullptr;
        audio_filter_sink = nullptr;
        av_frame_free(&filtered_audio);
        avcodec_free_context(&audio_codec);
        avcodec_free_context(&video_codec);
        avformat_close_input(&format);
        {
            std::lock_guard lock(frame_mutex);
            latest = {};
        }
        video_stream = audio_stream = -1;
        audio_sink_opened = false;
        audio_sink.reset();
        clear_held_packets();
        input_eof = false;
        hardware_selection.format = AV_PIX_FMT_NONE;
        sws_freeContext(display_sws);
        display_sws = nullptr;
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
        AVPacket* packet = av_packet_alloc();
        AVFrame* decoded = av_frame_alloc();
        PresentationClock clock;
        clock.observed_revision = presentation_revision.load();
        if (!packet || !decoded) {
            set_error("cannot allocate decode buffers");
            state_value = State::Error;
            av_frame_free(&decoded);
            av_packet_free(&packet);
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

            const int read_result = read_next_packet(packet);
            if (read_result < 0) {
                av_packet_unref(packet);
                if (stop_requested) break;
                if (read_result == AVERROR_EXIT || read_result == AVERROR(EAGAIN)) continue;
                if (read_result == AVERROR_EOF) {
                    finish_input(decoded, clock);
                    continue;
                }
                set_error("input read failed: " + ffmpeg_error(read_result));
                state_value = State::Error;
                continue;
            }

            if (packet->stream_index == video_stream) {
                if (avcodec_send_packet(video_codec, packet) >= 0) receive_video(decoded, clock);
            } else if (audio_codec && packet->stream_index == audio_stream) {
                decode_audio(packet, decoded);
            }
            av_packet_unref(packet);
        }
        av_frame_free(&decoded);
        av_packet_free(&packet);
        finish_worker();
    }

    void finish_input(AVFrame* decoded, PresentationClock& clock) {
        drain_video(decoded, clock);
        drain_audio(decoded);
        if (stop_requested) return;
        if (seek_pending()) return;
        if (options.loop && seekable_value) {
            apply_seek(0.0, clock, false);
            return;
        }
        state_value = State::Ended;
        if (audio_sink_opened) audio_sink->pause(true);
    }

    void clear_held_packets() {
        for (AVPacket* packet : held_packets) av_packet_free(&packet);
        held_packets.clear();
    }

    int read_next_packet(AVPacket* packet) {
        if (!held_packets.empty()) {
            AVPacket* held = held_packets.front();
            held_packets.pop_front();
            av_packet_move_ref(packet, held);
            av_packet_free(&held);
            return 0;
        }
        if (input_eof) return AVERROR_EOF;
        const int result = av_read_frame(format, packet);
        if (result == AVERROR_EOF) input_eof = true;
        return result;
    }

    // Video presentation must not block the only thread that can fill the audio device.
    // Otherwise a clock that ignores silence never advances, and playback stalls.
    bool pump_audio() {
        if (held_packets.size() >= 64 || input_eof || stop_requested || !audio_codec) return false;
        AVPacket* packet = av_packet_alloc();
        AVFrame* decoded = av_frame_alloc();
        if (!packet || !decoded) {
            av_packet_free(&packet);
            av_frame_free(&decoded);
            return false;
        }
        bool wrote = false;
        while (held_packets.size() < 64 && !stop_requested && !input_eof) {
            const int result = av_read_frame(format, packet);
            if (result == AVERROR_EOF) {
                input_eof = true;
                break;
            }
            if (result < 0) break;
            if (packet->stream_index == audio_stream) {
                decode_audio(packet, decoded);
                av_packet_unref(packet);
                wrote = true;
                break;
            }
            AVPacket* held = av_packet_alloc();
            if (!held) {
                av_packet_unref(packet);
                break;
            }
            av_packet_move_ref(held, packet);
            held_packets.push_back(held);
        }
        av_frame_free(&decoded);
        av_packet_free(&packet);
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
        interrupt_seek = false;
        return true;
    }

    bool apply_seek(double relative, PresentationClock& clock, bool notify_waiter) {
        const double absolute = start_time_value + relative;
        const auto timestamp = static_cast<std::int64_t>(std::llround(absolute * AV_TIME_BASE));
        bool ok = format && av_seek_frame(format, -1, timestamp, AVSEEK_FLAG_BACKWARD) >= 0;
        if (ok) {
            if (video_codec) avcodec_flush_buffers(video_codec);
            if (audio_codec) avcodec_flush_buffers(audio_codec);
            if (audio_sink_opened) {
                if (!configure_audio_filter(playback_rate_value.load())) {
                    set_error("cannot reset audio tempo filter after seek");
                    state_value = State::Error;
                    ok = false;
                } else {
                    reset_resampler();
                    audio_sink->flush();
                }
            }
        }
        if (ok) {
            clear_held_packets();
            input_eof = false;
            position_value = relative;
            audio_clock_media_origin = relative;
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

    void receive_video(AVFrame* decoded, PresentationClock& clock) {
        while (avcodec_receive_frame(video_codec, decoded) >= 0) {
            const bool keep_going = present_video_frame(decoded, clock);
            av_frame_unref(decoded);
            if (!keep_going) break;
        }
    }

    void drain_video(AVFrame* decoded, PresentationClock& clock) {
        if (!video_codec || avcodec_send_packet(video_codec, nullptr) < 0) return;
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
        const auto revision = presentation_revision.load();
        if (revision != clock.observed_revision) {
            clock.observed_revision = revision;
            clock.origin_set = false;
        }
        if (!clock.origin_set) {
            clock.origin = std::chrono::steady_clock::now();
            clock.media_origin = seconds;
            clock.origin_set = true;
        }
        const double rate = std::max(playback_rate_value.load(), minimum_speed);
        const auto delay = std::chrono::duration<double>((seconds - clock.media_origin) / rate);
        const auto due = clock.origin + std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
        const bool master_audio = audio_sink_opened && audio_sink && !live_value;
        const double relative = relative_media_seconds(seconds);
        const auto blocked = [this] {
            return stop_requested.load() || state_value.load() != State::Playing || seek_request >= 0.0;
        };
        if (master_audio) {
            while (!blocked()) {
                const double played = audio_clock_media_origin + audio_sink->clock_seconds() * rate;
                if (played + 0.04 >= relative) break;
                lock.unlock();
                const bool wrote_audio = pump_audio();
                lock.lock();
                if (stop_requested || seek_request >= 0.0) return false;
                if (!wrote_audio) condition.wait_for(lock, std::chrono::milliseconds(10), blocked);
            }
        } else {
            condition.wait_until(lock, due, blocked);
        }
        if (stop_requested || seek_request >= 0.0) return false;
        if (state_value != State::Playing) {
            condition.wait(lock, [this] {
                return stop_requested.load() || state_value.load() == State::Playing || seek_request >= 0.0;
            });
            if (stop_requested || seek_request >= 0.0) return false;
            clock.observed_revision = presentation_revision.load();
            clock.origin = std::chrono::steady_clock::now();
            clock.media_origin = seconds;
            clock.origin_set = true;
        }
        return true;
    }

    int display_rotation_degrees(const AVFrame* frame) const {
        if (const AVFrameSideData* side = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX)) {
            const int rotation = rotation_from_matrix(side->data, side->size);
            if (rotation != 0) return rotation;
        }
        const AVStream* stream = format->streams[video_stream];
        if (stream->codecpar) {
            for (int index = 0; index < stream->codecpar->nb_coded_side_data; ++index) {
                const AVPacketSideData& data = stream->codecpar->coded_side_data[index];
                if (data.type != AV_PKT_DATA_DISPLAYMATRIX) continue;
                const int rotation = rotation_from_matrix(data.data, data.size);
                if (rotation != 0) return rotation;
            }
        }
        if (const AVDictionaryEntry* rotate = av_dict_get(stream->metadata, "rotate", nullptr, 0))
            return normalize_rotation(std::atoi(rotate->value));
        return 0;
    }

    AVFrame* make_display_frame(AVFrame* source) {
        if (!source || source->width <= 0 || source->height <= 0) return nullptr;
        int scaled_width = source->width;
        int scaled_height = source->height;
        AVRational sar = source->sample_aspect_ratio;
        if (sar.num <= 0 || sar.den <= 0) sar = format->streams[video_stream]->sample_aspect_ratio;
        if (sar.num > 0 && sar.den > 0 && sar.num != sar.den)
            scaled_width = std::max(1, static_cast<int>(av_rescale(source->width, sar.num, sar.den)));
        const int rotation = display_rotation_degrees(source);
        const bool swap_axes = rotation == 90 || rotation == 270;
        const int output_width = swap_axes ? scaled_height : scaled_width;
        const int output_height = swap_axes ? scaled_width : scaled_height;
        if (output_width > 16384 || output_height > 16384) return nullptr;
        if (scaled_width > (std::numeric_limits<int>::max() / 4) / std::max(scaled_height, 1)) return nullptr;

        display_sws =
            sws_getCachedContext(display_sws, source->width, source->height, static_cast<AVPixelFormat>(source->format),
                                 scaled_width, scaled_height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!display_sws) return nullptr;
        configure_scaler_colors(display_sws, source);

        AVFrame* output = av_frame_alloc();
        if (!output) return nullptr;
        output->format = AV_PIX_FMT_RGBA;
        output->width = output_width;
        output->height = output_height;
        if (av_frame_get_buffer(output, 32) < 0) {
            av_frame_free(&output);
            return nullptr;
        }
        if (rotation == 0) {
            std::uint8_t* destination[] = {output->data[0]};
            int strides[] = {output->linesize[0]};
            if (sws_scale(display_sws, source->data, source->linesize, 0, source->height, destination, strides) <= 0) {
                av_frame_free(&output);
                return nullptr;
            }
            return output;
        }

        std::vector<std::uint8_t> scaled(static_cast<std::size_t>(scaled_width) * scaled_height * 4);
        std::uint8_t* destination[] = {scaled.data()};
        int strides[] = {scaled_width * 4};
        if (sws_scale(display_sws, source->data, source->linesize, 0, source->height, destination, strides) <= 0) {
            av_frame_free(&output);
            return nullptr;
        }
        rotate_rgba(scaled.data(), scaled_width, scaled_height, scaled_width * 4, output->data[0], output->linesize[0],
                    rotation);
        return output;
    }

    void publish_frame(AVFrame* decoded) {
        AVFrame* software = nullptr;
        AVFrame* source = decoded;
        const auto pixel_format = static_cast<AVPixelFormat>(decoded->format);
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixel_format);
        if (descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0) {
            software = av_frame_alloc();
            if (!software || av_hwframe_transfer_data(software, decoded, 0) < 0) {
                av_frame_free(&software);
                return;
            }
            source = software;
        }
        AVFrame* display = make_display_frame(source);
        if (!display) display = av_frame_clone(source);
        av_frame_free(&software);
        if (!display) return;
        Frame next;
        next.impl_ = std::make_shared<Frame::Impl>(display, decoded->best_effort_timestamp);
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
        if (decoded->best_effort_timestamp != AV_NOPTS_VALUE && audio_stream >= 0 &&
            !wait_for_audio_lead(frame_seconds(decoded, format->streams[audio_stream])))
            return;
        const double requested_rate = playback_rate_value.load();
        if (requested_rate != applied_playback_rate) {
            if (!configure_audio_filter(requested_rate)) {
                set_error("cannot configure audio tempo filter");
                state_value = State::Error;
                return;
            }
            reset_resampler();
            audio_sink->flush();
        }
        if (!audio_filter_source) return;
        if (av_buffersrc_add_frame_flags(audio_filter_source, decoded, AV_BUFFERSRC_FLAG_KEEP_REF) >= 0) {
            while (av_buffersink_get_frame(audio_filter_sink, filtered_audio) >= 0) {
                if (!write_audio_frame(filtered_audio)) {
                    set_error("cannot resample filtered audio");
                    state_value = State::Error;
                    av_frame_unref(filtered_audio);
                    return;
                }
                av_frame_unref(filtered_audio);
            }
        }
    }

    bool wait_for_audio_lead(double absolute_seconds) {
        if (!audio_sink_opened || !audio_sink || live_value) return true;
        const double relative = relative_media_seconds(absolute_seconds);
        std::unique_lock lock(control_mutex);
        const auto blocked = [this] {
            return stop_requested.load() || state_value.load() != State::Playing || seek_request >= 0.0;
        };
        while (!blocked()) {
            const double rate = std::max(playback_rate_value.load(), minimum_speed);
            const double played = audio_clock_media_origin + audio_sink->clock_seconds() * rate;
            if (relative <= played + 0.5) return true;
            condition.wait_for(lock, std::chrono::milliseconds(10), blocked);
        }
        return !stop_requested && seek_request < 0.0 && state_value == State::Playing;
    }

    bool discard_audio(const AVFrame* frame) const {
        if (discard_until < 0.0 || audio_stream < 0 || frame->best_effort_timestamp == AV_NOPTS_VALUE) return false;
        const double seconds = frame->best_effort_timestamp * av_q2d(format->streams[audio_stream]->time_base);
        return seconds + discard_slack_seconds < discard_until;
    }

    void reset_resampler() {
        swr_free(&resampler);
        av_channel_layout_uninit(&resampler_input_layout);
        resampler_input_format = AV_SAMPLE_FMT_NONE;
        resampler_input_rate = 0;
    }

    bool configure_resampler(const AVFrame* frame) {
        const auto input_format = static_cast<AVSampleFormat>(frame->format);
        const int input_rate = frame->sample_rate > 0 ? frame->sample_rate : audio_codec->sample_rate;
        if (input_format == AV_SAMPLE_FMT_NONE || input_rate <= 0 || frame->ch_layout.nb_channels <= 0) return false;
        if (resampler && resampler_input_format == input_format && resampler_input_rate == input_rate &&
            av_channel_layout_compare(&resampler_input_layout, &frame->ch_layout) == 0)
            return true;

        reset_resampler();
        AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&resampler, &output_layout, AV_SAMPLE_FMT_FLT, static_cast<int>(output_rate),
                                &frame->ch_layout, input_format, input_rate, 0, nullptr) < 0 ||
            !resampler || swr_init(resampler) < 0 ||
            av_channel_layout_copy(&resampler_input_layout, &frame->ch_layout) < 0) {
            reset_resampler();
            return false;
        }
        resampler_input_format = input_format;
        resampler_input_rate = input_rate;
        return true;
    }

    bool write_audio_frame(const AVFrame* frame) {
        if (!configure_resampler(frame)) return false;
        const auto delay = swr_get_delay(resampler, resampler_input_rate);
        const int capacity =
            static_cast<int>(av_rescale_rnd(delay + frame->nb_samples, output_rate, resampler_input_rate, AV_ROUND_UP));
        if (capacity <= 0) return false;
        audio_samples.resize(static_cast<std::size_t>(capacity) * output_channels);
        std::uint8_t* output[] = {reinterpret_cast<std::uint8_t*>(audio_samples.data())};
        const int produced = swr_convert(resampler, output, capacity,
                                         const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
        if (produced > 0) audio_sink->write(audio_samples.data(), static_cast<std::size_t>(produced));
        return produced >= 0;
    }

    bool fail(std::string message) {
        set_error(std::move(message));
        release_media();
        state_value = State::Error;
        return false;
    }
    void set_error(std::string message) {
        std::lock_guard lock(error_mutex);
        error_message = std::move(message);
    }

    AVFormatContext* format = nullptr;
    AVCodecContext* video_codec = nullptr;
    AVCodecContext* audio_codec = nullptr;
    SwrContext* resampler = nullptr;
    AVChannelLayout resampler_input_layout{};
    AVSampleFormat resampler_input_format = AV_SAMPLE_FMT_NONE;
    int resampler_input_rate = 0;
    AVFilterGraph* audio_filter_graph = nullptr;
    AVFilterContext* audio_filter_source = nullptr;
    AVFilterContext* audio_filter_sink = nullptr;
    AVFrame* filtered_audio = nullptr;
    int video_stream = -1;
    int audio_stream = -1;
    HardwareSelection hardware_selection;
    std::shared_ptr<AudioSink> audio_sink;
    bool audio_sink_opened = false;
    std::uint32_t output_rate = 0;
    std::uint32_t output_channels = 0;
    std::vector<float> audio_samples;
    Options options;
    std::thread worker;
    std::condition_variable condition;
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
    double applied_playback_rate = 1.0;
    double seek_request = -1.0;
    bool seek_completed = true;
    bool seek_ok = false;
    bool worker_running = false;
    std::atomic<bool> interrupt_seek{false};
    double discard_until = -1.0;
    double audio_clock_media_origin = 0.0;
    std::deque<AVPacket*> held_packets;
    bool input_eof = false;
    SwsContext* display_sws = nullptr;
    std::mutex lifecycle_mutex;
    std::mutex open_mutex;
    std::condition_variable open_finished;
    std::atomic<bool> open_active{false};
    std::thread::id open_thread{};
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
        resume_audio = impl_->audio_sink_opened;
    }
    impl_->condition.notify_all();
    if (resume_audio) impl_->audio_sink->pause(false);
}
void Player::pause() {
    bool pause_audio = false;
    {
        std::lock_guard lock(impl_->control_mutex);
        if (impl_->state_value != State::Playing) return;
        impl_->state_value = State::Paused;
        pause_audio = impl_->audio_sink_opened;
    }
    impl_->condition.notify_all();
    if (pause_audio) impl_->audio_sink->pause(true);
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
        pause_audio = impl_->audio_sink_opened;
    }
    impl_->condition.notify_all();
    if (pause_audio) impl_->audio_sink->pause(true);
}
bool Player::seek(double seconds) {
    if (!std::isfinite(seconds)) return false;
    std::unique_lock lock(impl_->control_mutex);
    const auto state = impl_->state_value.load();
    if (!impl_->seekable_value || !impl_->worker_running || state == State::Idle || state == State::Opening ||
        state == State::Error)
        return false;
    impl_->seek_request = std::clamp(seconds, 0.0, std::max(0.0, impl_->duration_value));
    impl_->interrupt_seek = true;
    impl_->seek_completed = false;
    impl_->seek_ok = false;
    impl_->condition.notify_all();
    impl_->condition.wait(
        lock, [&] { return impl_->seek_completed || !impl_->worker_running || impl_->stop_requested.load(); });
    return impl_->seek_completed && impl_->seek_ok;
}
bool Player::set_speed(double speed) {
    if (!can_set_speed() || !std::isfinite(speed) || speed < minimum_speed || speed > maximum_speed) return false;
    if (impl_->playback_rate_value.exchange(speed) != speed) {
        ++impl_->presentation_revision;
        impl_->condition.notify_all();
    }
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
    impl_->volume_value = std::clamp(volume, 0.0F, 1.0F);
    if (impl_->audio_sink_opened) impl_->audio_sink->set_volume(impl_->volume_value);
}
float Player::volume() const noexcept { return impl_->volume_value.load(); }
bool Player::audio_enabled() const noexcept { return impl_->audio_codec != nullptr; }
std::string Player::error() const {
    std::lock_guard lock(impl_->error_mutex);
    return impl_->error_message;
}

} // namespace imvideo
