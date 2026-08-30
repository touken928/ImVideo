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

namespace {

enum ResizeEdge : unsigned {
    ResizeNone = 0,
    ResizeLeft = 1U << 0U,
    ResizeRight = 1U << 1U,
    ResizeTop = 1U << 2U,
    ResizeBottom = 1U << 3U,
};

struct WindowChromeState {
    bool dragging = false;
    bool resizing = false;
    bool collapsed = false;
    unsigned resize_edges = ResizeNone;
    int cursor_offset_x = 0;
    int cursor_offset_y = 0;
    int resize_cursor_x = 0;
    int resize_cursor_y = 0;
    int resize_window_x = 0;
    int resize_window_y = 0;
    int resize_width = 1280;
    int resize_height = 720;
    int expanded_width = 1280;
    int expanded_height = 720;
};

void get_screen_cursor_position(GLFWwindow* window, int& screen_x, int& screen_y) {
    int window_x = 0;
    int window_y = 0;
    double cursor_x = 0.0;
    double cursor_y = 0.0;
    glfwGetWindowPos(window, &window_x, &window_y);
    glfwGetCursorPos(window, &cursor_x, &cursor_y);
    screen_x = window_x + static_cast<int>(std::lround(cursor_x));
    screen_y = window_y + static_cast<int>(std::lround(cursor_y));
}

unsigned hovered_resize_edges(const ImVec2& window_position, const ImVec2& window_size) {
    constexpr float grip_size = 6.0F;
    const ImVec2 mouse = ImGui::GetMousePos();
    const ImVec2 window_max = {window_position.x + window_size.x, window_position.y + window_size.y};
    if (!ImGui::IsMouseHoveringRect(window_position, window_max, false)) return ResizeNone;

    unsigned edges = ResizeNone;
    if (mouse.x < window_position.x + grip_size) edges |= ResizeLeft;
    if (mouse.x >= window_max.x - grip_size) edges |= ResizeRight;
    if (mouse.y < window_position.y + grip_size) edges |= ResizeTop;
    if (mouse.y >= window_max.y - grip_size) edges |= ResizeBottom;
    return edges;
}

void set_resize_cursor(unsigned edges) {
    if (((edges & ResizeLeft) && (edges & ResizeTop)) || ((edges & ResizeRight) && (edges & ResizeBottom))) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    } else if (((edges & ResizeRight) && (edges & ResizeTop)) || ((edges & ResizeLeft) && (edges & ResizeBottom))) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
    } else if (edges & (ResizeLeft | ResizeRight)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    } else if (edges & (ResizeTop | ResizeBottom)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
}

bool update_window_resize(GLFWwindow* window, WindowChromeState& state, const ImVec2& window_position,
                          const ImVec2& window_size) {
    constexpr int minimum_width = 480;
    constexpr int minimum_height = 270;
    const unsigned hovered_edges = hovered_resize_edges(window_position, window_size);
    const unsigned cursor_edges = state.resizing ? state.resize_edges : hovered_edges;
    if (cursor_edges != ResizeNone) set_resize_cursor(cursor_edges);

    if (!state.resizing && hovered_edges != ResizeNone && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.dragging = false;
        state.resizing = true;
        state.resize_edges = hovered_edges;
        get_screen_cursor_position(window, state.resize_cursor_x, state.resize_cursor_y);
        glfwGetWindowPos(window, &state.resize_window_x, &state.resize_window_y);
        glfwGetWindowSize(window, &state.resize_width, &state.resize_height);
    }
    if (!state.resizing) return hovered_edges != ResizeNone;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        state.resizing = false;
        state.resize_edges = ResizeNone;
        return hovered_edges != ResizeNone;
    }

    int cursor_x = 0;
    int cursor_y = 0;
    get_screen_cursor_position(window, cursor_x, cursor_y);
    const int delta_x = cursor_x - state.resize_cursor_x;
    const int delta_y = cursor_y - state.resize_cursor_y;
    int new_x = state.resize_window_x;
    int new_y = state.resize_window_y;
    int new_width = state.resize_width;
    int new_height = state.resize_height;

    if (state.resize_edges & ResizeLeft) {
        new_width = std::max(minimum_width, state.resize_width - delta_x);
        new_x = state.resize_window_x + state.resize_width - new_width;
    } else if (state.resize_edges & ResizeRight) {
        new_width = std::max(minimum_width, state.resize_width + delta_x);
    }
    if (state.resize_edges & ResizeTop) {
        new_height = std::max(minimum_height, state.resize_height - delta_y);
        new_y = state.resize_window_y + state.resize_height - new_height;
    } else if (state.resize_edges & ResizeBottom) {
        new_height = std::max(minimum_height, state.resize_height + delta_y);
    }

    glfwSetWindowSize(window, new_width, new_height);
    if (new_x != state.resize_window_x || new_y != state.resize_window_y) glfwSetWindowPos(window, new_x, new_y);
    state.expanded_width = new_width;
    state.expanded_height = new_height;
    return true;
}

