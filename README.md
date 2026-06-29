# imvideo

**Cross-platform video player library for Dear ImGui applications.**

`imvideo` gives you a minimal, single-header API to open, decode, and display
video (with audio) inside ImGui windows.

- **Decode** — FFmpeg demuxing + software decode on a dedicated worker thread.
- **Video** — swscale RGBA conversion → bounded thread-safe queue → OpenGL
  texture upload on the render thread (via `Update()`).
- **Audio** — swresample → float PCM → lock-free ring buffer → miniaudio
  playback callback (separate audio thread). Playback position uses a monotonic
  wall clock so audio underruns or device state cannot freeze video timing.
- **Queueing** — worker now blocks briefly instead of dropping decoded PCM/video
  when bounded buffers are temporarily full, which improves smoothness and seek
  recovery under normal UI load.
- **Stability model** — decode is throttled by bounded audio/video buffers during
  playback, and interrupted by pause/stop/seek. This prevents the worker from
  racing to EOF and dropping future audio.
- **Speed** — locked to 1.0 when audio is active (no time-stretch in v2).
  The `Video()` UI disables the speed slider and shows a tooltip.

---

## Build

### Prerequisites

- **CMake ≥ 3.20**
- **C++20 compiler** (Clang 14+, GCC 11+, MSVC 2022+)
- **Ninja** (recommended) or Make
- **[vcpkg](https://github.com/microsoft/vcpkg)** with `VCPKG_ROOT` set

### Library + demo (from project root)

```bash
git clone <repo-url> imvideo
cd imvideo

cmake -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build

# Run the demo player
./build/examples/imv/imv /path/to/video.mp4
```

### Standalone demo (independent vcpkg project)

`examples/imv/` is also a fully standalone CMake project with its own presets.

```bash
cd examples/imv

cmake --preset default
cmake --build --preset default

./build/default/imv /path/to/video.mp4
```

### Options

| CMake option | Default | Description |
|---|---|---|
| `IMVIDEO_BUILD_DEMO` | `ON` | Build the `imv` demo application |

### Manual configure

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build
```

---

## API example

```cpp
#include <imvideo/imvideo.h>

imvideo::Player player;

// Open a file
if (!player.Open("video.mp4")) {
    fprintf(stderr, "Error: %s\n", player.LastError());
    return;
}

// Start playback
player.Play();

// Inside your render loop (on the thread with a current GL context):
player.Update();                // pop decoded frame + upload texture

// Inside an ImGui window:
imvideo::Video(player);         // full controls: image + timeline + buttons

// Or just the image:
imvideo::Image(player, ImVec2(640, 360));
```

---

## Public API

All symbols live in namespace `imvideo`.

### `Player` class

| Method | Description |
|---|---|
| `Open(path)` | Open a video file.  Returns `false` on error. |
| `Close()` | Close and release all resources. |
| `Play()` | Start or resume playback. |
| `Pause()` | Pause at current position. |
| `Stop()` | Stop and rewind to beginning. |
| `Seek(s)` | Seek to `s` seconds. |
| `Position()` | Current playback position in seconds, driven by a monotonic wall clock. |
| `Duration()` | Total duration in seconds (0 if unknown). |
| `IsPlaying()` | Whether playback is active. |
| `SetVolume(v)` | Volume 0–1. |
| `SetSpeed(s)` | Playback speed (≥ 0.0625). Locked at 1.0 when audio is active. |
| `Update()` | **Must be called each frame** with a current GL context. |
| `Texture()` | Opaque `ImTextureID` (GLuint) for the current frame. |
| `VideoSize()` | Native video dimensions. |
| `LastError()` | Human-readable error string. |

### Free functions

| Function | Description |
|---|---|
| `Image(player, size)` | Draw the current frame as a static image. |
| `Video(player, size)` | Draw a full video widget (image + controls). |

---

## ⚠ Render-thread / OpenGL caveat

**`Update()` and the `Player` destructor assume a current OpenGL context.**
They create, update, and destroy the internal texture.  Call them from the
same thread that owns the GL context (usually the main render / UI thread).
The library does **not** create its own context, nor does it call
`glfwMakeContextCurrent` or similar.

The decode worker and audio callback run on separate threads and do not
touch OpenGL.

---

## Demo features (`imv`)

- **Left/right layout**: video viewport on the left, playlist sidebar on the right
- **Native file dialog**: open video files via OS file picker (batch select)
- **Drag-and-drop** video files onto the window
- **Playlist**: scrollable list of opened videos, click to switch, context menu
- Command-line arguments: `imv file1.mp4 file2.mkv`
- Per-video playback controls: play/pause, seek timeline, time display, volume
- Auto-scale video to window content (maintains aspect ratio)
- Audio playback for files with audio streams

---

## Project layout

```
├── CMakeLists.txt              # Top-level library build
├── CMakePresets.json           # Presets for quick configure/build
├── vcpkg.json                  # vcpkg manifest (dependencies)
├── cmake/
│   ├── FindFFMPEG.cmake        # Find module for FFmpeg
│   └── imvideoConfig.cmake.in  # Package config template
├── include/imvideo/
│   └── imvideo.h               # Public API (single header)
├── src/                        # Library implementation
│   ├── player_impl.hpp
│   ├── player.cpp
│   ├── video.cpp
│   ├── audio_engine.hpp / .cpp
│   ├── ring_buffer.hpp
│   └── video_frame.hpp
└── examples/imv/               # Standalone demo player (own vcpkg + CMake)
    ├── CMakeLists.txt
    ├── CMakePresets.json
    ├── vcpkg.json
    ├── main.cpp / app.hpp / app.cpp
    ├── file_dialog.hpp / _mac.mm / _linux.cpp / _win.cpp
    └── overlay/imvideo/        # vcpkg overlay port for imvideo
```

---

## Dependencies (via vcpkg)

| Package | Notes |
|---|---|
| FFmpeg | `avcodec`, `avformat`, `avutil`, `swscale`, `swresample` |
| Dear ImGui | `glfw-binding`, `opengl3-binding`, `docking-experimental` |
| GLFW3 | Window / OpenGL context (demo & examples only) |
| OpenGL | System library |
| miniaudio | Audio playback (callback reads from PCM ring buffer) |

---

## Version history

- **v0.1** — Multi-threaded decode, audio playback via miniaudio, stable wall-clock
  playback timing, bounded queues, speed locked when audio active, EOS handling.
- **v0.1** — Single-threaded video-only prototype.
