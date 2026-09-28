#pragma once

// What a core declares about itself, and the player's settings built on it:
// seats and bindings shared by every title of a platform, and option values
// for the platform with a per-title layer over them. Both the library and
// Direct mode read the same files, so a pad set up in one is set up in the
// other.
//
//   <data_dir>/platform/<platform>/input.ini             seats, maps, deadzone
//   <data_dir>/platform/<platform>/options.ini           option values (every title)
//   <data_dir>/platform/<platform>/options/<key>.ini     option values (one title)
//   <data_dir>/platform/<platform>/core_description.txt  the last --describe
//   <data_dir>/play.ini                                  the overlay's settings
//                                                        (every core, every title)
//
// The declarations come from the core itself, through
// `retro-core-runner --describe` (Retro-Runtime docs/CORE_RUNNER.md): the hub
// never loads a core in its own process. A runner that predates --describe
// leaves the description empty and says so; nothing here invents a label or a
// choice the core did not declare.

#include "rcore/rcore.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace retcomm::hub {

namespace fs = std::filesystem;

// ---- the core's declarations ------------------------------------------------

struct CoreOptionDecl {
    std::string key;
    std::string type;         // enum | bool | int | string (or the raw number)
    bool restart = false, netplay = false, developer = false;
    bool has_default = false;
    std::string default_value;
    std::int64_t int_min = 0, int_max = 0;
    std::vector<std::string> values; // enum choices, declared order
    std::string label;
    std::string description;
};

struct CoreInputDecl {
    std::uint32_t button = 0; // one RCORE_PAD_* bit, or 0
    std::uint32_t axis = 0;   // RCORE_AXIS_* + 1, or 0
    std::int32_t axis_direction = 0;
    std::string label;
};

struct CoreDescription {
    bool ok = false;
    std::string core_id, core_version, platforms;
    std::vector<CoreOptionDecl> options;
    std::vector<CoreInputDecl> inputs;
    // Why ok is false: a runner without --describe, a core that failed to load.
    std::string error;
    // The --describe text it was parsed from, for the cache.
    std::string raw;
    // Read back from core_description.txt rather than asked of a core now.
    bool cached = false;
};

// Parses `--describe` output (format revision 1). False, with *error set, on a
// missing header or an unknown revision; unknown record kinds are skipped so a
// later runner can add records without breaking this host.
bool parse_core_description(const std::string& text, CoreDescription& out, std::string* error);

// Runs `runner --describe --core <core> [--package <package>]`.
CoreDescription describe_core(const fs::path& runner, const fs::path& core,
                              const fs::path& package, int timeout_ms = 10000);

// The last description a page got for this platform, so the settings page can
// label things when no core is at hand (the library with no title open).
// Stale by nature: `cached` is set, and a page says which core it came from.
bool save_description_cache(const fs::path& data_dir, const std::string& platform,
                            const CoreDescription& d, std::string* error);
CoreDescription load_description_cache(const fs::path& data_dir, const std::string& platform);

// ---- input bindings ---------------------------------------------------------

// Everything a seat can drive: the sixteen rcore buttons, then each half of
// each stick axis, then the two analog triggers.
enum class PadTarget : int {
    // Buttons, in RCORE_PAD_* bit order (bit i = target i).
    South, East, West, North, L1, R1, L2, R2, L3, R3, Start, Select,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    // Half-axes: rcore positive is right / up.
    LxMinus, LxPlus, LyMinus, LyPlus, RxMinus, RxPlus, RyMinus, RyPlus,
    // Triggers, 0..32767.
    Lt, Rt,
    // Not the core's: the host's modifier for controller shortcuts
    // (Function + R1 save states, ...; HostHotkeys). Bound per seat on the
    // Configure page, by default to the Back/Select button; while it is held,
    // that seat's buttons do not reach the game.
    Function,
    Count
};
constexpr int kPadTargetCount = static_cast<int>(PadTarget::Count);
constexpr int kPadButtonTargets = 16;

// The stable key a target has in input.ini ("south", "lx-", "lt", ...).
const char* pad_target_key(PadTarget t);
// What the target is called when the core declared nothing for it.
const char* pad_target_generic_name(PadTarget t);
// The core's own label for this target, if it declared one ("A", "C-Up").
std::string pad_target_label(PadTarget t, const CoreDescription& d);
// True when the core declared a descriptor for this target. A target it did not
// declare still passes through to the core; the page lists it lower down.
bool pad_target_declared(PadTarget t, const CoreDescription& d);

// One physical input on a gamepad: a button, or one direction of an axis.
struct PadSource {
    enum Kind : int { None, Button, AxisPlus, AxisMinus } kind = None;
    int code = 0; // SDL_GamepadButton or SDL_GamepadAxis
    bool operator==(const PadSource& o) const { return kind == o.kind && code == o.code; }
    bool operator!=(const PadSource& o) const { return !(*this == o); }
};

