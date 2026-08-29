# Repository Guidelines

## Project Structure & Module Organization

Public C++17 headers live in `include/imvideo/`; keep FFmpeg and implementation details out of this API surface. Implementations are in `src/`, with private shared definitions such as `frame_internal.hpp` remaining there. `tests/` contains the lightweight player test and its `frame.xbm` fixture. `example/implayer/` is a standalone Dear ImGui application that deliberately consumes the packaged library rather than adding the source tree directly. CMake package helpers are under `cmake/`.

The core flow is `Source -> Player -> Frame -> Renderer`. Audio-device integration is caller-owned through `AudioSink`; miniaudio belongs only to the example.

## Build, Test, and Development Commands

Use Conan 2, CMake, and Ninja:

```sh
conan profile detect --force
conan create . -s build_type=Release -b missing
```

This creates `imvideo/0.1.0` and verifies that the package installs correctly. To build and run the repository test target:

```sh
conan install . -of build/test -s build_type=Release -b missing
cmake -S . -B build/test/build/Release -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=build/test/build/Release/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release -DIMVIDEO_BUILD_TESTS=ON
cmake --build build/test/build/Release
ctest --test-dir build/test/build/Release --output-on-failure
```

Build the packaged example from `example/implayer/` using the commands in `README.md`.

## Coding Style & Naming Conventions

Use four-space indentation and C++17. Follow the existing style: `PascalCase` for types, `snake_case` for functions and variables, and trailing underscores for private data members. Keep braces on the declaration line and prefer RAII, `std::unique_ptr`, and `std::shared_ptr` over manual ownership. No formatter is configured; run `git diff --check` before committing.

## Testing Guidelines

Tests use a small executable with the local `CHECK` macro rather than an external framework. Add focused regression checks to `tests/player_test.cpp`; place deterministic fixtures in `tests/`. Avoid network-dependent tests in the committed suite. Validate package creation and rebuild `implayer` when changing public APIs or exported dependencies.

## Commit & Pull Request Guidelines

Use concise, imperative commit subjects, as in `Add playback speed support`. Keep commits scoped to one coherent change. Pull requests should explain behavior and API changes, list platforms and commands tested, link relevant issues, and include screenshots for visible `implayer` UI changes. Never commit build directories, Conan caches, credentials, or private media URLs.
