#pragma once

#include <imvideo/audio_sink.hpp>

#include <memory>

class MiniaudioSink final : public imvideo::AudioSink {
public:
    MiniaudioSink();
    ~MiniaudioSink() override;

    bool open(int sample_rate, int channels) override;
    void close() override;
    void write(const float* samples, std::size_t frames) override;
    void pause(bool paused) override;
    void flush() override;
    void set_volume(float volume) override;
    [[nodiscard]] double clock_seconds() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
