#pragma once

// A core running inside the hub's window (docs/HOST_LIFECYCLE.md, "Running"):
// the picture drawn behind everything, the quick menu as an overlay, input
// from the hub's own gamepads, audio through SDL. The core itself runs in
// retcomm-core-runner, reached through corelink::CoreLink.

#include "../corelink/core_link.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace retcomm::hub {

namespace fs = std::filesystem;

struct PlayArgs {
    fs::path core;      // <title>_core.so, its .rcore.toml beside it
    std::string rom;
    fs::path title_dir; // empty = the core's own directory
    std::string tpak_rom;
    fs::path tpak_save;
    std::map<std::string, std::string> options;
    bool gl = true;
};

class PlaySession {
public:
    PlaySession() = default;
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;
    ~PlaySession();

    // runner: the retcomm-core-runner binary. session_dir: logs and the core's
    // cache. save_dir: where save regions persist.
    bool start(const PlayArgs& args, const fs::path& runner, const fs::path& session_dir,
               const fs::path& save_dir, std::string* error);

    // Every SDL event, before ImGui sees it. Returns true when the event was
    // the session's own (menu toggle) and should not reach ImGui.
    bool handle_event(const SDL_Event& e);
    // Once per hub frame: link messages, pacing and grants, audio, saves.
    void tick();
    // Inside an ImGui frame: the picture behind everything, then whichever
    // overlay applies (loading, quick menu, fault).
    void draw();

    // The player chose to leave (Direct mode then exits the app).
    bool finished() const { return finished_; }
    void shutdown();

private:
    void fill_pads(rcore_pad pads[RCORE_MAX_SEATS]) const;
    void grant_if_due();
    void pump_audio();
    void upload_frame();
    void set_paused(bool paused);
    void draw_menu();
    void draw_fault();
    void draw_loading();

    corelink::CoreLink link_;
    PlayArgs args_;
    bool menu_open_ = false;
    bool finished_ = false;
    bool user_quit_ = false;

    // Picture
    unsigned int tex_ = 0;
    std::uint32_t tex_w_ = 0, tex_h_ = 0;
    std::uint32_t aspect_num_ = 0, aspect_den_ = 0;

    // Audio: an SDL stream fed from the link's ring; its fill level paces
    // grants (the core states no frame rate; its audio clock stands in).
    SDL_AudioStream* audio_ = nullptr;
    std::uint32_t audio_hz_ = 0;
    std::vector<std::int16_t> audio_buf_;

    // Pacing before audio exists: a fixed 60 Hz.
    std::uint64_t next_grant_ns_ = 0;
    std::uint64_t last_persist_ns_ = 0;

    fs::path runner_log_;
    std::vector<std::string> fault_log_; // runner.log's tail, read once
    bool fault_log_loaded_ = false;
};

} // namespace retcomm::hub