struct InputBindings {
    std::array<PadSource, kPadTargetCount> pad{};
    std::array<SDL_Scancode, kPadTargetCount> key{};
    bool operator==(const InputBindings& o) const { return pad == o.pad && key == o.key; }
    bool operator!=(const InputBindings& o) const { return !(*this == o); }
};

// The table the hub always used before bindings existed: SDL's positional
// face buttons, sticks and triggers one to one; the keyboard as X/C/Z/S faces,
// Q/E shoulders, arrows, IJKL for the left stick. A gamepad plays exactly as
// it did before; the keyboard gains TFGH for the right stick, which that table
// lacked (so a keyboard had no C buttons).
InputBindings default_input_bindings();

std::string pad_source_to_string(const PadSource& s);     // "a", "lefty+", ""
bool pad_source_from_string(const std::string& s, PadSource& out);
std::string pad_source_display(const PadSource& s);       // for the page
// Short, console-neutral: "R1", "L2", "Start", "D-Up", "South" ... ("" for none).
std::string pad_source_label(const PadSource& s);

// ---- seats -------------------------------------------------------------------

constexpr int kInputSeats = 4;

// What drives one controller port.
struct SeatAssign {
    enum Source : int {
        Auto,     // the next gamepad not claimed by another seat; port 1 falls
                  // back to the keyboard when there is none
        Keyboard,
        Gamepad,  // the pad with this GUID, when it is connected
        None,     // unplugged
    };
    Source source = Auto;
    std::string guid; // SDL GUID string, Gamepad only
    std::string name; // what the pad called itself when it was chosen
    bool operator==(const SeatAssign& o) const {
        return source == o.source && guid == o.guid && name == o.name;
    }
    bool operator!=(const SeatAssign& o) const { return !(*this == o); }
};

// What is plugged into a controller's expansion slot. Only the Transfer Pak
// so far (a Game Boy cartridge and its save, which the core reads through
// it); the Controller Pak and Rumble Pak are not implemented yet. Every seat
// can hold one (retro-core-runner --tpak1-rom .. --tpak4-rom); a runner from
// before seats 2-4 gets port 1's only (limit_transfer_paks, hub_main.cpp).
struct SeatPak {
    enum Kind : int { None, TransferPak };
    Kind kind = None;
    std::string gb_rom;  // .gb / .gbc, absolute
    std::string gb_save; // .srm, absolute; empty = the game starts without one
    bool operator==(const SeatPak& o) const {
        return kind == o.kind && gb_rom == o.gb_rom && gb_save == o.gb_save;
    }
    bool operator!=(const SeatPak& o) const { return !(*this == o); }
};
constexpr int kTransferPakSeats = 4; // seats [0, this) can take a Transfer Pak

constexpr int kDefaultDeadzonePct = 10;

struct PlatformInput {
    std::array<SeatAssign, kInputSeats> seats{};
    std::array<SeatPak, kInputSeats> paks{};
    // Each seat's own maps: a gamepad map and a keyboard map. The one the seat
    // reads is its device's (an Auto seat can read either).
    std::array<InputBindings, kInputSeats> maps;
    // Stick deadzone, percent of full throw, every seat. 10 by default, as on
    // every console's page (worn and third-party sticks drift); 0 = raw.
    int deadzone_pct = kDefaultDeadzonePct;
    PlatformInput();
    bool operator==(const PlatformInput& o) const {
        return seats == o.seats && paks == o.paks && maps == o.maps &&
               deadzone_pct == o.deadzone_pct;
    }
    bool operator!=(const PlatformInput& o) const { return !(*this == o); }
};

fs::path platform_settings_dir(const fs::path& data_dir, const std::string& platform);

// A missing file, or a missing key, is the default; a value that names nothing
// SDL knows is dropped and reported in *warnings.
PlatformInput load_platform_input(const fs::path& data_dir, const std::string& platform,
                                  std::vector<std::string>* warnings = nullptr);
bool save_platform_input(const fs::path& data_dir, const std::string& platform,
                         const PlatformInput& in, std::string* error);

// Which device each seat reads this frame, given the connected pads' GUIDs in
// SDL's order. Pure, so it is testable without hardware:
//   1. Gamepad seats claim the first unclaimed pad with their GUID;
//   2. Auto seats take the remaining pads in order;
//   3. Keyboard seats read the keyboard;
//   4. an Auto port 1 left without a pad reads the keyboard, unless another
//      seat is set to Keyboard.
struct SeatPlan {
    int pad = -1;          // index into the connected list, or -1
    bool keyboard = false;
    bool connected() const { return pad >= 0 || keyboard; }
};
std::array<SeatPlan, kInputSeats> plan_seats(const PlatformInput& in,
                                             const std::vector<std::string>& pad_guids);

