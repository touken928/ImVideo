#include <imvideo/source.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

TEST_CASE("Source preserves its URI and mode", "[source]") {
    const auto file = imvideo::Source::file("movie.mp4");
    REQUIRE(file.uri() == "movie.mp4");
    REQUIRE(file.mode() == imvideo::SourceMode::File);

    const auto url = imvideo::Source::url("https://example.test/movie.mp4");
    REQUIRE(url.uri() == "https://example.test/movie.mp4");
    REQUIRE(url.mode() == imvideo::SourceMode::Url);

    const auto rtsp = imvideo::Source::rtsp("rtsp://camera.test/live");
    REQUIRE(rtsp.uri() == "rtsp://camera.test/live");
    REQUIRE(rtsp.mode() == imvideo::SourceMode::Rtsp);
}

TEST_CASE("Source takes ownership of a moved URI", "[source]") {
    std::string path = "clip.mp4";
    const auto source = imvideo::Source::file(std::move(path));

    REQUIRE(source.uri() == "clip.mp4");
}