void update_window_chrome(GLFWwindow* window, WindowChromeState& state) {
    const ImVec2 window_position = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    const float title_bar_height = ImGui::GetFrameHeight();

    const bool collapsed = ImGui::IsWindowCollapsed();
    if (collapsed != state.collapsed) {
        if (collapsed) {
            glfwGetWindowSize(window, &state.expanded_width, &state.expanded_height);
            glfwSetWindowSize(window, state.expanded_width, static_cast<int>(std::ceil(title_bar_height)));
        } else {
            glfwSetWindowSize(window, state.expanded_width, state.expanded_height);
        }
        state.collapsed = collapsed;
    }

    const bool resize_region_active = !collapsed && update_window_resize(window, state, window_position, window_size);
    const ImVec2 drag_min = {
        window_position.x + title_bar_height,
        window_position.y,
    };
    const ImVec2 drag_max = {
        window_position.x + window_size.x - title_bar_height,
        window_position.y + title_bar_height,
    };
    const bool drag_region_hovered = ImGui::IsMouseHoveringRect(drag_min, drag_max, false);
    if (!resize_region_active && drag_region_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        double cursor_x = 0.0;
        double cursor_y = 0.0;
        glfwGetCursorPos(window, &cursor_x, &cursor_y);
        state.cursor_offset_x = static_cast<int>(std::lround(cursor_x));
        state.cursor_offset_y = static_cast<int>(std::lround(cursor_y));
        state.dragging = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) state.dragging = false;
    if (!state.dragging) return;

    int window_x = 0;
    int window_y = 0;
    double cursor_x = 0.0;
    double cursor_y = 0.0;
    glfwGetWindowPos(window, &window_x, &window_y);
    glfwGetCursorPos(window, &cursor_x, &cursor_y);
    glfwSetWindowPos(window, window_x + static_cast<int>(std::lround(cursor_x)) - state.cursor_offset_x,
                     window_y + static_cast<int>(std::lround(cursor_y)) - state.cursor_offset_y);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: implayer <file-or-url>\n");
        return 2;
    }
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
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
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetStyle().WindowBorderSize = 0.0F;
    ImGui::GetStyle().WindowRounding = 0.0F;
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

    bool player_window_open = true;
    WindowChromeState window_chrome;
    while (player_window_open && !glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            glfwWaitEvents();
            continue;
        }
        if (const auto frame = player.frame()) renderer.update(frame);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        constexpr ImGuiWindowFlags window_flags =
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
        const bool show_player = ImGui::Begin("implayer", &player_window_open, window_flags);
        update_window_chrome(window, window_chrome);

        if (show_player) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.0F, 4.0F});
            if (ImGui::Button(player.state() == imvideo::State::Playing ? "Pause" : "Play", {54.0F, 0.0F})) {
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
            ImGui::SetNextItemWidth(64.0F);
            if (ImGui::Combo("##speed", &selected, labels, 4)) player.set_speed(rates[selected]);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(rate_supported ? "Playback speed"
                                                 : "Playback speed is unavailable for live or non-seekable input");
            }

            ImGui::SameLine();
            float volume_percent = player.volume() * 100.0F;
            ImGui::SetNextItemWidth(96.0F);
            if (ImGui::SliderFloat("##volume", &volume_percent, 0.0F, 100.0F, "Vol %.0f%%"))
                player.set_volume(volume_percent / 100.0F);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Volume");

            if (player.seekable()) {
                ImGui::SameLine();
                float position = static_cast<float>(player.position());
                const float duration = static_cast<float>(player.duration());
                char position_format[48]{};
                std::snprintf(position_format, sizeof(position_format), "%%.1f / %.1f s", duration);
                ImGui::SetNextItemWidth(-1.0F);
                if (ImGui::SliderFloat("##position", &position, 0.0F, duration, position_format)) player.seek(position);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback position");
            }
            ImGui::PopStyleVar();

            if (renderer.texture() != 0) {
                const ImVec2 available = ImGui::GetContentRegionAvail();
                const float aspect = renderer.width() > 0 && renderer.height() > 0
                                         ? static_cast<float>(renderer.width()) / renderer.height()
                                         : 1.0F;
                const float width = std::min(std::max(0.0F, available.x), std::max(0.0F, available.y) * aspect);
                const float height = width / aspect;
                const auto texture = (ImTextureID)(intptr_t)renderer.texture();
                if (width > 0.0F && height > 0.0F) {
                    const ImVec2 origin = ImGui::GetCursorScreenPos();
                    ImGui::SetCursorScreenPos(
                        {origin.x + (available.x - width) * 0.5F, origin.y + (available.y - height) * 0.5F});
                    ImGui::Image(texture, {width, height});
                }
            } else if (player.state() == imvideo::State::Error) {
                ImGui::TextWrapped("%s", player.error().data());
            } else {
                ImGui::TextUnformatted("Opening video...");
            }
        }
        ImGui::End();
        if (!player_window_open) glfwSetWindowShouldClose(window, GLFW_TRUE);

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
