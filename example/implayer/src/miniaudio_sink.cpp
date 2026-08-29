#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "miniaudio_sink.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

struct MiniaudioSink::Impl {
    static void callback(ma_device* device, void* output, const void*, ma_uint32 frame_count) {
        auto& self = *static_cast<Impl*>(device->pUserData);
        auto* destination = static_cast<float*>(output);
        const auto wanted = static_cast<std::size_t>(frame_count) * self.channels;
        std::size_t copied = 0;
        {
            std::lock_guard lock(self.mutex);
            copied = std::min(wanted, self.buffer.size() - self.read_offset);
            if (copied != 0)
                std::copy_n(self.buffer.data() + self.read_offset, copied, destination);
            self.read_offset += copied;
            if (self.read_offset == self.buffer.size()) {
                self.buffer.clear();
                self.read_offset = 0;
            } else if (self.read_offset > 32768) {
                self.buffer.erase(self.buffer.begin(), self.buffer.begin() + static_cast<std::ptrdiff_t>(self.read_offset));
                self.read_offset = 0;
            }
        }
        std::fill(destination + copied, destination + wanted, 0.0F);
        const float gain = self.volume.load(std::memory_order_relaxed);
        for (std::size_t index = 0; index < copied; ++index) destination[index] *= gain;
        self.played_frames.fetch_add(frame_count, std::memory_order_relaxed);
    }

    ma_device device{};
    std::mutex mutex;
    std::vector<float> buffer;
    std::size_t read_offset = 0;
    std::size_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::atomic<float> volume{1.0F};
    std::atomic<std::uint64_t> played_frames{0};
    bool initialized = false;
};

MiniaudioSink::MiniaudioSink() : impl_(std::make_unique<Impl>()) {}
MiniaudioSink::~MiniaudioSink() { close(); }

bool MiniaudioSink::open(int sample_rate, int channels) {
    close();
    if (sample_rate <= 0 || channels <= 0) return false;
    impl_->sample_rate = static_cast<std::uint32_t>(sample_rate);
    impl_->channels = static_cast<std::size_t>(channels);
    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = static_cast<ma_uint32>(channels);
    config.sampleRate = static_cast<ma_uint32>(sample_rate);
    config.dataCallback = &Impl::callback;
    config.pUserData = impl_.get();
    if (ma_device_init(nullptr, &config, &impl_->device) != MA_SUCCESS) return false;
    impl_->initialized = true;
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        close();
        return false;
    }
    return true;
}

void MiniaudioSink::close() {
    if (impl_->initialized) {
        ma_device_uninit(&impl_->device);
        impl_->initialized = false;
    }
    flush();
}

void MiniaudioSink::write(const float* samples, std::size_t frames) {
    const auto count = frames * impl_->channels;
    std::lock_guard lock(impl_->mutex);
    impl_->buffer.insert(impl_->buffer.end(), samples, samples + count);
}

void MiniaudioSink::pause(bool paused) {
    if (!impl_->initialized) return;
    if (paused) ma_device_stop(&impl_->device);
    else ma_device_start(&impl_->device);
}

void MiniaudioSink::flush() {
    std::lock_guard lock(impl_->mutex);
    impl_->buffer.clear();
    impl_->read_offset = 0;
    impl_->played_frames = 0;
}

void MiniaudioSink::set_volume(float volume) {
    impl_->volume = std::clamp(volume, 0.0F, 1.0F);
}

double MiniaudioSink::clock_seconds() const noexcept {
    return impl_->sample_rate
               ? static_cast<double>(impl_->played_frames.load()) / impl_->sample_rate
               : 0.0;
}
