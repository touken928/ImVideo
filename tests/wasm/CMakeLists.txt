cmake_minimum_required(VERSION 3.23)
project(imvideo_wasm_tests LANGUAGES CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "WASM tests require Emscripten")
endif()
find_package(imvideo CONFIG REQUIRED
    PATHS "${IMVIDEO_PACKAGE_DIR}/lib/cmake/imvideo" NO_DEFAULT_PATH)
if(TARGET OpenGL::GL)
    message(FATAL_ERROR "The WASM package must not import desktop OpenGL")
endif()

add_executable(wasm_tests main.cpp)
target_link_libraries(wasm_tests PRIVATE imvideo::imvideo)
target_link_options(wasm_tests PRIVATE
    "-sPTHREAD_POOL_SIZE=2" "-sALLOW_BLOCKING_ON_MAIN_THREAD=0"
    "-sINITIAL_MEMORY=128MB" "-sALLOW_MEMORY_GROWTH=0" "-sEXIT_RUNTIME=0"
    "-sSTACK_SIZE=1MB" "-sDEFAULT_PTHREAD_STACK_SIZE=1MB" "-sASSERTIONS=1"
    "SHELL:--preload-file \"${CMAKE_CURRENT_BINARY_DIR}/media@/media\""
)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/media")
foreach(fixture clip.mkv wide.mkv rotated.mkv)
    configure_file("../fixtures/${fixture}" "media/${fixture}" COPYONLY)
endforeach()
configure_file(../frame.xbm media/frame.xbm COPYONLY)
configure_file(runtime.html runtime.html COPYONLY)
