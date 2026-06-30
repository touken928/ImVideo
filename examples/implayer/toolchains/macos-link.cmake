# ---------------------------------------------------------------------------
# macOS — system frameworks required by imvideo / GLFW / OpenGL
#
# Included by CMakeLists.txt after the ImPlayer target is defined.
# ---------------------------------------------------------------------------

target_link_libraries(ImPlayer PRIVATE
    "-framework CoreVideo"
    "-framework VideoToolbox"
    "-framework CoreMedia"
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework CoreFoundation"
)
