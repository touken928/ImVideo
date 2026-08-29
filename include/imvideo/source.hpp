#pragma once

#include <string>

namespace imvideo {

struct Source {
    std::string uri;

    static Source file(std::string path);
    static Source url(std::string url);
    static Source rtsp(std::string url);
};

} // namespace imvideo
