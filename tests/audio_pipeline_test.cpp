#include "audio_pipeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

using CodecPtr = std::unique_ptr<AVCodecContext, imvideo::AvDeleter<AVCodecContext, avcodec_free_context>>;

class CollectingAudioSink final : public imvideo::AudioSink {
public:
    bool open(int rate, int channels) override { return rate == 48000 && channels == 2; }
    void close() override {}
    void write(const float* samples, std::size_t frames) override {
        samples_.insert(samples_.end(), samples, samples + frames * 2);
    }
    void pause(bool) override {}
    void flush() override { samples_.clear(); }
    void drain() override { ++drains_; }
    void set_volume(float) override {}
    double clock_seconds() const noexcept override { return static_cast<double>(samples_.size() / 2) / 48000; }

    std::vector<float> samples_;
    int drains_ = 0;
};

CodecPtr audio_codec() {
    CodecPtr codec{avcodec_alloc_context3(nullptr)};
    REQUIRE(codec);
    codec->sample_rate = 48000;
    codec->sample_fmt = AV_SAMPLE_FMT_FLT;
    codec->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    return codec;
}

imvideo::AvFramePtr audio_frame(AVSampleFormat format, int rate, int samples) {
    auto frame = imvideo::make_av_frame();
    REQUIRE(frame);
    frame->format = format;
    frame->sample_rate = rate;
    frame->nb_samples = samples;
    frame->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    REQUIRE(av_frame_get_buffer(frame.get(), 0) == 0);
    return frame;
}

} // namespace

TEST_CASE("Normal-rate audio preserves packed and planar stereo samples", "[audio][conversion]") {
    const auto format = GENERATE(AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP);
    auto codec = audio_codec();
    auto sink = std::make_shared<CollectingAudioSink>();
    imvideo::AudioPipeline pipeline;
    REQUIRE(pipeline.open(codec.get(), sink, 1.0F, 1.0));
    auto frame = audio_frame(format, 48000, 1024);
    std::vector<float> expected;
    for (int index = 0; index < frame->nb_samples; ++index) {
        for (int channel = 0; channel < 2; ++channel) {
            const float sample = static_cast<float>(std::sin(index * 0.01 + channel));
            expected.push_back(sample);
            const int plane = format == AV_SAMPLE_FMT_FLTP ? channel : 0;
            const int offset = format == AV_SAMPLE_FMT_FLTP ? index : index * 2 + channel;
            reinterpret_cast<float*>(frame->data[plane])[offset] = sample;
        }
    }
    REQUIRE(pipeline.consume(frame.get()));
    REQUIRE(pipeline.drain());
    REQUIRE(sink->samples_ == expected);
    REQUIRE(sink->drains_ == 1);
    REQUIRE_THAT(pipeline.written_seconds(), Catch::Matchers::WithinAbs(1024.0 / 48000, 1e-12));
}

TEST_CASE("Normal-rate audio drains resampling tails without a tempo graph", "[audio][drain]") {
    auto codec = audio_codec();
    auto sink = std::make_shared<CollectingAudioSink>();
    imvideo::AudioPipeline pipeline;
    REQUIRE(pipeline.open(codec.get(), sink, 1.0F, 1.0));
    auto frame = audio_frame(AV_SAMPLE_FMT_FLT, 44100, 4410);
    auto* samples = reinterpret_cast<float*>(frame->data[0]);
    for (int index = 0; index < frame->nb_samples * 2; ++index) samples[index] = 0.25F;
    REQUIRE(pipeline.consume(frame.get()));
    const auto before_drain = sink->samples_.size();
    REQUIRE(pipeline.drain());
    REQUIRE(sink->samples_.size() > before_drain);
    REQUIRE(sink->samples_.size() == 4800 * 2);
    REQUIRE(sink->drains_ == 1);
    REQUIRE_THAT(pipeline.written_seconds(), Catch::Matchers::WithinAbs(0.1, 1e-12));
}
