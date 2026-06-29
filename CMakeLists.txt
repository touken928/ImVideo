cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Project
# ---------------------------------------------------------------------------
project(imvideo VERSION 0.1.0
    DESCRIPTION "Cross-platform video player for Dear ImGui"
    LANGUAGES CXX C
)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

# ---- C++20 ---------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Options -------------------------------------------------------------
option(IMVIDEO_BUILD_DEMO "Build the imv demo application" ON)

# ---- Find dependencies ---------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# FFmpeg  (variable-style usage as required)
find_package(FFMPEG REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)

# ImGui  (config-mode, installed via vcpkg)
find_package(imgui CONFIG REQUIRED)

# OpenGL
find_package(OpenGL REQUIRED)

if(IMVIDEO_BUILD_DEMO)
    find_package(glfw3 CONFIG REQUIRED)
endif()

# ---- Library -------------------------------------------------------------
add_library(imvideo)

target_compile_features(imvideo PUBLIC cxx_std_20)

target_sources(imvideo PRIVATE
    src/player.cpp
    src/video.cpp
    src/audio_engine.cpp
)

# miniaudio is header-only
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)

target_include_directories(imvideo
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${FFMPEG_INCLUDE_DIRS}
        ${MINIAUDIO_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src    # for private headers if any
)

target_link_libraries(imvideo
    PUBLIC
        imgui::imgui                       # public header includes <imgui.h>
    PRIVATE
        FFMPEG::avcodec
        FFMPEG::avformat
        FFMPEG::avutil
        FFMPEG::swscale
        FFMPEG::swresample
        OpenGL::GL                         # private: texture upload
)

if(APPLE)
    target_link_libraries(imvideo
        PRIVATE
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework CoreFoundation"
            "-framework VideoToolbox"
            "-framework CoreMedia"
    )
endif()

# Alias for consumer find_package
add_library(imvideo::imvideo ALIAS imvideo)

# ---- Compile definitions -------------------------------------------------
target_compile_definitions(imvideo PRIVATE
    $<$<CONFIG:Debug>:IMVIDEO_DEBUG>
)

# ---- Install -------------------------------------------------------------

set(IMVIDEO_HEADERS
    include/imvideo/imvideo.h
)

install(TARGETS     imvideo
    EXPORT           imvideoTargets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES       ${IMVIDEO_HEADERS}
    DESTINATION     ${CMAKE_INSTALL_INCLUDEDIR}/imvideo
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/imvideoConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/imvideoConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/imvideoConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/imvideo
)

install(EXPORT      imvideoTargets
    FILE             imvideoTargets.cmake
    NAMESPACE        imvideo::
    DESTINATION      ${CMAKE_INSTALL_LIBDIR}/cmake/imvideo
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/imvideoConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/imvideoConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/imvideo
)

# ---- Demo ----------------------------------------------------------------
if(IMVIDEO_BUILD_DEMO)
    add_subdirectory(examples/imv)
endif()
