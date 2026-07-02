#include "app.hpp"

#include <imgui.h>
#include <tinyfiledialogs.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <vector>

namespace implayer {

// ========================================================================
// Construction
// ========================================================================

App::App() = default;
App::~App() = default;

// ========================================================================
// Open file
// ========================================================================

void App::OpenFile(const std::string& path) {
    OpenSource(path);
}

void App::OpenSource(const std::string& source) {
    player_.Close();
    if (player_.Open(std::string_view(source))) {
        player_.Play();
        file_open_ = true;
    } else {
        file_open_ = false;
    }
}

// ========================================================================
// OnFrame  —  called once per frame
// ========================================================================

void App::OnFrame() {
    // ---- Full-viewport background window --------------------------------
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking
                           | ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_MenuBar
                           | ImGuiWindowFlags_NoBringToFrontOnFocus
                           | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("ImPlayer", nullptr, flags);
    ImGui::PopStyleVar(2);

    Toolbar();

    // ---- Content: video (fills space) + bottom controls -----------------
    if (file_open_) {
        const float controls_h = 50.0f;

        ImGui::BeginChild("VideoArea", ImVec2(0, -controls_h), false,
                          ImGuiWindowFlags_NoScrollbar);
        VideoArea();
        ImGui::EndChild();

        Controls();
    } else {
        EmptyState();
    }

    ImGui::End();

    OpenUrlPopup();

    if (show_about_)
        AboutDialog();
}

// ========================================================================
// Toolbar  —  native ImGui menu bar with inline buttons
// ========================================================================

void App::Toolbar() {
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::MenuItem("  Open...  ", "Cmd+O")) {
        const char* filter[] = { "*.mp4", "*.mkv", "*.avi", "*.mov",
                                 "*.webm", "*.wmv", "*.flv", "*.m4v" };
        const char* path = tinyfd_openFileDialog("Open Video File", "", 8,
                                                 filter, "Video Files", 0);
        if (path)
            OpenFile(path);
    }

    ImGui::SameLine();

    if (ImGui::MenuItem("  Open URL...  ")) {
        show_open_url_ = true;
        ImGui::OpenPopup("Open URL");
    }

    ImGui::SameLine();

    if (ImGui::MenuItem("  About  "))
        show_about_ = true;

    ImGui::EndMenuBar();
}

// ========================================================================
// VideoArea  —  centered video viewport with auto-scale
// ========================================================================

void App::VideoArea() {
    player_.Update();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x <= 0 || avail.y <= 0) return;

    ImVec2 vs = player_.VideoSize();
    if (vs.x <= 0 || vs.y <= 0) {
        // No valid video frame yet — show nothing
        return;
    }

    // Calculate scaled size that fits within avail while keeping ratio
    float scale = std::min(avail.x / vs.x, avail.y / vs.y);
    ImVec2 render_size(vs.x * scale, vs.y * scale);

    // Center in available area
    float offset_x = (avail.x - render_size.x) * 0.5f;
    float offset_y = (avail.y - render_size.y) * 0.5f;
    ImGui::SetCursorPos(ImVec2(
        ImGui::GetCursorPosX() + offset_x,
        ImGui::GetCursorPosY() + offset_y));

    ImGui::Image(player_.Texture(), render_size);
}

// ========================================================================
// Controls  —  bottom transport bar
// ========================================================================