std::string gamepad_guid_string(SDL_JoystickID id);

// ---- Game Boy saves (Transfer Pak) --------------------------------------------

// The cartridge RAM a Game Boy ROM declares (header 0x147 type, 0x149 RAM
// size): what its battery save holds. MBC2 carts have 512 bytes built in.
// 0 with *error set for a file that is not a Game Boy ROM; 0 without an
// error for a cart with no RAM (it keeps no save).
std::size_t gb_cart_ram_bytes(const fs::path& rom, std::string* error);

// Writes a new, blank save for `rom` at `dest` (0xFF, as a fresh battery RAM
// reads): the size the cart declares. Refuses a cart without RAM and an
// existing `dest`.
bool create_gb_save(const fs::path& rom, const fs::path& dest, std::string* error);

// One frame of pads, from the connected devices and the seats' maps. A seat
// holding its Function input sends no buttons (sticks still pass).
void fill_pads_from_input(const PlatformInput& in, rcore_pad pads[RCORE_MAX_SEATS],
                          std::uint32_t max_seats);

// For host shortcuts: each seat's gamepad this frame (null for none or the
// keyboard) and whether its Function input is held.
struct SeatPadState {
    SDL_Gamepad* pad = nullptr;
    bool function = false;
};
std::array<SeatPadState, kInputSeats> seat_pad_states(const PlatformInput& in);
bool pad_source_down(SDL_Gamepad* g, const PadSource& s);

// ---- option values ----------------------------------------------------------

// key -> value, only the options the player changed; everything else is the
// core's default. `title_key` names a title's file: the session name the play
// path already uses for it (its package's stem in Direct mode). An empty
// title_key is the platform's file, which every title reads under its own.
std::map<std::string, std::string> load_core_options(const fs::path& data_dir,
                                                     const std::string& platform,
                                                     const std::string& title_key);
bool save_core_options(const fs::path& data_dir, const std::string& platform,
                       const std::string& title_key,
                       const std::map<std::string, std::string>& values, std::string* error);

// What a session is given: the platform's values, the title's over them, the
// command line's over both.
std::map<std::string, std::string> layer_core_options(
    const std::map<std::string, std::string>& platform,
    const std::map<std::string, std::string>& title,
    const std::map<std::string, std::string>& command_line);

// ---- host hotkeys ------------------------------------------------------------

// What the host does with the player's shortcuts while a game runs (the play
// overlay's): the same for every core, so stored with the play preferences.
enum class HostAction : int { Menu, SaveStates, ShowFps, Turbo, VolumeUp, VolumeDown, Count };
constexpr int kHostActionCount = static_cast<int>(HostAction::Count);
const char* host_action_key(HostAction a);   // "menu", "states", ...
const char* host_action_label(HostAction a); // "Pause menu", ...

struct HostHotkeys {
    // A key for each action (Turbo's is held; its combo toggles). F1 also
    // opens the menu.
    std::array<SDL_Scancode, kHostActionCount> key{};
    // Function + this on any seat's pad. L3 + R3 also opens the menu, fixed;
    // Guide is never used (Steam and the OS keep it).
    std::array<PadSource, kHostActionCount> combo{};
    bool operator==(const HostHotkeys& o) const { return key == o.key && combo == o.combo; }
    bool operator!=(const HostHotkeys& o) const { return !(*this == o); }
};
// Escape, F7, F3, Tab, =, -; Function + Start, R1, L1, R2, D-pad up, D-pad down.
HostHotkeys default_host_hotkeys();

// ---- play preferences -------------------------------------------------------

// What the play overlay (Retro-Runtime retro_overlay) shows and how loud the
// game is: the same for every core, so one file for every platform. The
// settings page edits it, and so do the in-game hotkeys (F3, +/-).
struct PlayPrefs {
    bool show_fps = false;
    int volume = 100; // percent, 0..100
    // Keep the game's sound while turbo runs, played faster (higher pitched)
    // so it keeps up; off drops it, as turbo used to.
    bool turbo_sound = true;
    HostHotkeys hotkeys = default_host_hotkeys();
    bool operator==(const PlayPrefs& o) const {
        return show_fps == o.show_fps && volume == o.volume && turbo_sound == o.turbo_sound &&
               hotkeys == o.hotkeys;
    }
    bool operator!=(const PlayPrefs& o) const { return !(*this == o); }
};

PlayPrefs load_play_prefs(const fs::path& data_dir);
bool save_play_prefs(const fs::path& data_dir, const PlayPrefs& prefs, std::string* error);

} // namespace retcomm::hub
