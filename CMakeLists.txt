cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Project
# ---------------------------------------------------------------------------
project(imvideo VERSION 0.1.0
    DESCRIPTION "A Dear ImGui video playback extension library (FFmpeg + miniaudio + OpenGL)"
    LANGUAGES CXX C
)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

# ---- C++20 ---------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Find dependencies ---------------------------------------------------
find_package(FFMPEG REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)
find_package(imgui CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

# miniaudio is header-only
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)

# ---- Library -------------------------------------------------------------
add_library(imvideo)

target_compile_features(imvideo PUBLIC cxx_std_20)

target_sources(imvideo PRIVATE
    src/player.cpp
    src/video.cpp
    src/audio_engine.cpp
)

target_include_directories(imvideo
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${FFMPEG_INCLUDE_DIRS}
        ${MINIAUDIO_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(imvideo
    PUBLIC
        imgui::imgui
    PRIVATE
        FFMPEG::avcodec
        FFMPEG::avformat
        FFMPEG::avutil
        FFMPEG::swscale
        FFMPEG::swresample
        OpenGL::GL
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
