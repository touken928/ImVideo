#include <imvideo/frame.hpp>
#include <imvideo/renderer.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("A default frame is empty", "[frame]") {
    const imvideo::Frame frame;

    REQUIRE_FALSE(frame);
    REQUIRE(frame.width() == 0);
    REQUIRE(frame.height() == 0);
    REQUIRE(frame.pts() == 0);
}

TEST_CASE("Empty frames remain empty when copied", "[frame]") {
    const imvideo::Frame original;
    const auto copy = original;

    REQUIRE_FALSE(original);
    REQUIRE_FALSE(copy);
    REQUIRE(original == copy);
    REQUIRE_FALSE(original != copy);
}

TEST_CASE("Renderer invalidation is safe without a graphics context", "[renderer]") {
    imvideo::Renderer renderer;
    renderer.invalidate();
    renderer.invalidate();

    REQUIRE(renderer.texture() == 0);
    REQUIRE(renderer.width() == 0);
    REQUIRE(renderer.height() == 0);
    REQUIRE_FALSE(renderer.update({}));
}
