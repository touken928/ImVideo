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
    // Handle identity: copies of one decoded frame compare equal, regardless
    // of timestamps. Distinct decoded frames do not compare their pixel data.
    [[nodiscard]] bool operator==(const Frame& other) const noexcept;
    [[nodiscard]] bool operator!=(const Frame& other) const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;

    friend class Player;
    friend class Renderer;
};

} // namespace imvideo
