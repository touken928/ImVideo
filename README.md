# imvideo

**A Dear ImGui video playback extension library.**  
FFmpeg decode + miniaudio output + OpenGL texture upload.

## Library overview

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

## How to consume (via vcpkg overlay)

This repository does **not** provide a pre-built vcpkg port. Instead, refer to
[`examples/imv/`](examples/imv/) which is a complete standalone CMake project
that consumes `imvideo` via a **vcpkg overlay port** — the recommended way to
use local/custom libraries in vcpkg.

Inside `examples/imv/` you will find two overlay port examples:

| Directory | Portfile type | Preset |
|-----------|---------------|--------|
| `overlay/imvideo/` | Local source path | `cmake --preset default` |
| `overlay-github/imvideo/` | `vcpkg_from_github(touken928/imvideo)` | `cmake --preset from-github` |

Study these files to learn the overlay port pattern and adapt it to your own
project.

### Quick demo

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

## Project layout

```
├── CMakeLists.txt              # Library build
├── include/imvideo/imvideo.h   # Public API (single header)
├── src/                        # Library implementation
└── examples/imv/               # Standalone demo + vcpkg overlay reference
    ├── overlay/imvideo/        # Local path overlay port
    ├── overlay-github/imvideo/ # GitHub-based overlay port
    ├── CMakePresets.json       # Two presets: default & from-github
    └── ...
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

Apache-2.0
