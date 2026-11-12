#include <imvideo/player.hpp>
#include <imvideo/renderer.hpp>

#include <GLES3/gl3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

emscripten::ProxyingQueue main_queue;
pthread_t main_thread;
std::unique_ptr<imvideo::Renderer> renderer;
std::atomic<bool> context_ready{true};
std::atomic<unsigned> context_losses{0};
imvideo::Frame retained_gpu_frame;

void check(bool passed, const std::string& message) {
    if (!passed) throw std::runtime_error(message);
}

template <typename Predicate> void wait_until(Predicate predicate, const std::string& message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!predicate()) {
        check(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void on_main(const std::function<void()>& operation) {
    std::string failure;
    check(main_queue.proxySync(main_thread,
                               [&] {
                                   try {
                                       operation();
                                   } catch (const std::exception& error) { failure = error.what(); }
                               }),
          "main-thread proxy failed");
    check(failure.empty(), failure);
}

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

GLint integer(GLenum name) {
    GLint value = 0;
    glGetIntegerv(name, &value);
    return value;
}

Image read_frame(const imvideo::Frame& frame) {
    Image image;
    on_main([&] {
        // Nondefault unpack and sampler state must neither corrupt the image
        // nor leak out of either public Renderer upload path.
        GLuint buffer = 0, texture = 0, sampler = 0;
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, 4096, nullptr, GL_STATIC_DRAW);
        glGenTextures(1, &texture);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, texture);
        glGenSamplers(1, &sampler);
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glBindSampler(0, sampler);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 19);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
        glPixelStorei(0x9240, GL_TRUE); // UNPACK_FLIP_Y_WEBGL
        glPixelStorei(0x9241, GL_TRUE); // UNPACK_PREMULTIPLY_ALPHA_WEBGL
        glPixelStorei(0x9243, 0x9244);  // UNPACK_COLORSPACE_CONVERSION_WEBGL / BROWSER_DEFAULT_WEBGL
        const bool uploaded = renderer->update(frame);
        const bool restored = integer(GL_UNPACK_ALIGNMENT) == 8 && integer(GL_UNPACK_ROW_LENGTH) == 19 &&
                              integer(GL_UNPACK_SKIP_ROWS) == 1 && integer(GL_UNPACK_SKIP_PIXELS) == 2 &&
                              integer(GL_PIXEL_UNPACK_BUFFER_BINDING) == static_cast<GLint>(buffer) &&
                              integer(GL_ACTIVE_TEXTURE) == GL_TEXTURE3 &&
                              integer(GL_TEXTURE_BINDING_2D) == static_cast<GLint>(texture) &&
                              integer(0x9240) == GL_TRUE && integer(0x9241) == GL_TRUE && integer(0x9243) == 0x9244;
        glActiveTexture(GL_TEXTURE0);
        const bool sampler_restored = integer(GL_SAMPLER_BINDING) == static_cast<GLint>(sampler);
        glBindSampler(0, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(0x9240, GL_FALSE);
        glPixelStorei(0x9241, GL_FALSE);
        glPixelStorei(0x9243, 0x9244);
        glDeleteBuffers(1, &buffer);
        glDeleteTextures(1, &texture);
        glDeleteSamplers(1, &sampler);
        check(uploaded, "Renderer upload failed");
        check(restored && sampler_restored, "Renderer did not restore host GL state");
        check(renderer->texture() != 0, "missing RGBA output texture");
        image.width = renderer->width();
        image.height = renderer->height();
        image.pixels.resize(static_cast<std::size_t>(image.width) * image.height * 4);
        GLuint framebuffer = 0;
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               static_cast<GLuint>(renderer->texture()), 0);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glReadPixels(0, 0, image.width, image.height, GL_RGBA, GL_UNSIGNED_BYTE, image.pixels.data());
        const auto error = glGetError();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        check(complete && error == GL_NO_ERROR, "RGBA texture readback failed");
    });
    check(image.width == frame.width() && image.height == frame.height(), "display dimensions changed during upload");
    bool nonblack = false;
    for (std::size_t index = 0; index < image.pixels.size(); ++index)
        if (index % 4 != 3 && image.pixels[index] != 0) nonblack = true;
    check(nonblack, "empty RGB pixels");
    return image;
}

struct Capture {
    imvideo::Frame frame;
    Image image;
    imvideo::PlaybackStatistics statistics;
};

Capture capture(const std::string& file, bool gpu) {
    imvideo::Options options;
    options.gpu_color_conversion = gpu;
    options.collect_statistics = true;
    imvideo::Player player;
    check(player.open(imvideo::Source::file(file), options), "open failed: " + player.error());
    wait_until([&] { return static_cast<bool>(player.frame()); }, "first frame timed out: " + file);
    const auto frame = player.frame();
    const auto statistics = player.statistics();
    player.close();
    check(frame && frame == imvideo::Frame(frame), "frame lifetime/identity failed after close");
    return {frame, read_frame(frame), statistics};
}

