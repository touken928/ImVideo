cmake_minimum_required(VERSION 3.23)
project(imvideo VERSION 0.2.0 LANGUAGES C CXX)

option(IMVIDEO_BUILD_TESTS "Build imvideo tests" OFF)

find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil avfilter swscale swresample)
find_package(Threads REQUIRED)
if(EMSCRIPTEN)
    if(BUILD_SHARED_LIBS)
        message(FATAL_ERROR "imvideo only supports static libraries with Emscripten")
    endif()
else()
    find_package(OpenGL REQUIRED)
endif()

add_library(imvideo
    src/audio_pipeline.cpp
    src/display_frame.cpp
    src/frame.cpp
    src/player.cpp
    src/renderer.cpp
    src/source.cpp
)
add_library(imvideo::imvideo ALIAS imvideo)
target_compile_features(imvideo PUBLIC cxx_std_17)
target_include_directories(imvideo
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE src
)
target_link_libraries(imvideo
    PRIVATE
        ffmpeg::avformat
        ffmpeg::avcodec
        ffmpeg::avutil
        ffmpeg::avfilter
        ffmpeg::swscale
        ffmpeg::swresample
        Threads::Threads
)
if(EMSCRIPTEN)
    target_sources(imvideo PRIVATE src/renderer_yuv.cpp)
    # These are usage requirements of the static library, including its consumer.
    target_compile_options(imvideo PUBLIC -pthread -fexceptions)
    target_link_options(imvideo PUBLIC -pthread -fexceptions
        "-sMIN_WEBGL_VERSION=2" "-sMAX_WEBGL_VERSION=2")
else()
    target_link_libraries(imvideo PRIVATE OpenGL::GL)
endif()
if(WIN32)
    target_link_libraries(imvideo PRIVATE ws2_32)
endif()
set_target_properties(imvideo PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS imvideo EXPORT imvideoTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT imvideoTargets FILE imvideoTargets.cmake NAMESPACE imvideo:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/imvideo)
configure_package_config_file(cmake/imvideoConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/imvideoConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/imvideo
)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/imvideoConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/imvideoConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/imvideoConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/imvideo
)

if(IMVIDEO_BUILD_TESTS)
    if(EMSCRIPTEN)
        message(FATAL_ERROR "Native CTest targets require a native build; cross-compile with IMVIDEO_BUILD_TESTS=OFF")
    endif()
    enable_testing()
    find_package(Catch2 3 REQUIRED)
    include(Catch)

    add_executable(imvideo_unit_tests
        tests/frame_test.cpp
        tests/packet_queue_test.cpp
        tests/player_control_test.cpp
        tests/source_test.cpp
    )
    target_include_directories(imvideo_unit_tests PRIVATE src)
    target_link_libraries(imvideo_unit_tests PRIVATE imvideo::imvideo Catch2::Catch2WithMain ffmpeg::avcodec)
    catch_discover_tests(imvideo_unit_tests
        TEST_PREFIX "unit: "
        PROPERTIES LABELS unit TIMEOUT 10
    )

    add_executable(imvideo_component_tests
        tests/audio_pipeline_test.cpp
        tests/player_component_test.cpp
    )
    target_include_directories(imvideo_component_tests PRIVATE src)
    target_link_libraries(imvideo_component_tests PRIVATE imvideo::imvideo Catch2::Catch2WithMain ffmpeg::avcodec)
    target_compile_definitions(imvideo_component_tests PRIVATE
        IMVIDEO_TEST_IMAGE="${CMAKE_CURRENT_SOURCE_DIR}/tests/frame.xbm"
        IMVIDEO_TEST_CLIP="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/clip.mkv"
        IMVIDEO_TEST_OFFSET="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/offset.ts"
        IMVIDEO_TEST_WIDE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wide.mkv"
        IMVIDEO_TEST_ROTATED="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rotated.mkv")
    if(WIN32)
        target_link_libraries(imvideo_component_tests PRIVATE ws2_32)
    endif()
    catch_discover_tests(imvideo_component_tests
        TEST_PREFIX "component: "
        PROPERTIES LABELS component TIMEOUT 10
    )
endif()
