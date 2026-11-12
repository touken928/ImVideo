#pragma once

#include <imvideo/audio_sink.hpp>

#include "ffmpeg_raii.hpp"

#include <cstdint>
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

struct AVCodecContext;
struct AVFilterContext;
struct AVFilterGraph;
struct AVFrame;
struct SwrContext;

namespace imvideo {

// Decoder-thread audio conversion and tempo processing. The caller owns the
// device through AudioSink; no platform audio integration belongs here.
class AudioPipeline {
public:
    AudioPipeline() = default;
    ~AudioPipeline();
    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    bool open(AVCodecContext* codec, const std::shared_ptr<AudioSink>& sink, float volume, double rate);
    void close();
    bool reset_tempo(double rate);
    bool consume(AVFrame* decoded);
    bool drain();
    void set_volume(float volume);

    [[nodiscard]] bool opened() const noexcept { return opened_; }
    [[nodiscard]] AudioSink* sink() const noexcept { return sink_.get(); }
    [[nodiscard]] double applied_rate() const noexcept { return applied_rate_; }
    [[nodiscard]] bool wrote_samples() const noexcept { return wrote_samples_; }
    [[nodiscard]] double written_seconds() const noexcept {
        return output_rate_ ? static_cast<double>(written_frames_) / output_rate_ : 0.0;
    }

private:
    void release_processing();
    bool configure_filter(double rate);
    bool append_atempo(AVFilterContext*& previous, double rate, int index);
    bool receive_filtered();
    void reset_resampler();
    bool configure_resampler(const AVFrame* frame);
    bool write_frame(const AVFrame* frame);
    void write_samples(const float* samples, std::size_t frames);

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
    AvFramePtr filtered_;
    std::uint32_t output_rate_ = 0;
    std::uint32_t output_channels_ = 0;
    double applied_rate_ = 1.0;
    bool wrote_samples_ = false;
    std::uint64_t written_frames_ = 0;
    std::vector<float> samples_;
};

} // namespace imvideo
