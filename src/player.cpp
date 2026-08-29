#include <imvideo/player.hpp>

#include "frame_internal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace imvideo {
namespace {

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

struct HardwareSelection { AVPixelFormat format = AV_PIX_FMT_NONE; };

AVPixelFormat choose_hardware_format(AVCodecContext* context, const AVPixelFormat* formats) {
    const auto wanted = static_cast<HardwareSelection*>(context->opaque)->format;
    for (const auto* current = formats; *current != AV_PIX_FMT_NONE; ++current) {
        if (*current == wanted) return *current;
    }
    return formats[0];
}

} // namespace

struct Player::Impl {
    ~Impl() { close(); }

    bool open(const Source& source, const Options& requested) {
        close();
        if (source.uri.empty()) return fail("source URI is empty");
        {
            std::lock_guard lock(error_mutex);
            error_message.clear();
        }
        options = requested;
        state_value = State::Opening;
        stop_requested = false;
        volume_value = 1.0F;
        audio_sink = options.audio_sink;

        format = avformat_alloc_context();
        if (!format) return fail("cannot allocate input context");
        format->interrupt_callback.callback = [](void* opaque) {
            return static_cast<Impl*>(opaque)->stop_requested.load() ? 1 : 0;
        };
        format->interrupt_callback.opaque = this;
        AVDictionary* dictionary = nullptr;
        if (source.uri.rfind("rtsp://", 0) == 0) av_dict_set(&dictionary, "rtsp_transport", "tcp", 0);
        int result = avformat_open_input(&format, source.uri.c_str(), nullptr, &dictionary);
        av_dict_free(&dictionary);
        if (result < 0) return fail("cannot open input: " + ffmpeg_error(result));
        if ((result = avformat_find_stream_info(format, nullptr)) < 0)
            return fail("cannot read stream information: " + ffmpeg_error(result));

        video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_stream < 0) return fail("input has no decodable video stream");
        if (!open_decoder(video_stream, video_codec, true)) return false;

        if (audio_sink) {
            audio_stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, video_stream, nullptr, 0);
            if (audio_stream >= 0 && !open_decoder(audio_stream, audio_codec, false)) {
                // A broken or unsupported audio stream must not prevent video playback.
                avcodec_free_context(&audio_codec);
                audio_stream = -1;
            }
            if (audio_codec && !open_audio_sink()) {
                avcodec_free_context(&audio_codec);
                audio_stream = -1;
            }
        }

        if (format->duration != AV_NOPTS_VALUE)
            duration_value = static_cast<double>(format->duration) / AV_TIME_BASE;
        else
            duration_value = 0.0;
        live_value = duration_value <= 0.0 && (source.uri.rfind("rtsp://", 0) == 0 ||
                                               source.uri.rfind("http://", 0) == 0 ||
                                               source.uri.rfind("https://", 0) == 0);
        seekable_value = !live_value && (format->pb == nullptr || (format->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0);
        state_value = options.autoplay ? State::Playing : State::Paused;
        worker = std::thread([this] { decode_loop(); });
        return true;
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
            avcodec_parameters_to_context(context, parameters);
            result = avcodec_open2(context, codec, nullptr);
        }
        if (result < 0) return fail("cannot open decoder: " + ffmpeg_error(result));
        return true;
    }

    void configure_hardware(const AVCodec* codec, AVCodecContext* context) {
        for (const auto type : hardware_candidates()) {
            for (int i = 0;; ++i) {
                const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
                if (!config) break;
                if (config->device_type != type || !(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;
                AVBufferRef* device = nullptr;
                if (av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) < 0) continue;
                hardware_selection.format = config->pix_fmt;
                context->opaque = &hardware_selection;
                context->get_format = choose_hardware_format;
                context->hw_device_ctx = av_buffer_ref(device);
                av_buffer_unref(&device);
                return;
            }
        }
    }

    bool open_audio_sink() {
        output_rate = audio_codec->sample_rate > 0 ? static_cast<std::uint32_t>(audio_codec->sample_rate) : 48000U;
        output_channels = 2;
        AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
        int result = swr_alloc_set_opts2(&resampler, &output_layout, AV_SAMPLE_FMT_FLT,
                                         static_cast<int>(output_rate), &audio_codec->ch_layout,
                                         audio_codec->sample_fmt, audio_codec->sample_rate, 0, nullptr);
        if (result < 0 || swr_init(resampler) < 0) return false;
        if (!audio_sink->open(static_cast<int>(output_rate), static_cast<int>(output_channels))) return false;
        audio_sink_opened = true;
        audio_sink->set_volume(volume_value);
        return true;
    }

    void close() {
        stop_requested = true;
        condition.notify_all();
        if (worker.joinable()) worker.join();
        if (audio_sink_opened) audio_sink->close();
        swr_free(&resampler);
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
        position_value = duration_value = 0.0;
        seekable_value = live_value = false;
        state_value = State::Idle;
    }

    void decode_loop() {
        AVPacket* packet = av_packet_alloc();
        AVFrame* decoded = av_frame_alloc();
        auto origin = std::chrono::steady_clock::now();
        double media_origin = 0.0;
        bool origin_set = false;

        while (!stop_requested) {
            {
                std::unique_lock lock(control_mutex);
                condition.wait(lock, [this] { return stop_requested || state_value == State::Playing || seek_request >= 0.0; });
                if (stop_requested) break;
                if (seek_request >= 0.0) {
                    const double target = std::exchange(seek_request, -1.0);
                    const auto timestamp = static_cast<std::int64_t>(target * AV_TIME_BASE);
                    if (av_seek_frame(format, -1, timestamp, AVSEEK_FLAG_BACKWARD) >= 0) {
                        avcodec_flush_buffers(video_codec);
                        if (audio_codec) avcodec_flush_buffers(audio_codec);
                        if (audio_sink_opened) audio_sink->flush();
                        position_value = target;
                        origin_set = false;
                        std::lock_guard frame_lock(frame_mutex);
                        latest = {};
                    }
                }
                if (state_value != State::Playing) continue;
            }

            const int read_result = av_read_frame(format, packet);
            if (read_result < 0) {
                if (options.loop && seekable_value) {
                    std::lock_guard lock(control_mutex);
                    seek_request = 0.0;
                    continue;
                }
                if (read_result == AVERROR_EOF) {
                    state_value = State::Ended;
                    continue;
                }
                set_error("input read failed: " + ffmpeg_error(read_result));
                state_value = State::Error;
                break;
            }

            if (packet->stream_index == video_stream) {
                if (avcodec_send_packet(video_codec, packet) >= 0) {
                    while (avcodec_receive_frame(video_codec, decoded) >= 0) {
                        const double seconds = frame_seconds(decoded, format->streams[video_stream]);
                        if (!live_value) {
                            if (!origin_set) {
                                origin = std::chrono::steady_clock::now();
                                media_origin = seconds;
                                origin_set = true;
                            }
                            const auto due = origin + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                          std::chrono::duration<double>(seconds - media_origin));
                            while (!stop_requested && state_value == State::Playing && std::chrono::steady_clock::now() < due)
                                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        }
                        AVFrame* clone = av_frame_clone(decoded);
                        if (clone) {
                            Frame next;
                            next.impl_ = std::make_shared<Frame::Impl>(clone, decoded->best_effort_timestamp);
                            std::lock_guard lock(frame_mutex);
                            latest = std::move(next);
                        }
                        position_value = seconds;
                        av_frame_unref(decoded);
                    }
                }
            } else if (audio_codec && packet->stream_index == audio_stream) {
                decode_audio(packet, decoded);
            }
            av_packet_unref(packet);
        }
        av_frame_free(&decoded);
        av_packet_free(&packet);
    }

    static double frame_seconds(const AVFrame* frame, const AVStream* stream) {
        if (frame->best_effort_timestamp == AV_NOPTS_VALUE) return 0.0;
        return frame->best_effort_timestamp * av_q2d(stream->time_base);
    }

    void decode_audio(AVPacket* packet, AVFrame* decoded) {
        if (avcodec_send_packet(audio_codec, packet) < 0) return;
        while (avcodec_receive_frame(audio_codec, decoded) >= 0) {
            const auto delay = swr_get_delay(resampler, audio_codec->sample_rate);
            const int capacity = static_cast<int>(av_rescale_rnd(delay + decoded->nb_samples, output_rate,
                                                                  audio_codec->sample_rate, AV_ROUND_UP));
            audio_samples.resize(static_cast<std::size_t>(capacity) * output_channels);
            std::uint8_t* output[] = {reinterpret_cast<std::uint8_t*>(audio_samples.data())};
            const int produced = swr_convert(resampler, output, capacity,
                                             const_cast<const std::uint8_t**>(decoded->extended_data), decoded->nb_samples);
            if (produced > 0) audio_sink->write(audio_samples.data(), static_cast<std::size_t>(produced));
            av_frame_unref(decoded);
        }
    }

    bool fail(std::string message) {
        set_error(std::move(message));
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
    bool seekable_value = false;
    bool live_value = false;
    std::atomic<float> volume_value{1.0F};
    double seek_request = -1.0;
};

