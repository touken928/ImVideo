# Core architecture and performance

The public flow remains `Source -> Player -> Frame -> Renderer`. FFmpeg types
and platform GL details stay under `src/`; applications provide the audio device
through `AudioSink`. Applications consume the installed Conan package.

| Module | Responsibility | Ownership / execution |
| --- | --- | --- |
| `player.cpp` | Open/close, demux/decode, controls, seek, presentation clocks | Control thread plus decoder worker |
| `audio_pipeline.*` | PCM format/rate conversion, optional tempo graph, EOF draining | Decoder worker; caller owns the device |
| `display_frame.cpp` | Display geometry, CPU conversion/rotation, deferred YUV frames | Decoder worker; reference-counted frame buffers |
| `packet_queue.hpp` | Bounded video read-ahead while supplying audio | One decoder worker; at most 64 retained packets |
| `ffmpeg_raii.hpp` | Frame/packet cleanup on every return path | Private RAII handles |
| `gl_api.hpp`, `gl_upload_state.hpp` | Platform GL dispatch and unpack-state restoration | Host's current GL context |
| `renderer*.cpp` | Texture upload and optional YUV conversion | Host render thread |

## Resource and thread boundaries

Player control operations belong to one application control thread. A concurrent
`close()` may cancel a blocked open. `frame()` and `error()` copy under their
respective mutexes; this does not make every getter safe across concurrent
open/close. Browser hosts run blocking Player operations on a worker and keep
Renderer calls on the thread owning the current WebGL context.

Closing stops and joins the decode worker before freeing codecs and input.
Outstanding `Frame` handles retain their own pixel-buffer references after
Player closes. Decoder scratch frames and prefetch packets are owned by RAII
handles; the hardware-transfer frame and audio-prefetch scratch are reused.
The prefetch queue transfers packet references, reuses headers, releases payloads
on seek, and frees headers on close. Unconsumed alternate streams do not occupy
the video queue.

Audio prefetch uses a separate frame from the video frame waiting for presentation.
Sharing that scratch frame would overwrite video while filling the device. The
timing tolerances and lead limits are named constants in Player, with unchanged
audio-master and steady-video-clock behavior.

## Hot paths

- At 1×, audio goes directly to PCM conversion/output. No filter graph is created
  or traversed. Compatible packed float stereo also avoids resampling; other
  formats retain conversion. Tempo changes create the graph when needed.
- EOF drains the optional filter, resampler and caller's sink in order. Bypassing
  the graph must not bypass resampler/sink tails. Filter failures are propagated.
- Display conversion pools RGBA storage and reuses rotation scratch. Unchanged
  input geometry keeps the configured swscale context, including its color state.
- Compatible Web YUV frames keep the GPU conversion path described in the
  [WebAssembly requirements](wasm-design.md). RGBA row strides upload
  directly. Both paths share the same scoped unpack/PBO/WebGL-state restoration.
  Windows GL dispatch is cached per renderer and cleared on context invalidation.

The changes remove work and allocations from repeated paths; they do not imply
a fixed end-to-end CPU percentage gain for every codec/device. Measure frame
cadence, audio underruns and process CPU separately from stage elapsed clocks
in the application using the library.

## Maintaining the design

Use `python tools/format.py --write` and `python tools/format.py` with
clang-format 22.1.8; CI runs the same check. Private classes use `PascalCase`,
methods and variables use `snake_case`, and private fields end in `_`.
Avoid putting FFmpeg or device-specific code in public headers.

Native CTest covers queue ordering/overflow/reference release, PCM preservation,
normal-rate resampler tails, and the public playback lifecycle. Native package
creation passed 38 tests. The packaged desktop example is a separate build
regression check.

The separate `tests/wasm` consumer links the installed CMake exports and runs
in headless Chromium on CI. It covers decoding, PCM audio, controls, rendering,
host GL state and context recovery; see the [WASM testing instructions](wasm-design.md).
