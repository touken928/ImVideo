#pragma once

#include <cstdint>
#include <memory>

namespace imvideo {

class Frame;

class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool update(const Frame& frame);
    // update() requires a current OpenGL context with pixel-unpack-buffer support.
    // Describe the most recent successful update. On failure these values remain unchanged,
    // though OpenGL may have partially modified the texture image.
    // OpenGL texture name; convert it to the UI toolkit's texture type at the call site.
    [[nodiscard]] std::uintptr_t texture() const noexcept;
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace imvideo
