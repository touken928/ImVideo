#include <imvideo/player.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <limits>
#include <utility>

using Catch::Matchers::WithinAbs;

TEST_CASE("A new player has idle defaults", "[player][control]") {
    const imvideo::Player player;

    REQUIRE(player.state() == imvideo::State::Idle);
    REQUIRE_THAT(player.position(), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(player.duration(), WithinAbs(0.0, 1e-12));
    REQUIRE_FALSE(player.seekable());
    REQUIRE_FALSE(player.live());
    REQUIRE_FALSE(player.can_set_speed());
    REQUIRE_THAT(player.speed(), WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(player.volume(), WithinAbs(1.0F, 1e-6));
    REQUIRE_FALSE(player.audio_enabled());
    REQUIRE(player.error().empty());
    REQUIRE_FALSE(player.frame());
}

TEST_CASE("Player clamps volume to the supported range", "[player][control]") {
    imvideo::Player player;

    player.set_volume(-1.0F);
    REQUIRE_THAT(player.volume(), WithinAbs(0.0F, 1e-6));

    player.set_volume(0.25F);
    REQUIRE_THAT(player.volume(), WithinAbs(0.25F, 1e-6));

    player.set_volume(2.0F);
    REQUIRE_THAT(player.volume(), WithinAbs(1.0F, 1e-6));
}

TEST_CASE("Playback controls are inert before a source is open", "[player][control]") {
    imvideo::Player player;

    player.play();
    player.pause();
    player.stop();

    REQUIRE(player.state() == imvideo::State::Idle);
    REQUIRE_FALSE(player.seek(1.0));
    REQUIRE_FALSE(player.seek(std::numeric_limits<double>::quiet_NaN()));
    REQUIRE_FALSE(player.set_speed(0.25));
    REQUIRE_FALSE(player.set_speed(1.0));
    REQUIRE_FALSE(player.set_speed(4.0));
    REQUIRE_FALSE(player.set_speed(std::numeric_limits<double>::infinity()));
    REQUIRE_THAT(player.speed(), WithinAbs(1.0, 1e-12));
}

TEST_CASE("An idle player can be moved", "[player][control]") {
    imvideo::Player original;
    original.set_volume(0.4F);

    imvideo::Player moved = std::move(original);

    REQUIRE(moved.state() == imvideo::State::Idle);
    REQUIRE_THAT(moved.volume(), WithinAbs(0.4F, 1e-6));
}
