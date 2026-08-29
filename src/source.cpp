#include <imvideo/source.hpp>

#include <utility>

namespace imvideo {

Source Source::file(std::string path) { return {std::move(path)}; }
Source Source::url(std::string url) { return {std::move(url)}; }
Source Source::rtsp(std::string url) { return {std::move(url)}; }

} // namespace imvideo
