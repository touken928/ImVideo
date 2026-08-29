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
play/pause, seeking when supported, volume control, and automatic sizing.

## API sketch

```cpp
imvideo::Player player;
imvideo::Renderer renderer;
imvideo::Options options;
options.autoplay = true;
options.loop = false;
options.audio_sink = std::make_shared<ApplicationAudioSink>();
player.open(imvideo::Source::file("movie.mp4"), options);

if (auto frame = player.frame(); frame && renderer.update(frame)) {
    const auto texture = (ImTextureID)(intptr_t)renderer.texture();
    ImGui::Image(texture, {
        static_cast<float>(renderer.width()),
        static_cast<float>(renderer.height())
    });
}
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
