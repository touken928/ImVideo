# ---------------------------------------------------------------------------
# ImPlayer  —  single-video player demo built on imvideo
#
# Dual-mode CMakeLists.txt:
#   - Top-level build: included via add_subdirectory from immpv/CMakeLists.txt.
#     Targets (imvideo::imvideo etc.) already exist from the parent project.
#   - Standalone build: invoked directly from examples/implayer/.  Uses vcpkg
#     to install imvideo from the overlay port at overlay/imvideo/, which
#     points the source root at ../../../
#
# Platform specifics (toolchains, link options, triplets) live entirely
# in toolchains/*.cmake — see CMakePresets.json for available presets.
# ---------------------------------------------------------------------------

# ---- standalone mode: declare project + find dependencies ----------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(implayer VERSION 0.1.0
        DESCRIPTION "ImPlayer — imvideo demo player"
        LANGUAGES CXX
    )

    find_package(FFMPEG REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)
    find_package(imvideo CONFIG REQUIRED)
    find_package(imgui  CONFIG REQUIRED)
    find_package(glfw3  CONFIG REQUIRED)
    find_package(tinyfiledialogs CONFIG REQUIRED)
    find_package(OpenGL REQUIRED)
endif()

# ---- executable ----------------------------------------------------------
add_executable(ImPlayer
    src/main.cpp
    src/app.cpp
    src/app.hpp
)

target_compile_features(ImPlayer PUBLIC cxx_std_20)

target_link_libraries(ImPlayer PRIVATE
    imvideo::imvideo
    imgui::imgui
    glfw
    tinyfiledialogs::tinyfiledialogs
    OpenGL::GL
)

# ---- platform-specific link options (all logic in toolchains/) ------------
if(DEFINED IMV_PLATFORM)
    include("${CMAKE_CURRENT_SOURCE_DIR}/toolchains/${IMV_PLATFORM}-link.cmake")
endif()
