#pragma once

#include <string>

namespace imvideo {

enum class SourceMode { File, Url, Rtsp };

class Source {
public:
    [[nodiscard]] static Source file(std::string path);
    [[nodiscard]] static Source url(std::string uri);
    [[nodiscard]] static Source rtsp(std::string uri);

    [[nodiscard]] const std::string& uri() const noexcept;
    [[nodiscard]] SourceMode mode() const noexcept;

private:
    Source(std::string uri, SourceMode mode);

    std::string uri_;
    SourceMode mode_;
};

} // namespace imvideo