class CountingAudioSink final : public imvideo::AudioSink {
public:
    bool open(int sample_rate, int channels) override {
        rate_ = sample_rate;
        samples_ = 0;
        consumed_ = 0;
        paused_ = false;
        nonzero_ = 0;
        drains_ = 0;
        ++opens_;
        return sample_rate > 0 && channels == 2;
    }
    void close() override {}
    void write(const float* samples, std::size_t frames) override {
        unsigned nonzero = 0;
        for (std::size_t index = 0; index < frames * 2; ++index)
            if (std::abs(samples[index]) > 0.00001F) ++nonzero;
        nonzero_ += nonzero;
        const auto written = samples_.fetch_add(frames) + frames;
        if (!paused_) consumed_ = written;
    }
    void pause(bool paused) override {
        paused_ = paused;
        if (!paused) consumed_ = samples_.load();
    }
    void flush() override { samples_ = consumed_ = 0; }
    void drain() override { ++drains_; }
    void set_volume(float volume) override { volume_ = volume; }
    double clock_seconds() const noexcept override {
        return rate_ > 0 ? static_cast<double>(consumed_.load()) / rate_ : 0.0;
    }

    std::atomic<std::uint64_t> samples_{0};
    std::atomic<std::uint64_t> consumed_{0};
    std::atomic<bool> paused_{false};
    std::atomic<unsigned> nonzero_{0}, drains_{0}, opens_{0};
    std::atomic<float> volume_{1.0F};
    int rate_ = 0;
};

void api_tests() {
    imvideo::Player player;
    check(player.state() == imvideo::State::Idle && !player.frame(), "incorrect idle defaults");
    check(!player.open(imvideo::Source::url("https://invalid.test/video")), "browser URL input must be rejected");
    check(!player.open(imvideo::Source::rtsp("rtsp://invalid.test/video")), "browser RTSP input must be rejected");
    check(!player.open(imvideo::Source::file("/media/missing.mkv")), "missing file was accepted");
    const auto error = player.error();
    player.close();
    check(!error.empty() && player.error().empty(), "error ownership/reset failed");
    const auto image = capture("/media/frame.xbm", true);
    check(image.image.width == 8 && image.image.height == 8, "XBM CPU fallback failed");
    check(image.statistics.cpu_converted_frames > 0, "XBM did not use CPU conversion");
}

void pixel_tests() {
    for (const auto* file : {"clip.mkv", "wide.mkv", "rotated.mkv"}) {
        const std::string path = std::string("/media/") + file;
        const auto cpu = capture(path, false);
        const auto gpu = capture(path, true);
        check(cpu.frame.pts() == gpu.frame.pts(), "reference frames have different PTS");
        check(cpu.frame != gpu.frame, "distinct decoded frames share identity");
        check(cpu.image.width == gpu.image.width && cpu.image.height == gpu.image.height, "GPU geometry mismatch");
        check(cpu.image.pixels.size() == gpu.image.pixels.size(), "GPU pixel count mismatch");
        int maximum = 0;
        for (std::size_t index = 0; index < cpu.image.pixels.size(); ++index)
            maximum = std::max(maximum, std::abs(cpu.image.pixels[index] - gpu.image.pixels[index]));
        check(maximum <= 6, "CPU/GPU color mismatch: " + std::to_string(maximum));
        if (std::string(file) == "wide.mkv") {
            check(gpu.image.width == 320 && gpu.image.height == 120, "SAR dimensions incorrect");
            check(gpu.statistics.cpu_converted_frames > 0, "SAR must retain CPU conversion");
        } else {
            check(gpu.statistics.gpu_prepared_frames > 0, "YUV fast path was not exercised");
            retained_gpu_frame = gpu.frame;
            if (std::string(file) == "rotated.mkv")
                check(gpu.image.width == 120 && gpu.image.height == 160, "rotation dimensions incorrect");
        }
    }
}

