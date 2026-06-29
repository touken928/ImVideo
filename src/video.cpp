// -----------------------------------------------------------------------
// video.cpp  —  Free functions Image() / Video()
// -----------------------------------------------------------------------
#include "player_impl.hpp"

#include <imgui.h>
#include <cmath>

// -----------------------------------------------------------------------
namespace imvideo {
// -----------------------------------------------------------------------

void Image(Player& player, ImVec2 size) {
    auto* impl = player.pimpl_.get();
    if (!impl) return;

    ImTextureID tex = impl->texture();
    if (!tex) return;

    ImVec2 vs = impl->video_size();
    if (vs.x <= 0.f || vs.y <= 0.f) return;

    if (size.x <= 0.f || size.y <= 0.f) {
        size = vs;
    } else {
        float scale = std::min(size.x / vs.x, size.y / vs.y);
        size = ImVec2(vs.x * scale, vs.y * scale);
    }

    ImGui::Image(tex, size);
}

bool Video(Player& player, ImVec2 size) {
    auto* impl = player.pimpl_.get();
    if (!impl) return false;

    bool keep_open = true;

    // --- video image -----------------------------------------------------
    Image(player, size);

    // --- controls --------------------------------------------------------
    ImGui::BeginDisabled(!impl->is_playing() && !impl->opened_);

    // Play / Pause / Stop
    if (impl->playing_ && !impl->paused_) {
        if (ImGui::Button("|| Pause"))  player.Pause();
    } else {
        if (ImGui::Button("|> Play"))   player.Play();
    }
    ImGui::SameLine();
    if (ImGui::Button("[] Stop")) {
        player.Stop();
        keep_open = false;
    }

    // Timeline
    double pos = impl->position();
    double dur = impl->duration();
    float  pos_f = static_cast<float>(dur > 0.0 ? pos / dur : 0.0);
    ImGui::PushItemWidth(-1);
    if (ImGui::SliderFloat("##seek", &pos_f, 0.0f, 1.0f, "%.3f")) {
        player.Seek(pos_f * dur);
    }
    ImGui::PopItemWidth();

    // Time display
    {
        char pb[16], db[16];
        auto fmt = [](double s, char* buf, size_t sz) {
            if (s < 0) s = 0;
            int h = static_cast<int>(s / 3600);
            int m = static_cast<int>(std::fmod(s, 3600) / 60);
            int sec = static_cast<int>(std::fmod(s, 60));
            if (h > 0)
                snprintf(buf, sz, "%d:%02d:%02d", h, m, sec);
            else
                snprintf(buf, sz, "%d:%02d", m, sec);
        };
        fmt(pos, pb, sizeof(pb));
        fmt(dur, db, sizeof(db));
        ImGui::Text("%s / %s", pb, db);
    }

    ImGui::EndDisabled();

    // Volume & speed
    bool has_audio = impl->has_audio();
    float vol = impl->volume_;
    if (ImGui::SliderFloat("Volume", &vol, 0.0f, 1.0f, "%.2f")) {
        player.SetVolume(vol);
    }

    if (has_audio) {
        // With audio, speed is locked to 1.0 in v2.
        float spd = 1.0f;
        ImGui::BeginDisabled(true);
        ImGui::SliderFloat("Speed", &spd, 0.0625f, 4.0f, "1.00 (audio locked)");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Speed control is disabled when audio is active. "
                              "Audio time-stretch is not yet implemented.");
    } else {
        float spd = impl->speed_;
        if (ImGui::SliderFloat("Speed", &spd, 0.0625f, 4.0f, "%.2f")) {
            player.SetSpeed(spd);
        }
    }

    return keep_open;
}

// -----------------------------------------------------------------------
} // namespace imvideo
// -----------------------------------------------------------------------
