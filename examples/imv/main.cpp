// -----------------------------------------------------------------------
// imv  —  Single-video player demo built on imvideo
//
// Entry point: init GLFW, ImGui, create App, run main loop.
// UI logic is in app.hpp / app.cpp.
// -----------------------------------------------------------------------
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "app.hpp"
#include "file_dialog.hpp"

#include <cstdio>
#include <string>
#include <vector>

// ---- GLFW callbacks ------------------------------------------------------
static void glfw_error_callback(int error, const char* desc) {
    fprintf(stderr, "[GLFW] Error %d: %s\n", error, desc);
}

static imv::App* g_app = nullptr;
static void drop_callback(GLFWwindow*, int count, const char** paths) {
    if (!g_app || count == 0) return;
    g_app->OpenFile(paths[0]);
}

// ========================================================================
// main
// ========================================================================
int main(int argc, char** argv) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    // ---- GLFW window ----------------------------------------------------
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#   define GL_SILENCE_DEPRECATION
#endif

    GLFWwindow* window = glfwCreateWindow(1280, 800,
                                          "imv — imvideo player",
                                          nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetDropCallback(window, drop_callback);

    // ---- ImGui setup ----------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;
    ImGui::GetStyle().FramePadding   = ImVec2(6, 3);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // ---- Application state ----------------------------------------------
    imv::App app;
    g_app = &app;

    // Load file from command line
    if (argc > 1)
        app.OpenFile(argv[1]);

    // ---- Main loop ------------------------------------------------------
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.OnFrame();

        // Render
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.08f, 0.08f, 0.08f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // ---- Shutdown -------------------------------------------------------
    g_app = nullptr;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
