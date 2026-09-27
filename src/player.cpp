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
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
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

class AudioPipeline {
public:
    AudioPipeline() = default;
    ~AudioPipeline() { close(); }

    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    bool open(AVCodecContext* codec, const std::shared_ptr<AudioSink>& sink, float volume, double rate) {
        close();
        if (!codec || !sink) return false;
        codec_ = codec;
        sink_ = sink;
        output_rate_ = codec->sample_rate > 0 ? static_cast<std::uint32_t>(codec->sample_rate) : 48000U;
        output_channels_ = 2;
        filtered_ = av_frame_alloc();
        if (!filtered_ || !configure_filter(rate)) {
            release_graph();
            sink_.reset();
            codec_ = nullptr;
            return false;
        }
        if (!sink_->open(static_cast<int>(output_rate_), static_cast<int>(output_channels_))) {
            release_graph();
            sink_.reset();
            codec_ = nullptr;
            return false;
        }
        opened_ = true;
        sink_->set_volume(volume);
        return true;
    }

    void close() {
        if (opened_ && sink_) sink_->close();
        opened_ = false;
        release_graph();
        sink_.reset();
        codec_ = nullptr;
        applied_rate_ = 1.0;
        wrote_samples_ = false;
    }

    [[nodiscard]] bool opened() const noexcept { return opened_; }
    [[nodiscard]] AudioSink* sink() const noexcept { return sink_.get(); }
    [[nodiscard]] double applied_rate() const noexcept { return applied_rate_; }
    [[nodiscard]] bool wrote_samples() const noexcept { return wrote_samples_; }

    bool reset_tempo(double rate) {
        if (!configure_filter(rate)) return false;
        reset_resampler();
        if (opened_ && sink_) sink_->flush();
        wrote_samples_ = false;
        return true;
    }

    bool consume(AVFrame* decoded) {
        if (!source_) return true;
        if (av_buffersrc_add_frame_flags(source_, decoded, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) return true;
        while (av_buffersink_get_frame(sink_filter_, filtered_) >= 0) {
            if (!write_frame(filtered_)) {
                av_frame_unref(filtered_);
                return false;
            }
            av_frame_unref(filtered_);
        }
        return true;
    }

    void set_volume(float volume) {
        if (opened_ && sink_) sink_->set_volume(volume);
    }

private:
    void release_graph() {
        swr_free(&resampler_);
        av_channel_layout_uninit(&resampler_input_layout_);
        resampler_input_format_ = AV_SAMPLE_FMT_NONE;
        resampler_input_rate_ = 0;
        avfilter_graph_free(&graph_);
        source_ = nullptr;
        sink_filter_ = nullptr;
        av_frame_free(&filtered_);
        output_rate_ = 0;
        output_channels_ = 0;
    }

    bool configure_filter(double rate) {
        avfilter_graph_free(&graph_);
        source_ = nullptr;
        sink_filter_ = nullptr;
        if (!codec_) return false;

        graph_ = avfilter_graph_alloc();
        if (!graph_) return false;

        char layout[256]{};
        if (av_channel_layout_describe(&codec_->ch_layout, layout, sizeof(layout)) < 0) return false;
        const char* sample_format = av_get_sample_fmt_name(codec_->sample_fmt);
        if (!sample_format) return false;

        char arguments[512]{};
        std::snprintf(arguments, sizeof(arguments), "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                      codec_->sample_rate, codec_->sample_rate, sample_format, layout);
        const AVFilter* source_filter = avfilter_get_by_name("abuffer");
        const AVFilter* sink_filter = avfilter_get_by_name("abuffersink");
        if (!source_filter || !sink_filter ||
            avfilter_graph_create_filter(&source_, source_filter, "audio_source", arguments, nullptr, graph_) < 0)
            return false;

        AVFilterContext* previous = source_;
        double remaining = rate;
        int index = 0;
        while (remaining < 0.5 || remaining > 2.0) {
            const double stage_rate = remaining < 0.5 ? 0.5 : 2.0;
            remaining /= stage_rate;
            if (!append_atempo(previous, stage_rate, index++)) return false;
        }
        if (!append_atempo(previous, remaining, index)) return false;

        if (avfilter_graph_create_filter(&sink_filter_, sink_filter, "audio_sink", nullptr, nullptr, graph_) < 0 ||
            avfilter_link(previous, 0, sink_filter_, 0) < 0 || avfilter_graph_config(graph_, nullptr) < 0)
            return false;
        applied_rate_ = rate;
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
        if (avfilter_graph_create_filter(&filter, atempo, name.c_str(), value.c_str(), nullptr, graph_) < 0 ||
            avfilter_link(previous, 0, filter, 0) < 0)
            return false;
        previous = filter;
        return true;
    }

    void reset_resampler() {
        swr_free(&resampler_);
        av_channel_layout_uninit(&resampler_input_layout_);
        resampler_input_format_ = AV_SAMPLE_FMT_NONE;
        resampler_input_rate_ = 0;
    }

    bool configure_resampler(const AVFrame* frame) {
        const auto input_format = static_cast<AVSampleFormat>(frame->format);
        const int input_rate = frame->sample_rate > 0 ? frame->sample_rate : codec_->sample_rate;
        if (input_format == AV_SAMPLE_FMT_NONE || input_rate <= 0 || frame->ch_layout.nb_channels <= 0) return false;
        if (resampler_ && resampler_input_format_ == input_format && resampler_input_rate_ == input_rate &&
            av_channel_layout_compare(&resampler_input_layout_, &frame->ch_layout) == 0)
            return true;

        reset_resampler();
        AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&resampler_, &output_layout, AV_SAMPLE_FMT_FLT, static_cast<int>(output_rate_),
                                &frame->ch_layout, input_format, input_rate, 0, nullptr) < 0 ||
            !resampler_ || swr_init(resampler_) < 0 ||
            av_channel_layout_copy(&resampler_input_layout_, &frame->ch_layout) < 0) {
            reset_resampler();
            return false;
        }
        resampler_input_format_ = input_format;
        resampler_input_rate_ = input_rate;
        return true;
    }

