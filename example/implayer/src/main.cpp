#include <imvideo/player.hpp>
#include <imvideo/renderer.hpp>

#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui.h>

#include "miniaudio_sink.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: implayer <file-or-url>\n");
        return 2;
    }
    if (!glfwInit()) return 1;
#if defined(__APPLE__)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    GLFWwindow* window = glfwCreateWindow(1280, 720, "implayer", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    imvideo::Player player;
    imvideo::Renderer renderer;
    auto audio_sink = std::make_shared<MiniaudioSink>();
    const auto source = std::string_view(argv[1]).find("://") == std::string_view::npos
                            ? imvideo::Source::file(argv[1])
                            : (std::string_view(argv[1]).rfind("rtsp://", 0) == 0
                                   ? imvideo::Source::rtsp(argv[1])
                                   : imvideo::Source::url(argv[1]));
    imvideo::Options options;
    options.audio_sink = audio_sink;
    player.open(source, options);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (const auto frame = player.frame()) renderer.update(frame);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::Begin("Video");
        if (renderer.texture() != 0) {
            const float available = ImGui::GetContentRegionAvail().x;
            const float aspect = renderer.height() > 0 ? static_cast<float>(renderer.width()) / renderer.height() : 1.0F;
            const auto texture = (ImTextureID)(intptr_t)renderer.texture();
            ImGui::Image(texture, {available, available / aspect});
        } else if (player.state() == imvideo::State::Error) {
            ImGui::TextWrapped("%s", player.error().data());
        } else {
            ImGui::TextUnformatted("Opening video...");
        }
        float volume = player.volume();
        if (ImGui::SliderFloat("Volume", &volume, 0.0F, 1.0F)) player.set_volume(volume);
        if (ImGui::Button(player.state() == imvideo::State::Playing ? "Pause" : "Play")) {
            player.state() == imvideo::State::Playing ? player.pause() : player.play();
        }
        if (player.seekable()) {
            float position = static_cast<float>(player.position());
            const float duration = static_cast<float>(player.duration());
            if (ImGui::SliderFloat("Position", &position, 0.0F, duration, "%.1f s")) player.seek(position);
        }
        ImGui::End();

        ImGui::Render();
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.08F, 0.08F, 0.1F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    player.close();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
