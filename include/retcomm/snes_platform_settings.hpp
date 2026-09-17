#pragma once

#include "retcomm/app_state.hpp"
#include "retcomm/catalog.hpp"
#include "retcomm/paths.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

inline bool is_snes_platform(const std::string& platform) {
    return platform == "snes" || platform == "sfc" || platform == "supernintendo";
}

// Global Retro Super Nintendo prefs (Display / Audio / Input / Hotkeys).
// Persisted under data_dir/platform/snes/{config.ini,keybinds.ini}, in the
// exact vocabulary the snesrecomp runner reads next to its exe
// (runner/src/desktop/mmx_config.c + runner/src/keybinds.c), so a merged
// file is indistinguishable from one the game's own launcher wrote.
struct SnesPlatformSettings {
    // --- Display ([Graphics]) ---
    int window_scale = 3;      // 1..6 (WindowScale)
    // [Graphics] Fullscreen is tri-state, not a flag: snesrecomp reads it with
    // strtol into a uint8 and host_main.c branches on 1 (borderless desktop)
    // and 2 (exclusive). Modelling it as a bool made Exclusive unreachable.
    // 0 windowed, 1 borderless, 2 exclusive.
    int fullscreen = 0;
    int display_aspect = 0;    // 0 "4:3", 1 "8:7", 2 "1:1" (DisplayAspect)
    int output_method = 0;     // 0 SDL, 1 SDL-Software, 2 OpenGL (OutputMethod)
    // config.ini [Graphics] Renderer. snesrecomp's own vocabulary
    // (runner/src/desktop/host_main.c): "auto" = SDL's pick, "opengl" = the
    // framework's native GL presenter, "software", or an SDL render driver id
    // such as "vulkan" / "direct3d11". Empty follows the older OutputMethod
    // key, which is how an existing config keeps its presenter. Renderer is
    // the newer control; output_method below is kept in step with it so an
    // older snesrecomp build still lands on the same presenter.
    std::string renderer;
    bool linear_filtering = false;
    bool new_renderer = true;
    bool no_sprite_limits = true;
    // [Graphics] FrameBlend: average each presented frame with the previous one,
    // so alternate-frame flicker reads as translucency the way a CRT showed it.
    bool frame_blend = false;
    // [Graphics] VSync (capital S is the canonical spelling; "Vsync" is only
    // accepted in the legacy [Video]/[Emulation] sections). Driver vsync at
    // present time; snesrecomp defaults it on.
    bool vsync = true;
    // [Graphics] IgnoreAspectRatio: stretch to the window instead of pillarboxing.
    bool ignore_aspect_ratio = false;
    // Widescreen and Shader are deliberately NOT here: both are per-game
    // (Metal Warriors stores a column count, not a flag), so the global merge
    // leaves them exactly as the title shipped them.

    // --- General ([General]) ---
    // RunAhead: frames of local input-latency hiding (0 = off). Each frame
    // costs a whole extra emulated frame per presented frame.
    int run_ahead = 0;
    bool autosave = true;              // [General] Autosave
    bool display_perf_title = false;   // [General] DisplayPerfInTitle

    // --- Local rewind ---
    // snesrecomp has no config.ini key for this: snes_rewind.c reads only
    // SNESRECOMP_REWIND from the environment, and defaults rewind ON. So this
    // is a Retro preference, not a runner key — it is kept in Retro's own
    // global config.ini ([General] EnableRewind) and turned into
    // SNESRECOMP_REWIND=0 in the child's environment at launch. It is
    // deliberately left out of the per-game merge: writing a key the runner
    // does not read into a game's config.ini would look like a setting and do
    // nothing. Consequence: it applies to launches through Retro, not to the
    // game's exe started by hand.
    bool rewind_enabled = true;

    // --- Audio ([Sound]) ---
    bool enable_audio = true;
    int audio_freq = 32040;    // 32040 = the SPC's native rate
    int volume = 100;          // [Sound] Volume, 0..100

    // --- Input ([GamepadMap] + [Controller]) ---
    // Five seats: snesrecomp's core models the Super Multitap (HUD-101) and its
    // joypad.h calls out the 5-player layout by name — port 1 plus a tap on
    // port 2 gives pads 0..4. Seats 3-5 extend snesrecomp's own key scheme
    // (EnableGamepad<N> / ControlsP<N> / SourceP<N> / keybinds [playerN]).
    //
    // NOTE (2026-09-17): snesrecomp's *desktop host* still reads only seats 1-2
    // — runner/src/desktop/host_main.c holds GamepadInfo g_gamepad[2] and
    // mmx_config.c parses only EnableGamepad1/EnableGamepad2 and
    // Controls/ControlsP2. The keys below are written for seats 3-5 and are
    // inert until that host grows multitap input. The settings page says so
    // rather than presenting five seats that all appear to work.
    static constexpr int kMaxPlayers = 5;
    // Read from an existing config.ini but no longer authored: the settings page
    // has no toggle for it, and write_ini_body derives EnableGamepad<n> from
    // player_src (gamepad seat -> true, None/Keyboard -> false). Kept so parsing
    // an existing file stays symmetric with writing one.
    std::array<bool, kMaxPlayers> enable_gamepad{{true, true, false, false, false}};
    int gamepad_deadzone = 10000;  // raw axis units 1..32767 (UI shows percent)
    // Per-seat device, the way recomp-ui's launcher writes [Controller]:
    // SourcePn 0 none / 1 keyboard / 2 gamepad, GuidPn when src == 2.
    std::array<int, kMaxPlayers> player_src{{1, 0, 0, 0, 0}};
    std::array<std::string, kMaxPlayers> player_guid{};
    // [Controller] RewindGesture: pad buttons joined with '+', e.g. "Select+R3"
    // (the default when empty), or "none". The pad-side twin of the Rewind
    // hotkey, the same way the PlayStation page carries pad chords.
    std::string rewind_gesture;
    // [Controller] SaveStateMenuGesture, same spelling. Empty = the runner's
    // Select+R default; "none" disables the pad route.
    std::string savestate_menu_gesture;

