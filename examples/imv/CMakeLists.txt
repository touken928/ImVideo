# ---------------------------------------------------------------------------
# imv  —  Single-video player demo built on imvideo
#
# Dual-mode CMakeLists.txt:
#   - Top-level build: included via add_subdirectory from immpv/CMakeLists.txt.
#     Targets (imvideo::imvideo etc.) already exist from the parent project.
#   - Standalone build: invoked directly from examples/imv/.  Uses vcpkg to
#     install imvideo from the overlay port at overlay/imvideo/, which points
#     the source root at ../../../
# ---------------------------------------------------------------------------

# ---- standalone mode: declare project + find dependencies ----------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(imv VERSION 0.1.0
        DESCRIPTION "imv — imvideo demo player"
        LANGUAGES CXX
    )

    # Find all required packages (vcpkg provides them)
    # imvideo's exported targets reference FFMPEG::* targets, so find FFMPEG first
    find_package(FFMPEG REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)
    find_package(imvideo CONFIG REQUIRED)
    find_package(imgui  CONFIG REQUIRED)
    find_package(glfw3  CONFIG REQUIRED)
    find_package(OpenGL REQUIRED)
endif()

# ---- executable ----------------------------------------------------------
add_executable(imv
    main.cpp
    app.cpp
    app.hpp
)

target_compile_features(imv PUBLIC cxx_std_20)

target_link_libraries(imv PRIVATE
    imvideo::imvideo
    imgui::imgui
    glfw
    OpenGL::GL
)

# Platform-specific file dialog + system frameworks
if(APPLE)
    target_sources(imv PRIVATE file_dialog_mac.mm)
    target_link_libraries(imv PRIVATE
        "-framework AppKit"
        "-framework CoreVideo"
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
    )
elseif(WIN32)
    target_sources(imv PRIVATE file_dialog_win.cpp)
elseif(UNIX)
    target_sources(imv PRIVATE file_dialog_linux.cpp)
endif()
