#include <imvideo/player.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::fprintf(stderr, "check failed: %s (%s:%d)\n", #expression, __FILE__, __LINE__); \
        return 1; \
    } \
} while (false)

int main() {
    const auto source = imvideo::Source::file(IMVIDEO_TEST_IMAGE);
    CHECK(source.uri == IMVIDEO_TEST_IMAGE);

    imvideo::Player player;
    player.set_volume(-1.0F);
    CHECK(player.volume() == 0.0F);
    player.set_volume(2.0F);
    CHECK(player.volume() == 1.0F);

    imvideo::Options options;
    CHECK(player.open(source, options));
    CHECK(!player.audio_enabled());

    imvideo::Frame frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!(frame = player.frame()) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    CHECK(frame);
    CHECK(frame.width() == 8);
    CHECK(frame.height() == 8);
    auto retained = frame;
    player.close();
    CHECK(player.state() == imvideo::State::Idle);
    CHECK(retained && retained.width() == 8 && retained.height() == 8);

    CHECK(!player.open(imvideo::Source::file("/path/that/does/not/exist")));
    CHECK(player.state() == imvideo::State::Error);
    CHECK(!player.error().empty());
    player.close();
    CHECK(player.state() == imvideo::State::Idle);
    return 0;
}
