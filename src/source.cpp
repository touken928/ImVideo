#include <imvideo/source.hpp>

#include <utility>

namespace imvideo {

Source::Source(std::string uri, SourceMode mode) : uri_(std::move(uri)), mode_(mode) {}

Source Source::file(std::string path) { return Source{std::move(path), SourceMode::File}; }
Source Source::url(std::string uri) { return Source{std::move(uri), SourceMode::Url}; }
Source Source::rtsp(std::string uri) { return Source{std::move(uri), SourceMode::Rtsp}; }

const std::string& Source::uri() const noexcept { return uri_; }
SourceMode Source::mode() const noexcept { return mode_; }

} // namespace imvideo
