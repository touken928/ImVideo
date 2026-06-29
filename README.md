# imvideo

**A Dear ImGui video playback extension library.**  
FFmpeg decode + miniaudio output + OpenGL texture upload.

## Use as a library (via vcpkg)

Add `imvideo` to your project's `vcpkg.json` and point the overlay port to the
cloned repository:

```json
{
  "dependencies": ["imvideo"],
  "vcpkg-configuration": {
    "overlay-ports": [ "./path/to/imvideo/ports" ]
  }
}
```

Then `find_package(imvideo CONFIG REQUIRED)` and link `imvideo::imvideo`:

```cmake
find_package(imvideo CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE imvideo::imvideo)
```

**Quick start via GitHub:**

```bash
git clone https://github.com/touken928/imvideo.git
```

Then add `"overlay-ports": [ "./imvideo/ports" ]` to your project's
`vcpkg-configuration` (or `vcpkg.json`).

### Minimal usage

```cpp
#include <imvideo/imvideo.h>

imvideo::Player player;

if (!player.Open("video.mp4")) {
    fprintf(stderr, "Error: %s\n", player.LastError());
    return;
}
player.Play();

// In your render loop (with a current GL context):
player.Update();

// Inside an ImGui window:
imvideo::Video(player);   // full widget: image + timeline + buttons
imvideo::Image(player, ImVec2(640, 360));  // frame only
```

## Build the demo (IMV)

`examples/imv/` is a standalone CMake + vcpkg project that demonstrates the
library. Build it independently:

```bash
cd examples/imv

cmake --preset default
cmake --build --preset default

./build/default/imv /path/to/video.mp4
```

## Public API

All symbols in `imvideo` namespace.

### `Player` class

| Method | Description |
|--------|-------------|
| `Open(path)` | Open a video file. Returns `false` on error. |
| `Close()` | Close and release all resources. |
| `Play()` | Start or resume playback. |
| `Pause()` | Pause at current position. |
| `Stop()` | Stop and rewind to beginning. |
| `Seek(s)` | Seek to `s` seconds. |
| `Position()` | Current playback position in seconds (wall clock). |
| `Duration()` | Total duration in seconds (0 if unknown). |
| `IsPlaying()` | Whether playback is active. |
| `SetVolume(v)` | Volume 0–1. |
| `SetSpeed(s)` | Speed (≥ 0.0625). Locked to 1.0 when audio is active. |
| `Update()` | **Must be called each frame** with a current GL context. |
| `Texture()` | Current frame as `ImTextureID`. |
| `VideoSize()` | Native video dimensions. |
| `LastError()` | Human-readable error string. |

### Free functions

| Function | Description |
|----------|-------------|
| `Image(player, size)` | Draw the current frame as a static image. |
| `Video(player, size)` | Draw a full video widget (image + controls). |

## ⚠ Important: OpenGL context requirement

`Update()` and the `Player` destructor **require a current OpenGL context**.
Call them from the same thread that owns the GL context (the render/UI thread).
The library does not create or manage GL contexts.

## Project layout

```
├── CMakeLists.txt              # Library build
├── vcpkg.json                  # Library dependencies (ffmpeg, imgui, miniaudio)
├── ports/imvideo/              # vcpkg overlay port (for consumers)
├── cmake/                      # CMake helpers
├── include/imvideo/imvideo.h   # Public API (single header)
├── src/                        # Library implementation
└── examples/imv/               # Standalone demo (own vcpkg + CMake)
```

## Dependencies

| Package | Role |
|---------|------|
| FFmpeg | Demux + decode (avcodec, avformat, swscale, swresample) |
| Dear ImGui | Public API dependency (`imgui.h`) |
| miniaudio | Audio output (PCM ring buffer → callback) |
| OpenGL | Texture upload (system library) |

*GLFW and ImGui backends are required by the demo only, not by the library.*

## License

MIT
