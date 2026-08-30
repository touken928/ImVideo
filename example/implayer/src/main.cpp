#include <imvideo/player.hpp>
#include <imvideo/renderer.hpp>

#include "miniaudio_sink.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <cmath>
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
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    imvideo::Player player;
    imvideo::Renderer renderer;
    auto audio_sink = std::make_shared<MiniaudioSink>();
    const auto argument = std::string_view(argv[1]);
    const auto source = argument.rfind("rtsp://", 0) == 0
                            ? imvideo::Source::rtsp(argv[1])
                            : (argument.find("://") != std::string_view::npos ? imvideo::Source::url(argv[1])
                                                                              : imvideo::Source::file(argv[1]));
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

        if (ImGui::Button(player.state() == imvideo::State::Playing ? "Pause" : "Play")) {
            player.state() == imvideo::State::Playing ? player.pause() : player.play();
        }
        ImGui::SameLine();
        static constexpr double rates[] = {0.5, 1.0, 1.5, 2.0};
        static constexpr const char* labels[] = {"0.5x", "1.0x", "1.5x", "2.0x"};
        int selected = 0;
        const double current_rate = player.speed();
        for (int index = 1; index < 4; ++index)
            if (std::abs(rates[index] - current_rate) < std::abs(rates[selected] - current_rate)) selected = index;
        const bool rate_supported = player.can_set_speed();
        ImGui::BeginDisabled(!rate_supported);
        ImGui::SetNextItemWidth(90.0F);
        if (ImGui::Combo("Speed", &selected, labels, 4)) player.set_speed(rates[selected]);
        ImGui::EndDisabled();
        if (!rate_supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Playback speed is unavailable for live or non-seekable input");

        float volume = player.volume();
        if (ImGui::SliderFloat("Volume", &volume, 0.0F, 1.0F)) player.set_volume(volume);
        if (player.seekable()) {
            float position = static_cast<float>(player.position());
            const float duration = static_cast<float>(player.duration());
            if (ImGui::SliderFloat("Position", &position, 0.0F, duration, "%.1f s")) player.seek(position);
        }

        if (renderer.texture() != 0) {
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const float aspect =
                renderer.height() > 0 ? static_cast<float>(renderer.width()) / renderer.height() : 1.0F;
            const float width = std::min(available.x, std::max(0.0F, available.y) * aspect);
            const auto texture = (ImTextureID)(intptr_t)renderer.texture();
            if (width > 0.0F) ImGui::Image(texture, {width, width / aspect});
        } else if (player.state() == imvideo::State::Error) {
            ImGui::TextWrapped("%s", player.error().data());
        } else {
            ImGui::TextUnformatted("Opening video...");
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
