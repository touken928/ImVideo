#include "audio_pipeline.hpp"

#include <charconv>
#include <cstdio>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

namespace imvideo {

AudioPipeline::~AudioPipeline() { close(); }

bool AudioPipeline::open(AVCodecContext* codec, const std::shared_ptr<AudioSink>& sink, float volume, double rate) {
    close();
    if (!codec || !sink) return false;
    codec_ = codec;
    sink_ = sink;
    output_rate_ = codec->sample_rate > 0 ? static_cast<std::uint32_t>(codec->sample_rate) : 48000U;
    output_channels_ = 2;
    if (!configure_filter(rate)) {
        release_processing();
        sink_.reset();
        codec_ = nullptr;
        return false;
    }
    if (!sink_->open(static_cast<int>(output_rate_), static_cast<int>(output_channels_))) {
        release_processing();
        sink_.reset();
        codec_ = nullptr;
        return false;
    }
    opened_ = true;
    sink_->set_volume(volume);
    return true;
}

void AudioPipeline::close() {
    if (opened_ && sink_) sink_->close();
    opened_ = false;
    release_processing();
    sink_.reset();
    codec_ = nullptr;
    applied_rate_ = 1.0;
    wrote_samples_ = false;
    written_frames_ = 0;
}

bool AudioPipeline::reset_tempo(double rate) {
    if (!configure_filter(rate)) return false;
    reset_resampler();
    if (opened_ && sink_) sink_->flush();
    wrote_samples_ = false;
    written_frames_ = 0;
    return true;
}

bool AudioPipeline::consume(AVFrame* decoded) {
    if (!opened_ || !decoded) return false;
    if (!source_) return write_frame(decoded);
    if (av_buffersrc_add_frame_flags(source_, decoded, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) return false;
    return receive_filtered();
}

bool AudioPipeline::receive_filtered() {
    for (;;) {
        const int result = av_buffersink_get_frame(sink_filter_, filtered_.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
        if (result < 0) return false;
        const bool ok = write_frame(filtered_.get());
        av_frame_unref(filtered_.get());
        if (!ok) return false;
    }
}

bool AudioPipeline::drain() {
    if (!opened_) return true;
    if (source_) {
        const int sent = av_buffersrc_add_frame_flags(source_, nullptr, 0);
        if (sent < 0 && sent != AVERROR_EOF) return false;
        if (!receive_filtered()) return false;
    }
    if (resampler_) {
        samples_.resize(4096 * output_channels_);
        for (;;) {
            auto* destination = reinterpret_cast<std::uint8_t*>(samples_.data());
            const int produced = swr_convert(resampler_, &destination, 4096, nullptr, 0);
            if (produced < 0) return false;
            if (produced == 0) break;
            write_samples(samples_.data(), static_cast<std::size_t>(produced));
        }
    }
    sink_->drain();
    return true;
}

void AudioPipeline::set_volume(float volume) {
    if (opened_ && sink_) sink_->set_volume(volume);
}

void AudioPipeline::release_processing() {
    reset_resampler();
    avfilter_graph_free(&graph_);
    source_ = nullptr;
    sink_filter_ = nullptr;
    filtered_.reset();
    output_rate_ = 0;
    output_channels_ = 0;
}

bool AudioPipeline::configure_filter(double rate) {
    avfilter_graph_free(&graph_);
    source_ = nullptr;
    sink_filter_ = nullptr;
    if (!codec_) return false;

    // No graph or extra AVFrame references are needed at the normal rate.
    if (rate == 1.0) {
        applied_rate_ = rate;
        return true;
    }
    if (!filtered_) filtered_ = make_av_frame();
    if (!filtered_) return false;

    graph_ = avfilter_graph_alloc();
    if (!graph_) return false;
#if defined(__EMSCRIPTEN__)
    graph_->nb_threads = 1;
#endif

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
    // The normal-speed path needs no overlap/add tempo processing.
    if (remaining != 1.0 && !append_atempo(previous, remaining, index)) return false;

    if (avfilter_graph_create_filter(&sink_filter_, sink_filter, "audio_sink", nullptr, nullptr, graph_) < 0 ||
        avfilter_link(previous, 0, sink_filter_, 0) < 0 || avfilter_graph_config(graph_, nullptr) < 0)
        return false;
    applied_rate_ = rate;
    return true;
}

bool AudioPipeline::append_atempo(AVFilterContext*& previous, double rate, int index) {
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

void AudioPipeline::reset_resampler() {
    swr_free(&resampler_);
    av_channel_layout_uninit(&resampler_input_layout_);
    resampler_input_format_ = AV_SAMPLE_FMT_NONE;
    resampler_input_rate_ = 0;
}

bool AudioPipeline::configure_resampler(const AVFrame* frame) {
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

bool AudioPipeline::write_frame(const AVFrame* frame) {
    if (!frame || !sink_ || frame->nb_samples < 0) return false;
    const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (frame->format == AV_SAMPLE_FMT_FLT && frame->sample_rate == static_cast<int>(output_rate_) &&
        av_channel_layout_compare(&frame->ch_layout, &stereo) == 0 && !resampler_) {
        write_samples(reinterpret_cast<const float*>(frame->data[0]), static_cast<std::size_t>(frame->nb_samples));
        return true;
    }
    if (!configure_resampler(frame)) return false;
    const auto delay = swr_get_delay(resampler_, resampler_input_rate_);
    const auto required = av_rescale_rnd(delay + frame->nb_samples, output_rate_, resampler_input_rate_, AV_ROUND_UP);
    if (required < 0 || required > std::numeric_limits<int>::max()) return false;
    if (required == 0) return true;
    const int capacity = static_cast<int>(required);
    samples_.resize(static_cast<std::size_t>(capacity) * output_channels_);
    std::uint8_t* output[] = {reinterpret_cast<std::uint8_t*>(samples_.data())};
    const int produced = swr_convert(resampler_, output, capacity,
                                     const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
    if (produced > 0) { write_samples(samples_.data(), static_cast<std::size_t>(produced)); }
    return produced >= 0;
}

void AudioPipeline::write_samples(const float* samples, std::size_t frames) {
    if (frames == 0) return;
    sink_->write(samples, frames);
    written_frames_ += frames;
    wrote_samples_ = true;
}

} // namespace imvideo
