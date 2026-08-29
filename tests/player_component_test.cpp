#include <imvideo/player.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <thread>

namespace {

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return predicate();
}

class RecordingAudioSink final : public imvideo::AudioSink {
public:
    bool open(int, int) override {
        ++open_calls;
        return true;
    }
    void close() override { ++close_calls; }
    void write(const float*, std::size_t) override { ++write_calls; }
    void pause(bool) override { ++pause_calls; }
    void flush() override { ++flush_calls; }
    void set_volume(float) override { ++volume_calls; }
    [[nodiscard]] double clock_seconds() const noexcept override { return 0.0; }

    std::atomic<int> open_calls{0};
    std::atomic<int> close_calls{0};
    std::atomic<int> write_calls{0};
    std::atomic<int> pause_calls{0};
    std::atomic<int> flush_calls{0};
    std::atomic<int> volume_calls{0};
};

} // namespace

TEST_CASE("Player decodes a local image and retains the frame", "[player][decode]") {
    imvideo::Player player;
    imvideo::Options options;
    options.autoplay = false;

    REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_IMAGE), options));
    REQUIRE(player.state() == imvideo::State::Paused);
    REQUIRE_FALSE(player.audio_enabled());
    REQUIRE_FALSE(player.live());
    REQUIRE_FALSE(player.frame());

    player.play();
    REQUIRE(wait_until([&player] { return static_cast<bool>(player.frame()); }));

    const auto retained = player.frame();
    REQUIRE(retained.width() == 8);
    REQUIRE(retained.height() == 8);

    player.close();
    REQUIRE(player.state() == imvideo::State::Idle);
    REQUIRE_FALSE(player.frame());
    REQUIRE(retained);
    REQUIRE(retained.width() == 8);
    REQUIRE(retained.height() == 8);
}

TEST_CASE("A video-only source does not open an audio sink", "[player][audio]") {
    auto sink = std::make_shared<RecordingAudioSink>();
    imvideo::Options options;
    options.audio_sink = sink;

    imvideo::Player player;
    REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_IMAGE), options));
    REQUIRE_FALSE(player.audio_enabled());

    player.set_volume(0.5F);
    player.pause();
    player.close();

    REQUIRE(sink->open_calls == 0);
    REQUIRE(sink->close_calls == 0);
    REQUIRE(sink->write_calls == 0);
    REQUIRE(sink->pause_calls == 0);
    REQUIRE(sink->flush_calls == 0);
    REQUIRE(sink->volume_calls == 0);
}

TEST_CASE("Playback speed follows source capabilities and limits", "[player][speed]") {
    imvideo::Options options;
    options.autoplay = false;
    imvideo::Player player;

    REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_IMAGE), options));
    REQUIRE_FALSE(player.set_speed(0.1));
    REQUIRE_FALSE(player.set_speed(8.0));
    REQUIRE_FALSE(player.set_speed(std::numeric_limits<double>::quiet_NaN()));

    const bool supported = player.can_set_speed();
    REQUIRE(player.set_speed(2.0) == supported);
    REQUIRE(player.speed() == (supported ? 2.0 : 1.0));

    player.close();
    REQUIRE(player.speed() == 1.0);
    REQUIRE_FALSE(player.can_set_speed());
}

TEST_CASE("Opening an empty source reports an error and close recovers", "[player][error]") {
    imvideo::Player player;

    REQUIRE_FALSE(player.open(imvideo::Source::file("")));
    REQUIRE(player.state() == imvideo::State::Error);
    REQUIRE_FALSE(player.error().empty());

    player.close();
    REQUIRE(player.state() == imvideo::State::Idle);
}

TEST_CASE("Opening a missing file reports an error", "[player][error]") {
    imvideo::Player player;

    REQUIRE_FALSE(player.open(imvideo::Source::file("imvideo-file-that-does-not-exist.mp4")));
    REQUIRE(player.state() == imvideo::State::Error);
    REQUIRE_FALSE(player.error().empty());
}

TEST_CASE("Player can reopen a source after closing", "[player][lifecycle]") {
    imvideo::Player player;

    REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_IMAGE)));
    REQUIRE(wait_until([&player] { return static_cast<bool>(player.frame()); }));
    player.close();

    REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_IMAGE)));
    REQUIRE(wait_until([&player] { return static_cast<bool>(player.frame()); }));
    player.close();
    REQUIRE(player.state() == imvideo::State::Idle);
}
