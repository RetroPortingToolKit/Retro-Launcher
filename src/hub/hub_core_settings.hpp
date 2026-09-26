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

struct PlatformInput {
    std::array<SeatAssign, kInputSeats> seats{};
    // Each seat's own maps: a gamepad map and a keyboard map. The one the seat
    // reads is its device's (an Auto seat can read either).
    std::array<InputBindings, kInputSeats> maps;
    // Stick deadzone, percent of full throw, every seat. 0 = raw (the default:
    // what the hub always sent, and every N64 game has a deadzone of its own).
    int deadzone_pct = 0;
    PlatformInput();
    bool operator==(const PlatformInput& o) const {
        return seats == o.seats && maps == o.maps && deadzone_pct == o.deadzone_pct;
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

// One frame of pads, from the connected devices and the seats' maps.
void fill_pads_from_input(const PlatformInput& in, rcore_pad pads[RCORE_MAX_SEATS],
                          std::uint32_t max_seats);

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

} // namespace retcomm::hub
