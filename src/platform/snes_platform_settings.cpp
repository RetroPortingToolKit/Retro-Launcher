#include "retcomm/snes_platform_settings.hpp"
#include "ini_text.hpp"
#include "retcomm/psx_input_profiles.hpp" // sdl_scancode_name / sdl_scancode_from_name

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <system_error>

namespace retcomm {
using namespace ini_text;
namespace {

constexpr int kN = SnesPlatformSettings::kButtonCount;
constexpr int kP = SnesPlatformSettings::kMaxPlayers;

// Runner Controls= order (mmx_config.c kDefaultGamepadCmds / shipped ini line).
const char* kButtonLabels[kN] = {"Up", "Down", "Left",  "Right", "Select", "Start",
                                 "A",  "B",    "X",     "Y",     "L",      "R"};
const char* kButtonIniKeys[kN] = {"up", "down", "left", "right", "select", "start",
                                  "a",  "b",    "x",    "y",     "l",      "r"};
// keybinds.c defaults: a=X b=Z x=S y=A l=C r=V start=Return select=RShift + arrows.
const int kKbDefaults[kN] = {82, 81, 80, 79, 229, 40, 27, 29, 22, 4, 6, 25};
// Shipped "Controls =" line: SNES A is SDL B and vice versa (runner comment).
const char* kPadDefaults[kN] = {"DpadUp", "DpadDown", "DpadLeft", "DpadRight", "Back", "Start",
                                "B",      "A",        "Y",        "X",         "Lb",   "Rb"};
// keybinds.ini is written in the runner's own file order so a diff against a
// runner-generated file is empty: a b x y l r start select up down left right.
const int kKbFileOrder[kN] = {6, 7, 8, 9, 10, 11, 5, 4, 0, 1, 2, 3};

// ParseGamepadButtonName vocabulary (mmx_config.c). Lb/Rb alias L1/R1.
const char* kPadTokens[] = {"DpadUp", "DpadDown", "DpadLeft", "DpadRight", "Back", "Start",
                            "A",      "B",        "X",        "Y",         "Lb",   "Rb",
                            "L1",     "R1",       "L2",       "R2",        "L3",   "R3",
                            "Guide"};

const char* kHotkeyKeys[SnesPlatformSettings::kHotkeyCount] = {
    "Fullscreen",  "Reset",       "Pause",       "PauseDimmed",
    "Turbo",       "WindowBigger", "WindowSmaller", "VolumeUp",
    "VolumeDown",  "DisplayPerf", "ToggleRenderer", "ToggleWidescreen"};
const char* kHotkeyLabels[SnesPlatformSettings::kHotkeyCount] = {
    "Fullscreen",    "Reset",         "Pause",           "Pause (dimmed)",
    "Turbo",         "Window bigger", "Window smaller",  "Volume up",
    "Volume down",   "Display perf",  "Toggle renderer", "Toggle widescreen"};
// Values the framework ships in every port's config.ini [KeyMap].
const char* kHotkeyDefaults[SnesPlatformSettings::kHotkeyCount] = {
    "Alt+Return", "Ctrl+r",    "Shift+p", "p", "Tab", "Ctrl+Up",
    "Ctrl+Down",  "Shift+=",   "Shift+-", "f", "r",   "Alt+w"};

const char* kAspectLit[] = {"4:3", "8:7", "1:1"};
const char* kOutputLit[] = {"SDL", "SDL-Software", "OpenGL"};

int parse_aspect(const std::string& v) {
    if (ieq(v, "8:7") || ieq(v, "SquarePixels") || ieq(v, "1")) return 1;
    if (ieq(v, "1:1") || ieq(v, "SquareFrame") || ieq(v, "2")) return 2;
    return 0; // "4:3" / "CRT" / "0"
}

int parse_output(const std::string& v) {
    if (ieq(v, "SDL-Software")) return 1;
    if (ieq(v, "OpenGL")) return 2;
    return 0;
}

int pad_token_index(const std::string& v) {
    for (int i = 0; i < static_cast<int>(sizeof(kPadTokens) / sizeof(kPadTokens[0])); ++i)
        if (ieq(v, kPadTokens[i])) return i;
    return -1;
}

std::string join_controls(const std::array<std::string, kN>& c) {
    std::string out;
    for (int b = 0; b < kN; ++b) {
        if (b) out += ", ";
        out += c[static_cast<size_t>(b)].empty() ? SnesPlatformSettings::button_default_pad_token(b)
                                                  : c[static_cast<size_t>(b)];
    }
    return out;
}

void split_controls(const std::string& line, std::array<std::string, kN>& out) {
    std::istringstream in(line);
    std::string tok;
    int b = 0;
    while (b < kN && std::getline(in, tok, ',')) {
        tok = trim_copy(tok);
        // Keep only tokens the runner would accept; anything else falls back to
        // the default so the merged line never breaks the runner's parser.
        if (pad_token_index(tok) >= 0) out[static_cast<size_t>(b)] = kPadTokens[pad_token_index(tok)];
        ++b;
    }
}

void parse_ini(const std::string& body, SnesPlatformSettings& s) {
    for_each_ini_pair(body, [&](const std::string& sec, const std::string& key,
                                const std::string& val) {
        bool bv = false;
        if (ieq(sec, "Graphics")) {
            if (ieq(key, "WindowScale")) s.window_scale = std::clamp(std::atoi(val.c_str()), 1, 6);
            else if (ieq(key, "Fullscreen")) s.fullscreen = std::atoi(val.c_str()) != 0;
            else if (ieq(key, "DisplayAspect")) s.display_aspect = parse_aspect(val);
            else if (ieq(key, "OutputMethod")) s.output_method = parse_output(val);
            else if (ieq(key, "LinearFiltering") && parse_bool(val, &bv)) s.linear_filtering = bv;
            else if (ieq(key, "NewRenderer") && parse_bool(val, &bv)) s.new_renderer = bv;
            else if (ieq(key, "NoSpriteLimits") && parse_bool(val, &bv)) s.no_sprite_limits = bv;
        } else if (ieq(sec, "Sound")) {
            if (ieq(key, "EnableAudio") && parse_bool(val, &bv)) s.enable_audio = bv;
            else if (ieq(key, "AudioFreq")) {
                const int f = std::atoi(val.c_str());
                if (f > 0) s.audio_freq = f;
            }
        } else if (ieq(sec, "GamepadMap")) {
            if (ieq(key, "EnableGamepad1") && parse_bool(val, &bv)) s.enable_gamepad[0] = bv;
            else if (ieq(key, "EnableGamepad2") && parse_bool(val, &bv)) s.enable_gamepad[1] = bv;
            else if (ieq(key, "GamepadDeadzone"))
                s.gamepad_deadzone = std::clamp(std::atoi(val.c_str()), 1, 32767);
            else if (ieq(key, "Controls")) split_controls(val, s.pad_controls[0]);
            else if (ieq(key, "ControlsP2")) split_controls(val, s.pad_controls[1]);
        } else if (ieq(sec, "Controller")) {
            for (int p = 0; p < kP; ++p) {
                const std::string src = "SourceP" + std::to_string(p + 1);
                const std::string guid = "GuidP" + std::to_string(p + 1);
                if (ieq(key, src.c_str()))
                    s.player_src[static_cast<size_t>(p)] = std::clamp(std::atoi(val.c_str()), 0, 2);
                else if (ieq(key, guid.c_str()))
                    s.player_guid[static_cast<size_t>(p)] = val;
            }
        } else if (ieq(sec, "KeyMap")) {
            for (int i = 0; i < SnesPlatformSettings::kHotkeyCount; ++i)
                if (ieq(key, kHotkeyKeys[i])) s.hotkeys[static_cast<size_t>(i)] = val;
        }
    });
}

void parse_keybinds(const std::string& body, SnesPlatformSettings& s) {
    for_each_ini_pair(body, [&](const std::string& sec, const std::string& key,
                                const std::string& val) {
        int p = -1;
        if (ieq(sec, "player1")) p = 0;
        else if (ieq(sec, "player2")) p = 1;
        if (p < 0) return;
        for (int b = 0; b < kN; ++b) {
            if (ieq(key, kButtonIniKeys[b])) {
                s.kb_scancode[static_cast<size_t>(p)][static_cast<size_t>(b)] =
                    sdl_scancode_from_name(val.c_str());
                return;
            }
        }
    });
}

// The managed key set. Everything else in the game's config.ini (Widescreen,
// Shader, Load/Save state rows, [Controller.<guid>] profiles, Msu1, Netplay
// name) passes through untouched.
void write_ini_body(std::string& body, const SnesPlatformSettings& s) {
    upsert_ini_key(body, "Graphics", "WindowScale", std::to_string(std::clamp(s.window_scale, 1, 6)));
    upsert_ini_key(body, "Graphics", "Fullscreen", s.fullscreen ? "1" : "0");
    upsert_ini_key(body, "Graphics", "DisplayAspect", kAspectLit[std::clamp(s.display_aspect, 0, 2)]);
    upsert_ini_key(body, "Graphics", "OutputMethod", kOutputLit[std::clamp(s.output_method, 0, 2)]);
    upsert_ini_key(body, "Graphics", "LinearFiltering", s.linear_filtering ? "1" : "0");
    upsert_ini_key(body, "Graphics", "NewRenderer", s.new_renderer ? "1" : "0");
    upsert_ini_key(body, "Graphics", "NoSpriteLimits", s.no_sprite_limits ? "1" : "0");
    upsert_ini_key(body, "Sound", "EnableAudio", s.enable_audio ? "1" : "0");
    upsert_ini_key(body, "Sound", "AudioFreq", std::to_string(s.audio_freq));
    upsert_ini_key(body, "GamepadMap", "EnableGamepad1", s.enable_gamepad[0] ? "true" : "false");
    upsert_ini_key(body, "GamepadMap", "EnableGamepad2", s.enable_gamepad[1] ? "true" : "false");
    upsert_ini_key(body, "GamepadMap", "GamepadDeadzone",
                   std::to_string(std::clamp(s.gamepad_deadzone, 1, 32767)));
    upsert_ini_key(body, "GamepadMap", "Controls", join_controls(s.pad_controls[0]));
    upsert_ini_key(body, "GamepadMap", "ControlsP2", join_controls(s.pad_controls[1]));
    for (int p = 0; p < kP; ++p) {
        const std::string n = std::to_string(p + 1);
        upsert_ini_key(body, "Controller", "SourceP" + n,
                       std::to_string(std::clamp(s.player_src[static_cast<size_t>(p)], 0, 2)));
        upsert_ini_key(body, "Controller", "GuidP" + n,
                       s.player_src[static_cast<size_t>(p)] == 2 ? s.player_guid[static_cast<size_t>(p)]
                                                                 : std::string());
    }
    for (int i = 0; i < SnesPlatformSettings::kHotkeyCount; ++i) {
        const std::string& v = s.hotkeys[static_cast<size_t>(i)];
        upsert_ini_key(body, "KeyMap", kHotkeyKeys[i], v.empty() ? kHotkeyDefaults[i] : v);
    }
}

std::string keybinds_body(const SnesPlatformSettings& s) {
    std::string out =
        "# SNES Controller Keybinds\n"
        "# Written by RetComM (snesrecomp keybinds.c-compatible format).\n"
        "# Use SDL key names, or \"None\" to leave a button unbound.\n\n";
    for (int p = 0; p < kP; ++p) {
        out += "[player" + std::to_string(p + 1) + "]\n";
        for (int i = 0; i < kN; ++i) {
            const int b = kKbFileOrder[i];
            char line[64];
            std::snprintf(line, sizeof(line), "%-7s = %s\n", kButtonIniKeys[b],
                          sdl_scancode_name(s.kb_scancode[static_cast<size_t>(p)][static_cast<size_t>(b)]));
            out += line;
        }
        out += "\n";
    }
    return out;
}

} // namespace

const char* SnesPlatformSettings::button_label(int b) {
    return (b >= 0 && b < kN) ? kButtonLabels[b] : "?";
}
const char* SnesPlatformSettings::button_ini_key(int b) {
    return (b >= 0 && b < kN) ? kButtonIniKeys[b] : "";
}
int SnesPlatformSettings::button_default_scancode(int b) {
    return (b >= 0 && b < kN) ? kKbDefaults[b] : 0;
}
const char* SnesPlatformSettings::button_default_pad_token(int b) {
    return (b >= 0 && b < kN) ? kPadDefaults[b] : "";
}
int SnesPlatformSettings::pad_token_count() {
    return static_cast<int>(sizeof(kPadTokens) / sizeof(kPadTokens[0]));
}
const char* SnesPlatformSettings::pad_token(int i) {
    return (i >= 0 && i < pad_token_count()) ? kPadTokens[i] : "";
}
const char* SnesPlatformSettings::hotkey_ini_key(int i) {
    return (i >= 0 && i < kHotkeyCount) ? kHotkeyKeys[i] : "";
}
const char* SnesPlatformSettings::hotkey_label(int i) {
    return (i >= 0 && i < kHotkeyCount) ? kHotkeyLabels[i] : "";
}
const char* SnesPlatformSettings::hotkey_default(int i) {
    return (i >= 0 && i < kHotkeyCount) ? kHotkeyDefaults[i] : "";
}

void SnesPlatformSettings::apply_defaults_if_unset() {
    for (int i = 0; i < kHotkeyCount; ++i)
        if (hotkeys[static_cast<size_t>(i)].empty()) hotkeys[static_cast<size_t>(i)] = kHotkeyDefaults[i];
    for (int p = 0; p < kMaxPlayers; ++p) {
        for (int b = 0; b < kButtonCount; ++b) {
            auto& tok = pad_controls[static_cast<size_t>(p)][static_cast<size_t>(b)];
            if (tok.empty()) tok = kPadDefaults[b];
        }
    }
}

void SnesPlatformSettings::reset_system_to_defaults() {
    const SnesPlatformSettings d;
    const auto keep_src = player_src;
    const auto keep_guid = player_guid;
    *this = d;
    player_src = keep_src;
    player_guid = keep_guid;
    for (int p = 0; p < kMaxPlayers; ++p)
        for (int b = 0; b < kButtonCount; ++b)
            kb_scancode[static_cast<size_t>(p)][static_cast<size_t>(b)] = kKbDefaults[b];
    apply_defaults_if_unset();
}

fs::path snes_platform_settings_dir(const Paths& paths) {
    return paths.data_dir / "platform" / "snes";
}
fs::path snes_platform_settings_ini_path(const Paths& paths) {
    return snes_platform_settings_dir(paths) / "config.ini";
}
fs::path snes_platform_keybinds_ini_path(const Paths& paths) {
    return snes_platform_settings_dir(paths) / "keybinds.ini";
}

SnesPlatformSettings load_snes_platform_settings(const Paths& paths) {
    SnesPlatformSettings s;
    for (int p = 0; p < kP; ++p)
        for (int b = 0; b < kN; ++b)
            s.kb_scancode[static_cast<size_t>(p)][static_cast<size_t>(b)] = kKbDefaults[b];
    std::error_code ec;
    const fs::path ini = snes_platform_settings_ini_path(paths);
    const fs::path kb = snes_platform_keybinds_ini_path(paths);
    if (fs::is_regular_file(ini, ec)) parse_ini(read_text_file(ini), s);
    if (fs::is_regular_file(kb, ec)) parse_keybinds(read_text_file(kb), s);
    s.apply_defaults_if_unset();
    return s;
}

bool save_snes_platform_settings(const Paths& paths, const SnesPlatformSettings& s_in,
                                 std::string* error) {
    SnesPlatformSettings s = s_in;
    s.apply_defaults_if_unset();
    std::string ini = read_text_file(snes_platform_settings_ini_path(paths));
    if (ini.empty())
        ini = "# RetComM global Super Nintendo settings — merged into each title's "
              "config.ini on install/update/launch.\n";
    write_ini_body(ini, s);
    if (!write_text_file(snes_platform_settings_ini_path(paths), ini, error)) return false;
    return write_text_file(snes_platform_keybinds_ini_path(paths), keybinds_body(s), error);
}

ApplySnesPlatformResult apply_snes_platform_defaults(const Paths& paths, const AppState& state,
                                                     const Title& title,
                                                     const fs::path& game_cwd) {
    ApplySnesPlatformResult r;
    if (!is_snes_platform(title.platform)) {
        r.skipped = true;
        r.message = "not a Super Nintendo title";
        return r;
    }
    if (title_excludes_platform_config(state, title.id)) {
        r.skipped = true;
        r.message = "excluded from platform config: " + title.id;
        return r;
    }
    if (game_cwd.empty()) {
        r.ok = false;
        r.message = "empty game cwd";
        return r;
    }
    std::error_code ec;
    if (!fs::is_regular_file(snes_platform_settings_ini_path(paths), ec) &&
        !fs::is_regular_file(snes_platform_keybinds_ini_path(paths), ec)) {
        r.skipped = true;
        r.message = "no global Super Nintendo settings yet";
        return r;
    }

    const SnesPlatformSettings s = load_snes_platform_settings(paths);
    const fs::path dest_ini = game_cwd / "config.ini";
    std::string ini = read_text_file(dest_ini);
    write_ini_body(ini, s);
    std::string err;
    if (!write_text_file(dest_ini, ini, &err)) {
        r.ok = false;
        r.message = err;
        return r;
    }
    if (!write_text_file(game_cwd / "keybinds.ini", keybinds_body(s), &err)) {
        r.ok = false;
        r.message = err;
        return r;
    }
    r.message = "applied Super Nintendo platform settings → " + game_cwd.string();
    return r;
}

} // namespace retcomm
