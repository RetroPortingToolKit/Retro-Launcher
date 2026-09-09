#pragma once

#include "retcomm/app_state.hpp"
#include "retcomm/catalog.hpp"
#include "retcomm/paths.hpp"

#include <array>
#include <filesystem>
#include <string>

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
    bool fullscreen = false;   // Fullscreen = 0/1
    int display_aspect = 0;    // 0 "4:3", 1 "8:7", 2 "1:1" (DisplayAspect)
    int output_method = 0;     // 0 SDL, 1 SDL-Software, 2 OpenGL (OutputMethod)
    bool linear_filtering = false;
    bool new_renderer = true;
    bool no_sprite_limits = true;
    // Widescreen and Shader are deliberately NOT here: both are per-game
    // (Metal Warriors stores a column count, not a flag), so the global merge
    // leaves them exactly as the title shipped them.

    // --- Audio ([Sound]) ---
    bool enable_audio = true;
    int audio_freq = 32040;    // 32040 = the SPC's native rate

    // --- Input ([GamepadMap] + [Controller]) ---
    static constexpr int kMaxPlayers = 2;
    std::array<bool, kMaxPlayers> enable_gamepad{{true, true}};
    int gamepad_deadzone = 10000;  // raw axis units 1..32767 (UI shows percent)
    // Per-seat device, the way recomp-ui's launcher writes [Controller]:
    // SourcePn 0 none / 1 keyboard / 2 gamepad, GuidPn when src == 2.
    std::array<int, kMaxPlayers> player_src{{1, 0}};
    std::array<std::string, kMaxPlayers> player_guid{};

    // --- Gamepad button map ([GamepadMap] Controls / ControlsP2) ---
    // Twelve SNES buttons in the runner's own order (kButtonOrder below), each
    // holding one runner token ("DpadUp", "Back", "Lb", ...). The runner has
    // no per-device identity for these yet, so the map is per seat.
    static constexpr int kButtonCount = 12;
    std::array<std::array<std::string, kButtonCount>, kMaxPlayers> pad_controls{};

    // --- Keyboard binds (keybinds.ini [playerN]) --- SDL scancodes, 0 = None.
    std::array<std::array<int, kButtonCount>, kMaxPlayers> kb_scancode{};

    // --- Hotkeys (config.ini [KeyMap]) ---
    static constexpr int kHotkeyCount = 12;
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
