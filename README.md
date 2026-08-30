# imvideo

`imvideo` is a lightweight C++17 video playback library for Dear ImGui. It uses
FFmpeg for file, HTTP and RTSP input, tries platform hardware decoders before
falling back to software decoding, and exposes decoded video through one simple
pipeline:

```
Source -> Player -> Frame -> Renderer -> OpenGL texture
```

The library uses OpenGL for rendering. It does not depend on a window toolkit;
the host may use GLFW, SDL3, Qt, or another OpenGL context provider.

## Build and package

Conan 2, CMake and Ninja are required:

```sh
conan profile detect --force
conan create . -s build_type=Release -b missing
```

The recipe creates `imvideo/0.1.0`. Consumers use the normal Conan CMake flow
and link `imvideo::imvideo` after `find_package(imvideo CONFIG REQUIRED)`.

## Build the example through the package

The example deliberately consumes the packaged library rather than adding the
source tree as a subdirectory:

```sh
cd example/implayer
conan install . -of build -s build_type=Release -b missing
cmake --preset conan-release
cmake --build --preset conan-release
./build/build/Release/implayer /path/to/movie.mp4
```

Pass an HTTP(S) URL or RTSP URL in place of a local file. The example provides
play/pause, seeking when supported, `0.5x` to `2.0x` playback controls, volume
control, and automatic sizing.

## Windows releases

The `Release implayer for Windows` GitHub Actions workflow builds a statically
linked Windows x86_64 executable as a downloadable workflow artifact when run
manually. Pushing a version tag such as `v0.1.0` runs the same packaged-library
build and publishes `implayer.exe` directly in a GitHub Release with
automatically generated release notes. The executable still uses Windows system
libraries, but does not require separately distributed FFmpeg, GLFW, or MSVC
runtime DLLs.

```sh
git tag v0.1.0
git push origin v0.1.0
```

Update the versions in `conanfile.py` and `CMakeLists.txt` before tagging a new
library release.

## API sketch

```cpp
imvideo::Player player;
imvideo::Renderer renderer;
imvideo::Options options;
options.autoplay = true;
options.loop = false;
options.audio_sink = std::make_shared<ApplicationAudioSink>();
player.open(imvideo::Source::file("movie.mp4"), options);
player.set_speed(1.5);

if (auto frame = player.frame(); frame && renderer.update(frame)) {
    const auto texture = (ImTextureID)(intptr_t)renderer.texture();
    ImGui::Image(texture, {
        static_cast<float>(renderer.width()),
        static_cast<float>(renderer.height())
    });
}
```

`Source` records how the input should be opened instead of inferring that choice
again inside `Player`. Use `file()`, `url()`, or `rtsp()` at the call site:

```cpp
player.open(imvideo::Source::url("https://example.com/movie.mp4"), options);
player.open(imvideo::Source::rtsp("rtsp://camera/live"), options);
```

The core package does not depend on Dear ImGui. `Renderer::texture()` returns a
`std::uintptr_t` containing the OpenGL texture name. The application converts it
to its UI toolkit's texture type at the call site, as shown above.

The core package also has no audio-device dependency. Applications optionally
implement `imvideo::AudioSink` and assign a `std::shared_ptr` to
`Options::audio_sink`; the `implayer` example supplies `MiniaudioSink`. Leave
`audio_sink` empty for silent previews or multi-camera RTSP walls. In that mode
no audio decoder or resampler is created. `Player` retains the sink until its
decode thread has stopped, and `set_volume()` forwards volume to the sink.

`Player::set_speed()` accepts speeds from `0.25x` through `4.0x` for
seekable, non-live inputs. Video presentation timing is scaled and audio is
processed through FFmpeg's `atempo` filter so pitch is preserved. RTSP and
non-seekable/live HTTP inputs remain fixed at `1.0x`; use
`can_set_speed()` to decide whether to expose a speed control.
