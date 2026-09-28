#pragma once

// A core running inside the hub's window (Retro-Runtime docs/HOST_LIFECYCLE.md, "Running"):
// the picture drawn behind everything, the quick menu as an overlay, input
// from the hub's own gamepads, audio through SDL. The core itself runs in
// retro-core-runner, reached through corelink::CoreLink.
//
// The play overlay -- FPS, TURBO, the volume meter, toasts, the save-state
// browser -- is Retro-Runtime's retro_overlay (docs/OVERLAY.md), so it looks
// the same whichever core is running. This class feeds it events and draws
// its images; it draws none of its own.
//
// Hotkeys while playing, as bound on the settings page (PlayPrefs::hotkeys,
// play.ini [keys] / [combos]); the defaults:
//   Esc / F1      pause menu     Function + Start   (also L3 + R3)
//   F7            save states    Function + R1
//   F3            show FPS       Function + L1
//   Tab (held)    turbo          Function + R2 (held)
//   = / -         volume         Function + D-Up / D-Down (keypad +/- too)
// Function is a per-seat pad input (the Configure page; Back / Select by
// default); while it is held that seat sends the game no buttons.

#include "core_link.hpp" // Retro-Runtime: retro_corelink
#include "hub/hub_core_settings.hpp"
#include "overlay.hpp"   // Retro-Runtime: retro_overlay

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace retcomm::hub {

namespace fs = std::filesystem;
namespace corelink = ::retro::corelink;

struct PlayArgs {
    fs::path core;      // <title>_core.so, its .rcore.toml beside it
    // A GAME_PACKAGE core's game shim (<slug>_game.so, the title's generated
    // code; Retro-Runtime docs/CORE_ABI.md): runner --package. Empty for any
    // other core.
    fs::path package;
    std::string rom;
    fs::path title_dir; // empty = the package's directory, else the core's
    // Seat N's Transfer Pak (index N-1): the Game Boy cartridge (runner
    // --tpakN-rom) and its battery save (region tpakN). Empty = no pak.
    std::array<std::string, 4> tpak_rom;
    std::array<fs::path, 4> tpak_save;
    std::map<std::string, std::string> options;
    bool gl = true;
    // Extra NAME=value for the runner, which passes its environment on to the
    // core: title-app mode sets RETRO_TITLE_STATE_DIR=<data dir> here.
    std::vector<std::string> env;
    // Which device each seat reads and what its inputs drive
    // (hub_core_settings.hpp): the platform's saved seats, or the defaults.
    PlatformInput input;
    // The overlay's settings, and where the hotkeys write them back (empty =
    // not persisted).
    PlayPrefs prefs;
    fs::path data_dir;
};

class PlaySession {
public:
    PlaySession() = default;
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;
    ~PlaySession();

    // runner: the retro-core-runner binary. session_dir: logs and the core's
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
    void fill_pads(rcore_pad pads[RCORE_MAX_SEATS]);
    void grant(std::uint64_t now);
    void grant_if_due();
    bool audio_queue_full() const;
    void await_late_frame();
    void run_turbo();
    void set_turbo_running(bool on);
    void note_frames(std::uint64_t now);
    void pump_audio();
    void upload_frame();
    void set_paused(bool paused);
    bool paused() const { return menu_open_ || states_.is_open(); }
    void sync_pause();
    void set_volume(int percent);
    void set_show_fps(bool on);
    void save_prefs();
    void open_states();
    bool handle_states_event(const SDL_Event& e);
    void poll_states_pad(std::uint64_t now);
    void poll_host_combos();
    std::string shortcut_text(HostAction a) const; // "F7 or Function+R1"
    void service_states();
    void draw_overlay();
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

    // The overlay (Retro-Runtime retro_overlay) and one texture per layer.
    retro::overlay::Osd osd_;
    retro::overlay::SavestateMenu states_;
    retro::overlay::InputGuard guard_;
    struct LayerTex {
        unsigned int tex = 0;
        std::uint64_t rev = 0;
        std::uint32_t w = 0, h = 0;
    };
    std::map<std::uint32_t, LayerTex> layer_tex_;
    fs::path states_dir_;
    bool states_configured_ = false;
    bool states_were_open_ = false;
    retro::overlay::SavestateMenu::Request state_request_; // waiting for a gap between frames

    PlayPrefs prefs_;
    bool turbo_ = false;          // turbo in effect this frame
    bool turbo_key_ = false;      // its key held
    bool turbo_pad_ = false;      // its Function combo held
    bool turbo_running_ = false;  // what tick() last put in force
    std::optional<int> saved_swap_interval_; // vsync to put back when turbo ends
    std::array<bool, kHostActionCount> combo_prev_{}; // last frame's combos
    bool l3r3_prev_ = false;
    bool audio_paused_ = true;    // the SDL stream's state, as last set
    bool was_paused_ = false;
    std::uint64_t noted_frames_ = 0, noted_ns_ = 0;
};

} // namespace retcomm::hub
