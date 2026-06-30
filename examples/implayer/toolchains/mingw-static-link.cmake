# ---------------------------------------------------------------------------
# MinGW static — resolve circular dependencies between FFmpeg components
#
# Included by CMakeLists.txt after the ImPlayer target is defined.
# When linking statically, libavformat references symbols in libavcodec
# and vice versa.  --start-group / --end-group tells GNU ld to resolve
# them in any order.
# ---------------------------------------------------------------------------

target_link_libraries(ImPlayer PRIVATE
    "-Wl,--start-group"
    FFMPEG::avcodec
    FFMPEG::avformat
    FFMPEG::avutil
    FFMPEG::swscale
    FFMPEG::swresample
    "-Wl,--end-group"
)
