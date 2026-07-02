#pragma once

#include <imvideo/imvideo.h>

#include <array>
#include <string>

namespace implayer {

// -----------------------------------------------------------------------
// App  —  owns player state and renders the full ImPlayer UI.
// Layout:
//   Top:    native ImGui menu bar (Open…, About)
//   Center: video viewport (auto-scale, centered)
//   Bottom: transport controls, seek bar, volume, time
// -----------------------------------------------------------------------
class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// Open a single video file (closes any previous).
    void OpenFile(const std::string& path);

    /// Open a local path or supported URL.
    void OpenSource(const std::string& source);

    /// Called once per frame from the main loop (GL context active).
    void OnFrame();

    /// Access the player for external callbacks (e.g., drag-drop).
    imvideo::Player& Player() { return player_; }

private:
    void Toolbar();       // top menu bar
    void VideoArea();     // centered video viewport
    void Controls();      // bottom transport bar
    void EmptyState();    // centered placeholder when no file loaded
    void AboutDialog();   // about modal
    void OpenUrlPopup();  // lightweight URL input popup

    imvideo::Player player_;
    bool  file_open_       = false;
    bool  show_about_      = false;
    bool  show_open_url_   = false;
    float volume_          = 1.0f;
    std::array<char, 1024> source_input_{};
};

} // namespace implayer
