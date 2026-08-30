cmake_minimum_required(VERSION 3.23)
project(imvideo VERSION 0.1.0 LANGUAGES C CXX)

option(IMVIDEO_BUILD_TESTS "Build imvideo tests" OFF)

find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil avfilter swscale swresample)
find_package(OpenGL REQUIRED)

add_library(imvideo
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
        OpenGL::GL
)
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
    enable_testing()
    find_package(Catch2 3 REQUIRED)
    include(Catch)

    add_executable(imvideo_unit_tests
        tests/frame_test.cpp
        tests/player_control_test.cpp
        tests/source_test.cpp
    )
    target_link_libraries(imvideo_unit_tests PRIVATE imvideo::imvideo Catch2::Catch2WithMain)
    catch_discover_tests(imvideo_unit_tests
        TEST_PREFIX "unit: "
        PROPERTIES LABELS unit TIMEOUT 10
    )

    add_executable(imvideo_component_tests
        tests/player_component_test.cpp
    )
    target_link_libraries(imvideo_component_tests PRIVATE imvideo::imvideo Catch2::Catch2WithMain)
    target_compile_definitions(imvideo_component_tests PRIVATE
        IMVIDEO_TEST_IMAGE="${CMAKE_CURRENT_SOURCE_DIR}/tests/frame.xbm")
    catch_discover_tests(imvideo_component_tests
        TEST_PREFIX "component: "
        PROPERTIES LABELS component TIMEOUT 10
    )
endif()
