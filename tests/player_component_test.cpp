#include <imvideo/player.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return predicate();
}

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
inline void close_socket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
inline void close_socket(Socket socket) { ::close(socket); }
#endif

class StallServer {
public:
    StallServer() {
#if defined(_WIN32)
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        listen_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
        listen_socket = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
        if (listen_socket == invalid_socket) return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listen_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return;
        socklen_t length = sizeof(address);
        if (::getsockname(listen_socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) return;
        port_ = ntohs(address.sin_port);
        if (::listen(listen_socket, 1) != 0) return;
        ready = true;
        thread = std::thread([this] { accept_one(); });
    }

    ~StallServer() {
        stop = true;
        if (listen_socket != invalid_socket) close_socket(listen_socket);
        if (thread.joinable()) thread.join();
        if (accepted != invalid_socket) close_socket(accepted);
#if defined(_WIN32)
        WSACleanup();
#endif
    }

    [[nodiscard]] bool ok() const noexcept { return ready; }
    [[nodiscard]] int port() const noexcept { return port_; }

private:
    void accept_one() {
        accepted = ::accept(listen_socket, nullptr, nullptr);
        while (!stop && accepted != invalid_socket) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    Socket listen_socket = invalid_socket;
    Socket accepted = invalid_socket;
    int port_ = 0;
    bool ready = false;
    std::atomic<bool> stop{false};
    std::thread thread;
};

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

TEST_CASE("Close after pause returns and playback can restart", "[player][lifecycle]") {
    for (int attempt = 0; attempt < 25; ++attempt) {
        imvideo::Player player;
        imvideo::Options options;
        options.autoplay = false;
        REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_IMAGE), options));
        player.play();
        player.pause();
        player.close();
        REQUIRE(player.state() == imvideo::State::Idle);
    }
}

TEST_CASE("Playback reaches the end of a finite clip", "[player][timeline]") {
    imvideo::Player player;
    REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_CLIP)));
    REQUIRE(player.duration() > 1.8);
    REQUIRE(player.duration() < 2.2);

    REQUIRE(wait_until([&player] { return player.state() == imvideo::State::Ended; }, std::chrono::seconds(5)));
    UNSCOPED_INFO("position=" << player.position() << " duration=" << player.duration());
    // The last frame PTS is one frame before the container duration.
    REQUIRE(player.position() + 0.12 >= player.duration());
}

TEST_CASE("Seek presents the requested media time", "[player][timeline]") {
    imvideo::Options options;
    options.autoplay = false;
    imvideo::Player player;
    REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_CLIP), options));
    REQUIRE(player.seekable());
    REQUIRE(player.seek(1.0));

    player.play();
    REQUIRE(wait_until([&player] { return static_cast<bool>(player.frame()); }));
    REQUIRE(player.position() >= 0.8);
    REQUIRE(player.position() < 1.6);
}

TEST_CASE("Position is relative to a non-zero input start time", "[player][timeline]") {
    imvideo::Options options;
    options.autoplay = false;
    imvideo::Player player;
    REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_OFFSET), options));
    REQUIRE(player.duration() > 1.5);
    REQUIRE(player.duration() < 2.5);
    REQUIRE_FALSE(player.live());

    player.play();
    REQUIRE(wait_until([&player] { return static_cast<bool>(player.frame()); }));
    REQUIRE(player.position() >= 0.0);
    REQUIRE(player.position() < 0.5);
}

TEST_CASE("A looping clip restarts instead of ending", "[player][timeline]") {
    imvideo::Options options;
    options.loop = true;
    imvideo::Player player;
    REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_CLIP), options));

    bool saw_late_position = false;
    REQUIRE(wait_until(
        [&player, &saw_late_position] {
            if (player.position() > 1.5) saw_late_position = true;
            return saw_late_position && player.position() < 0.4 && player.state() == imvideo::State::Playing;
        },
        std::chrono::seconds(6)));
}

TEST_CASE("An audio sink receives samples from a clip", "[player][audio]") {
    auto sink = std::make_shared<RecordingAudioSink>();
    imvideo::Options options;
    options.audio_sink = sink;
    imvideo::Player player;
    REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_CLIP), options));
    REQUIRE(player.audio_enabled());
    REQUIRE(wait_until([&sink] { return sink->write_calls.load() > 0; }));
    player.close();
    REQUIRE(sink->close_calls.load() == 1);
}

TEST_CASE("Video does not run ahead of a stalled audio clock", "[player][sync]") {
    auto sink = std::make_shared<RecordingAudioSink>();
    imvideo::Options options;
    options.audio_sink = sink;
    imvideo::Player player;
    REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_CLIP), options));
    REQUIRE(player.audio_enabled());

    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    UNSCOPED_INFO("position=" << player.position());
    REQUIRE(player.position() < 0.3);
    player.close();
    REQUIRE(player.state() == imvideo::State::Idle);
}

TEST_CASE("close() cancels an open that is blocked in another thread", "[player][lifecycle]") {
    StallServer server;
    REQUIRE(server.ok());
    imvideo::Player player;
    std::atomic<bool> finished{false};
    bool opened = true;
    const auto uri = "http://127.0.0.1:" + std::to_string(server.port()) + "/clip.mp4";
    std::thread opener([&] {
        opened = player.open(imvideo::Source::url(uri));
        finished = true;
    });

    REQUIRE(wait_until([&player] { return player.state() == imvideo::State::Opening; }, std::chrono::seconds(2)));
    player.close();
    REQUIRE(wait_until([&finished] { return finished.load(); }, std::chrono::seconds(3)));
    opener.join();
    REQUIRE_FALSE(opened);
    REQUIRE(player.state() == imvideo::State::Idle);
}

TEST_CASE("Anamorphic video uses display dimensions", "[player][display]") {
    imvideo::Options options;
    options.autoplay = false;
    imvideo::Player player;
    REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_WIDE), options));
    player.play();
    REQUIRE(wait_until([&player] { return static_cast<bool>(player.frame()); }));
    REQUIRE(player.frame().width() == 320);
    REQUIRE(player.frame().height() == 120);
}

TEST_CASE("Rotated video uses display dimensions", "[player][display]") {
    imvideo::Options options;
    options.autoplay = false;
    imvideo::Player player;
    REQUIRE(player.open(imvideo::Source::file(IMVIDEO_TEST_ROTATED), options));
    player.play();
    REQUIRE(wait_until([&player] { return static_cast<bool>(player.frame()); }));
    REQUIRE(player.frame().width() == 120);
    REQUIRE(player.frame().height() == 160);
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