void App::Controls() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 2));

    // ---- Play / Pause / Stop --------------------------------------------
    if (player_.IsPlaying()) {
        if (ImGui::Button("  || Pause  "))
            player_.Pause();
    } else {
        if (ImGui::Button("  |> Play  "))
            player_.Play();
    }
    ImGui::SameLine();
    if (ImGui::Button("  [] Stop  "))
        player_.Stop();

    // ---- Volume ---------------------------------------------------------
    ImGui::SameLine(0, 16);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderFloat("##vol", &volume_, 0.0f, 1.0f, "Vol: %.2f"))
        player_.SetVolume(volume_);

    // ---- Seek bar + time display ----------------------------------------
    double pos  = player_.Position();
    double dur  = player_.Duration();
    float  pos_f = static_cast<float>(dur > 0.0 ? pos / dur : 0.0);

    // Format time
    auto fmt = [](double s, char* buf, size_t sz) {
        if (s < 0) s = 0;
        int h = static_cast<int>(s / 3600);
        int m = static_cast<int>(std::fmod(s, 3600) / 60);
        int sec = static_cast<int>(std::fmod(s, 60));
        if (h > 0) std::snprintf(buf, sz, "%d:%02d:%02d", h, m, sec);
        else       std::snprintf(buf, sz, "%d:%02d", m, sec);
    };

    char pb[16], db[16];
    fmt(pos, pb, sizeof(pb));
    fmt(dur, db, sizeof(db));

    float time_w  = ImGui::CalcTextSize("00:00 / 00:00").x + 12;
    float seek_w  = ImGui::GetContentRegionAvail().x - time_w - 12;

    if (seek_w > 40.0f) {
        ImGui::SetNextItemWidth(seek_w);
        if (ImGui::SliderFloat("##seek", &pos_f, 0.0f, 1.0f, ""))
            player_.Seek(static_cast<double>(pos_f) * dur);
        ImGui::SameLine();
    }
    ImGui::Text("%s / %s", pb, db);

    ImGui::PopStyleVar(2);
}

// ========================================================================
// OpenUrlPopup  —  modal URL / source input
// ========================================================================

void App::OpenUrlPopup() {
    if (show_open_url_)
        ImGui::OpenPopup("Open URL");

    ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Open URL", &show_open_url_, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextUnformatted("Open a local path, file:// URL, or network URL.");
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::SetNextItemWidth(560.0f);
    ImGui::InputText("##source", source_input_.data(), source_input_.size());

    const bool submitted = ImGui::IsItemDeactivatedAfterEdit()
                        && ImGui::IsKeyPressed(ImGuiKey_Enter, false);

    if (submitted || ImGui::Button("Open")) {
        OpenSource(source_input_.data());
        if (file_open_) {
            show_open_url_ = false;
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        show_open_url_ = false;
        ImGui::CloseCurrentPopup();
    }

    const char* err = player_.LastError();
    if (err && err[0] != '\0') {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextWrapped("Error: %s", err);
    }

    ImGui::EndPopup();
}

// ========================================================================
// EmptyState  —  centered placeholder when no video is loaded
// ========================================================================

void App::EmptyState() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const char* msg = "Drop a video file or use Open / Open URL";
    ImVec2 ts = ImGui::CalcTextSize(msg);
    const char* err = player_.LastError();
    const bool has_error = err && err[0] != '\0';
    const float block_h = ts.y + (has_error ? ImGui::GetTextLineHeightWithSpacing() * 2.0f : 0.0f);
    ImGui::SetCursorPos(ImVec2(
        (avail.x - ts.x) * 0.5f,
        (avail.y - block_h) * 0.5f));
    ImGui::TextDisabled("%s", msg);
    if (has_error) {
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::TextWrapped("Last error: %s", err);
    }
}

// ========================================================================
// AboutDialog  -  simple modal
// ========================================================================

void App::AboutDialog() {
    ImGui::SetNextWindowSize(ImVec2(400, 240), ImGuiCond_Once);
    if (!ImGui::Begin("About ImPlayer", &show_about_,
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("ImPlayer - imvideo Demo Player");
    ImGui::Separator();
    ImGui::TextUnformatted("Version 0.1.0");
    ImGui::BulletText("imvideo - Dear ImGui video playback library");
    ImGui::BulletText("FFmpeg - audio/video decoding");
    ImGui::BulletText("miniaudio - audio output");
    ImGui::BulletText("Dear ImGui + GLFW + OpenGL");
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Separator();
    ImGui::TextUnformatted("MIT License");
    ImGui::End();
}

} // namespace implayer
