#include "frame_internal.hpp"

namespace imvideo {

Frame::operator bool() const noexcept { return impl_ && impl_->av_frame; }
int Frame::width() const noexcept { return *this ? impl_->geometry.width : 0; }
int Frame::height() const noexcept { return *this ? impl_->geometry.height : 0; }
std::int64_t Frame::pts() const noexcept { return *this ? impl_->pts_value : 0; }
bool Frame::operator==(const Frame& other) const noexcept { return impl_ == other.impl_; }
bool Frame::operator!=(const Frame& other) const noexcept { return !(*this == other); }

} // namespace imvideo
