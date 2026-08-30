cmake_minimum_required(VERSION 3.23)
project(implayer LANGUAGES CXX)

find_package(imvideo CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(miniaudio CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

set(IMGUI_BACKEND_PATHS)
foreach(IMGUI_CONFIGURATION RELEASE DEBUG RELWITHDEBINFO MINSIZEREL)
    if(DEFINED imgui_PACKAGE_FOLDER_${IMGUI_CONFIGURATION})
        list(APPEND IMGUI_BACKEND_PATHS "${imgui_PACKAGE_FOLDER_${IMGUI_CONFIGURATION}}/res/bindings")
    endif()
endforeach()
find_file(IMGUI_IMPL_GLFW_SOURCE imgui_impl_glfw.cpp PATHS ${IMGUI_BACKEND_PATHS} NO_DEFAULT_PATH REQUIRED)
find_file(IMGUI_IMPL_OPENGL3_SOURCE imgui_impl_opengl3.cpp PATHS ${IMGUI_BACKEND_PATHS} NO_DEFAULT_PATH REQUIRED)

add_executable(implayer
    src/main.cpp
    src/miniaudio_sink.cpp
    ${IMGUI_IMPL_GLFW_SOURCE}
    ${IMGUI_IMPL_OPENGL3_SOURCE}
)
target_compile_features(implayer PRIVATE cxx_std_17)
target_include_directories(implayer PRIVATE ${IMGUI_BACKEND_PATHS})
target_link_libraries(implayer PRIVATE imvideo::imvideo glfw imgui::imgui miniaudio::miniaudio OpenGL::GL)
if(WIN32)
    target_link_libraries(implayer PRIVATE ws2_32)
endif()
if(APPLE)
    target_compile_definitions(implayer PRIVATE GL_SILENCE_DEPRECATION)
endif()