    // --- Gamepad button map ([GamepadMap] Controls / ControlsP2) ---
    // Twelve SNES buttons in the runner's own order (kButtonOrder below), each
    // holding one runner token ("DpadUp", "Back", "Lb", ...). The runner has
    // no per-device identity for these yet, so the map is per seat.
    static constexpr int kButtonCount = 12;
    std::array<std::array<std::string, kButtonCount>, kMaxPlayers> pad_controls{};

    // --- Keyboard binds (keybinds.ini [playerN]) --- SDL scancodes, 0 = None.
    std::array<std::array<int, kButtonCount>, kMaxPlayers> kb_scancode{};

    // --- Hotkeys (config.ini [KeyMap]) ---
    // 15: snesrecomp's kKeys_ table gained SaveStateMenu, Rewind and Screenshot
    // after this list was first written, and they were appended there (not
    // inserted) so the Load/Save slot ranges kept their numbering.
    static constexpr int kHotkeyCount = 15;
    std::array<std::string, kHotkeyCount> hotkeys{};

    // Button vocabulary (index = runner Controls order:
    // Up Down Left Right Select Start A B X Y L R).
    static const char* button_label(int b);      // "Up", "Select", "A", ...
    static const char* button_ini_key(int b);    // keybinds.ini key: "up", "select", "a"
    static int button_default_scancode(int b);
    static const char* button_default_pad_token(int b);

    // Gamepad token vocabulary accepted by the runner's Controls= parser.
    static int pad_token_count();
    static const char* pad_token(int i);

    static const char* hotkey_ini_key(int i);
    static const char* hotkey_label(int i);
    static const char* hotkey_default(int i);

    void apply_defaults_if_unset();
    // Restore Display / Audio / Hotkeys / gamepad maps / keyboard binds to
    // defaults. Leaves per-seat device assignments alone.
    void reset_system_to_defaults();
};

// Named controller presets for a seat, kept in platform/snes/pad_profiles.ini.
//
// This is Retro's own store, not a snesrecomp concept: the runner has no
// per-device profile vocabulary at all — it reads one [GamepadMap] ControlsP<n>
// line per seat. A profile is therefore a saved copy of a seat's twelve pad
// tokens and twelve keyboard scancodes that can be applied to any seat, which
// is what makes "the same pad, seat 3 this time" a one-click job instead of
// twenty-four.
struct SnesPadProfile {
    std::string name;
    std::array<std::string, SnesPlatformSettings::kButtonCount> pad_controls{};
    std::array<int, SnesPlatformSettings::kButtonCount> kb_scancode{};
};

fs::path snes_pad_profiles_path(const Paths& paths);
std::vector<SnesPadProfile> load_snes_pad_profiles(const Paths& paths);
// Insert or replace by name. Returns false only on a write failure.
bool save_snes_pad_profile(const Paths& paths, const SnesPadProfile& profile,
                           std::string* error = nullptr);
bool rename_snes_pad_profile(const Paths& paths, const std::string& from, const std::string& to,
                             std::string* error = nullptr);
bool delete_snes_pad_profile(const Paths& paths, const std::string& name,
                             std::string* error = nullptr);

fs::path snes_platform_settings_dir(const Paths& paths);
fs::path snes_platform_settings_ini_path(const Paths& paths);
fs::path snes_platform_keybinds_ini_path(const Paths& paths);

SnesPlatformSettings load_snes_platform_settings(const Paths& paths);
bool save_snes_platform_settings(const Paths& paths, const SnesPlatformSettings& s,
                                 std::string* error = nullptr);

struct ApplySnesPlatformResult {
    bool ok = true;
    bool skipped = false;
    std::string message;
};

// Merge global prefs into a title install cwd (config.ini upsert + keybinds.ini
// copy). No-op when not SNES, excluded in AppState, or global files are absent.
ApplySnesPlatformResult apply_snes_platform_defaults(const Paths& paths, const AppState& state,
                                                     const Title& title,
                                                     const fs::path& game_cwd);

} // namespace retcomm