    bool write_frame(const AVFrame* frame) {
        if (!configure_resampler(frame) || !sink_) return false;
        const auto delay = swr_get_delay(resampler_, resampler_input_rate_);
        const int capacity = static_cast<int>(
            av_rescale_rnd(delay + frame->nb_samples, output_rate_, resampler_input_rate_, AV_ROUND_UP));
        if (capacity <= 0) return false;
        samples_.resize(static_cast<std::size_t>(capacity) * output_channels_);
        std::uint8_t* output[] = {reinterpret_cast<std::uint8_t*>(samples_.data())};
        const int produced = swr_convert(resampler_, output, capacity,
                                         const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
        if (produced > 0) {
            sink_->write(samples_.data(), static_cast<std::size_t>(produced));
            wrote_samples_ = true;
        }
        return produced >= 0;
    }

    AVCodecContext* codec_ = nullptr;
    std::shared_ptr<AudioSink> sink_;
    bool opened_ = false;
    SwrContext* resampler_ = nullptr;
    AVChannelLayout resampler_input_layout_{};
    AVSampleFormat resampler_input_format_ = AV_SAMPLE_FMT_NONE;
    int resampler_input_rate_ = 0;
    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* source_ = nullptr;
    AVFilterContext* sink_filter_ = nullptr;
    AVFrame* filtered_ = nullptr;
    std::uint32_t output_rate_ = 0;
    std::uint32_t output_channels_ = 0;
    double applied_rate_ = 1.0;
    bool wrote_samples_ = false;
    std::vector<float> samples_;
};

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
        loop = requested.loop;
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
            decoder_error.clear();
            if (audio_stream >= 0 && !open_decoder(audio_stream, audio_codec, false, decoder_error)) {
                avcodec_free_context(&audio_codec);
                audio_stream = -1;
            }
            if (audio_codec &&
                !audio.open(audio_codec, requested.audio_sink, volume_value, playback_rate_value.load())) {
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
        clear_held_packets();
        input_eof = false;
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
        AVPacket* packet = av_packet_alloc();
        AVFrame* decoded = av_frame_alloc();
        PresentationClock clock;
        clock.observed_revision = presentation_revision.load();
        if (!packet || !decoded) {
            set_error("cannot allocate decode buffers");
            set_state(State::Error);
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
                set_state(State::Error);
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
        if (loop && seekable_value) {
            apply_seek(0.0, clock, false);
            return;
        }
        set_state(State::Ended);
        if (audio.opened()) audio.sink()->pause(true);
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
            clear_held_packets();
            input_eof = false;
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
        const bool master_audio = audio.opened() && !live_value;
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
            const auto delay = std::chrono::duration<double>((seconds - clock.media_origin) / rate);
            const auto due = clock.origin + std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
            if (master_audio) {
                while (!blocked() && presentation_revision.load() == revision) {
                    lock.unlock();
                    const bool rate_ok = synchronize_audio_rate();
                    lock.lock();
                    if (!rate_ok) return false;
                    if (blocked() || presentation_revision.load() != revision) break;
                    const double played = audio_clock_media_origin +
                                          audio.sink()->clock_seconds() * std::max(audio.applied_rate(), minimum_speed);
                    if (played + 0.04 >= relative) break;
                    lock.unlock();
                    const bool wrote_audio = pump_audio();
                    lock.lock();
                    if (stop_requested || seek_request >= 0.0) return false;
                    if (!wrote_audio)
                        condition.wait_for(lock, std::chrono::milliseconds(10), [this, revision, &blocked] {
                            return blocked() || presentation_revision.load() != revision;
                        });
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
            // Transfer copies pixels, not the decoded frame's display/color metadata.
            if (av_frame_copy_props(software, decoded) < 0) {
                av_frame_free(&software);
                return;
            }
            source = software;
        }
        AVFrame* display = this->display.convert(source, format->streams[video_stream]);
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
            if (relative <= played + 0.5) return true;
            condition.wait_for(lock, std::chrono::milliseconds(10), blocked);
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
    std::deque<AVPacket*> held_packets;
    bool input_eof = false;
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
    impl_->volume_value = std::clamp(volume, 0.0F, 1.0F);
    impl_->audio.set_volume(impl_->volume_value);
}
float Player::volume() const noexcept { return impl_->volume_value.load(); }
bool Player::audio_enabled() const noexcept { return impl_->audio_codec != nullptr; }
std::string Player::error() const {
    std::lock_guard lock(impl_->error_mutex);
    return impl_->error_message;
}

} // namespace imvideo
