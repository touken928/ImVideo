// -----------------------------------------------------------------------
// audio_engine.cpp  —  miniaudio implementation
//
// This is the ONE translation unit that defines MINIAUDIO_IMPLEMENTATION.
// -----------------------------------------------------------------------
#include "audio_engine.hpp"

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <cstring>

namespace imvideo {

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init(SpscRingBuffer<float>* ring,
                       unsigned int sample_rate,
                       unsigned int channels,
                       float volume) {
    shutdown();

    if (!ring) { err_ = "null ring buffer"; return false; }
    ring_   = ring;
    sr_     = sample_rate;
    ch_     = channels;
    volume_ = volume;

    device_ = std::make_unique<ma_device>();

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format   = ma_format_f32;
    config.playback.channels = channels;
    config.sampleRate        = sample_rate;
    config.dataCallback      = data_callback_static;
    config.pUserData         = this;

    ma_result result = ma_device_init(nullptr, &config, device_.get());
    if (result != MA_SUCCESS) {
        err_ = "ma_device_init: ";
        err_ += ma_result_description(result);
        device_.reset();
        return false;
    }

    valid_ = true;
    started_.store(false, std::memory_order_release);
    return true;
}

void AudioEngine::shutdown() {
    if (device_) {
        if (ma_device_is_started(device_.get())) {
            ma_device_stop(device_.get());
        }
        ma_device_uninit(device_.get());
        device_.reset();
    }
    valid_ = false;
    ring_  = nullptr;
    started_.store(false, std::memory_order_release);
    frames_consumed_.store(0, std::memory_order_relaxed);
}

bool AudioEngine::start() {
    if (!valid_) return false;
    ma_result result = ma_device_start(device_.get());
    if (result != MA_SUCCESS) {
        err_ = "ma_device_start: ";
        err_ += ma_result_description(result);
        return false;
    }
    started_.store(true, std::memory_order_release);
    return true;
}

void AudioEngine::stop() {
    if (device_ && ma_device_is_started(device_.get())) {
        ma_device_stop(device_.get());
    }
    started_.store(false, std::memory_order_release);
}

void AudioEngine::set_volume(float v) {
    volume_ = std::clamp(v, 0.0f, 1.0f);
}

// static
void AudioEngine::data_callback_static(ma_device* pDevice, void* pOutput,
                                       const void*, unsigned int frameCount) {
    auto* self = static_cast<AudioEngine*>(pDevice->pUserData);
    if (!self || !self->ring_) return;

    float* out = static_cast<float*>(pOutput);
    size_t num_floats = static_cast<size_t>(frameCount) * self->ch_;

    // Read as many float samples as available
    size_t read = self->ring_->read(out, num_floats);

    // Apply volume
    if (self->volume_ != 1.0f) {
        for (size_t i = 0; i < read; ++i) out[i] *= self->volume_;
    }

    // Fill any remaining with silence
    if (read < num_floats) {
        std::memset(out + read, 0, (num_floats - read) * sizeof(float));
    }

    // Advance consumed frame counter (in audio timebase: frames = sample groups)
    self->frames_consumed_.fetch_add(read / self->ch_, std::memory_order_release);
}

} // namespace imvideo