Player::Player() : impl_(std::make_unique<Impl>()) {}
Player::~Player() = default;
Player::Player(Player&&) noexcept = default;
Player& Player::operator=(Player&&) noexcept = default;
bool Player::open(const Source& source, const Options& options) { return impl_->open(source, options); }
void Player::close() { impl_->close(); }
void Player::play() {
    if (impl_->state_value == State::Paused || impl_->state_value == State::Ended) {
        if (impl_->state_value == State::Ended && impl_->seekable_value) seek(0.0);
        impl_->state_value = State::Playing;
        if (impl_->audio_sink_opened) impl_->audio_sink->pause(false);
        impl_->condition.notify_all();
    }
}
void Player::pause() {
    if (impl_->state_value == State::Playing) {
        impl_->state_value = State::Paused;
        if (impl_->audio_sink_opened) impl_->audio_sink->pause(true);
    }
}
void Player::stop() {
    if (impl_->seekable_value) seek(0.0);
    impl_->state_value = State::Paused;
    if (impl_->audio_sink_opened) impl_->audio_sink->pause(true);
}
bool Player::seek(double seconds) {
    if (!impl_->seekable_value || !std::isfinite(seconds)) return false;
    std::lock_guard lock(impl_->control_mutex);
    impl_->seek_request = std::clamp(seconds, 0.0, impl_->duration_value);
    impl_->condition.notify_all();
    return true;
}
State Player::state() const noexcept { return impl_->state_value; }
double Player::position() const noexcept { return impl_->position_value; }
double Player::duration() const noexcept { return impl_->duration_value; }
bool Player::seekable() const noexcept { return impl_->seekable_value; }
bool Player::live() const noexcept { return impl_->live_value; }
Frame Player::frame() const { std::lock_guard lock(impl_->frame_mutex); return impl_->latest; }
void Player::set_volume(float volume) {
    impl_->volume_value = std::clamp(volume, 0.0F, 1.0F);
    if (impl_->audio_sink_opened) impl_->audio_sink->set_volume(impl_->volume_value);
}
float Player::volume() const noexcept { return impl_->volume_value.load(); }
bool Player::audio_enabled() const noexcept { return impl_->audio_codec != nullptr; }
std::string_view Player::error() const noexcept { return impl_->error_message; }

} // namespace imvideo
