#pragma once

#include <cstdint>
#include <memory>

namespace imvideo {

class Frame {
public:
    Frame() = default;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] std::int64_t pts() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;

    friend class Player;
    friend class Renderer;
};

} // namespace imvideo