void audio_tests() {
    const auto sink = std::make_shared<CountingAudioSink>();
    imvideo::Options options;
    options.audio_sink = sink;
    options.fail_on_audio_error = true;
    options.autoplay = false;
    imvideo::Player player;
    check(player.open(imvideo::Source::file("/media/clip.mkv"), options), "audio open failed: " + player.error());
    check(player.audio_enabled() && player.seekable(), "audio/seek capabilities missing");
    player.play();
    wait_until([&] { return sink->nonzero_.load() > 0 && player.frame(); }, "no decoded PCM/video");
    player.pause();
    check(player.state() == imvideo::State::Paused, "pause failed");
    check(player.seek(0.5), "seek failed");
    check(player.set_speed(2.0), "tempo change failed");
    player.set_volume(0.25F);
    check(sink->volume_ == 0.25F, "volume was not forwarded");
    player.play();
    wait_until([&] { return player.state() == imvideo::State::Ended; }, "audio tail/tempo did not reach EOF");
    check(sink->samples_ > 0 && sink->drains_ > 0, "audio tail was not drained");
    player.close();
    check(!player.frame() && player.state() == imvideo::State::Idle, "close failed");
    const auto opens = sink->opens_.load();
    options.autoplay = true;
    check(player.open(imvideo::Source::file("/media/frame.xbm"), options), "reopen failed");
    wait_until([&] { return static_cast<bool>(player.frame()); }, "reopen frame timed out");
    check(!player.audio_enabled() && sink->opens_ == opens, "silent media opened an audio device");
}

void loop_tests() {
    imvideo::Options options;
    options.loop = true;
    imvideo::Player player;
    check(player.open(imvideo::Source::file("/media/wide.mkv"), options), "loop source failed to open");
    check(player.set_speed(4.0), "loop speed failed");
    wait_until([&] { return player.position() > 0.15; }, "loop did not advance");
    wait_until([&] { return player.position() < 0.1; }, "loop did not restart");
    check(player.state() == imvideo::State::Playing, "loop stopped playback");
}

EM_BOOL context_changed(int event, const void*, void*) {
    if (event == EMSCRIPTEN_EVENT_WEBGLCONTEXTLOST) {
        renderer->invalidate();
        context_ready = false;
        ++context_losses;
    } else {
        context_ready = true;
    }
    return EM_TRUE;
}

void context_tests() {
    check(static_cast<bool>(retained_gpu_frame), "missing retained GPU frame");
    const auto before = read_frame(retained_gpu_frame);
    const auto losses = context_losses.load();
    on_main([] {
        const int supported = EM_ASM_INT({
            const extension = Module.canvas.getContext('webgl2').getExtension('WEBGL_lose_context');
            if (!extension) return 0;
            extension.loseContext();
            setTimeout(extension.restoreContext.bind(extension), 100);
            return 1;
        });
        check(supported, "context-loss extension unavailable");
    });
    wait_until([&] { return context_losses > losses && context_ready; }, "context did not restore");
    const auto after = read_frame(retained_gpu_frame);
    check(before.pixels == after.pixels, "restored context changed the image");
}

EM_JS(void, report, (const char* name, int passed, const char* message), {
    window.wasmTestResults.push({name : UTF8ToString(name), passed : !!passed, message : UTF8ToString(message)});
});

void run_case(const char* name, const std::function<void()>& test) {
    std::string error;
    try {
        test();
    } catch (const std::exception& failure) { error = failure.what(); }
    std::printf("%s: %s %s\n", error.empty() ? "PASS" : "FAIL", name, error.c_str());
    on_main([&] { report(name, error.empty(), error.c_str()); });
}

} // namespace

int main() {
    try {
        check(EM_ASM_INT({ return crossOriginIsolated && !!globalThis.SharedArrayBuffer; }),
              "cross-origin isolation missing");
        check(emscripten_has_threading_support(), "pthreads unavailable");
        EmscriptenWebGLContextAttributes attributes;
        emscripten_webgl_init_context_attributes(&attributes);
        attributes.majorVersion = 2;
        attributes.antialias = false;
        const auto context = emscripten_webgl_create_context("#canvas", &attributes);
        check(context > 0, "WebGL 2 unavailable");
        check(emscripten_webgl_make_context_current(context) == EMSCRIPTEN_RESULT_SUCCESS, "current context failed");
        renderer = std::make_unique<imvideo::Renderer>();
        main_thread = pthread_self();
        emscripten_set_webglcontextlost_callback("#canvas", nullptr, true, context_changed);
        emscripten_set_webglcontextrestored_callback("#canvas", nullptr, true, context_changed);
        std::thread([] {
            run_case("API, virtual files and transport rejection", api_tests);
            run_case("CPU/GPU pixels, GL state, SAR and rotation", pixel_tests);
            run_case("Audio PCM, controls, tempo, drain and reopen", audio_tests);
            run_case("Looping playback", loop_tests);
            run_case("WebGL context loss and recovery", context_tests);
            on_main([] { EM_ASM({ window.wasmTestsFinished = true; }); });
        }).detach();
        emscripten_exit_with_live_runtime();
    } catch (const std::exception& error) {
        EM_ASM({ window.wasmTestFailure = UTF8ToString($0); }, error.what());
    }
}
