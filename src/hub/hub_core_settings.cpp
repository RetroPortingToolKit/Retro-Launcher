#include "hub/hub_core_settings.hpp"

#include "transport.hpp" // Retro-Runtime corelink: run_to_completion, path_utf8

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>

namespace retcomm::hub {

namespace {

namespace corelink = ::retro::corelink;

// Pressed, for a button target driven by an axis: the threshold the hub always
// used for the triggers' digital L2/R2.
constexpr int kAxisPressed = 16384;

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// --describe escapes \, TAB, LF and CR inside a field.
std::string unescape_field(const std::string& f) {
    std::string out;
    out.reserve(f.size());
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '\\' || i + 1 == f.size()) {
            out += f[i];
            continue;
        }
        switch (f[++i]) {
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case '\\': out += '\\'; break;
            default: out += '\\'; out += f[i]; break;
        }
    }
    return out;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        out.push_back(unescape_field(line.substr(start, tab - start)));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return out;
}

bool parse_i64(const std::string& s, std::int64_t& v) {
    try {
        size_t used = 0;
        v = std::stoll(s, &used);
        return used == s.size();
    } catch (...) {
        return false;
    }
}

struct TargetInfo {
    const char* key;
    const char* generic;
    std::uint32_t button; // RCORE_PAD_* bit, or 0
    std::uint32_t axis;   // RCORE_AXIS_* + 1, or 0
    std::int32_t dir;     // for a half-axis: +1 / -1; a trigger: +1
};

// Indexed by PadTarget.
constexpr TargetInfo kTargets[kPadTargetCount] = {
    {"south", "South face", RCORE_PAD_SOUTH, 0, 0},
    {"east", "East face", RCORE_PAD_EAST, 0, 0},
    {"west", "West face", RCORE_PAD_WEST, 0, 0},
    {"north", "North face", RCORE_PAD_NORTH, 0, 0},
    {"l1", "L1", RCORE_PAD_L1, 0, 0},
    {"r1", "R1", RCORE_PAD_R1, 0, 0},
    {"l2", "L2 (digital)", RCORE_PAD_L2, 0, 0},
    {"r2", "R2 (digital)", RCORE_PAD_R2, 0, 0},
    {"l3", "L3", RCORE_PAD_L3, 0, 0},
    {"r3", "R3", RCORE_PAD_R3, 0, 0},
    {"start", "Start", RCORE_PAD_START, 0, 0},
    {"select", "Select", RCORE_PAD_SELECT, 0, 0},
    {"up", "D-pad up", RCORE_PAD_DPAD_UP, 0, 0},
    {"down", "D-pad down", RCORE_PAD_DPAD_DOWN, 0, 0},
    {"left", "D-pad left", RCORE_PAD_DPAD_LEFT, 0, 0},
    {"right", "D-pad right", RCORE_PAD_DPAD_RIGHT, 0, 0},
    {"lx-", "Left stick left", 0, RCORE_AXIS_LX + 1, -1},
    {"lx+", "Left stick right", 0, RCORE_AXIS_LX + 1, +1},
    {"ly-", "Left stick down", 0, RCORE_AXIS_LY + 1, -1},
    {"ly+", "Left stick up", 0, RCORE_AXIS_LY + 1, +1},
    {"rx-", "Right stick left", 0, RCORE_AXIS_RX + 1, -1},
    {"rx+", "Right stick right", 0, RCORE_AXIS_RX + 1, +1},
    {"ry-", "Right stick down", 0, RCORE_AXIS_RY + 1, -1},
    {"ry+", "Right stick up", 0, RCORE_AXIS_RY + 1, +1},
    {"lt", "Left trigger", 0, RCORE_AXIS_LT + 1, +1},
    {"rt", "Right trigger", 0, RCORE_AXIS_RT + 1, +1},
};

const TargetInfo& info(PadTarget t) { return kTargets[static_cast<int>(t)]; }

// The descriptor that names this target: the same button bit, or the same
// axis and direction. Direction 0 declares the whole axis (n64lle's "Control
// Stick (left/right)"), so it names both halves.
const CoreInputDecl* decl_for(PadTarget t, const CoreDescription& d) {
    const TargetInfo& ti = info(t);
    for (const CoreInputDecl& in : d.inputs) {
        if (ti.button && in.button == ti.button) return &in;
        if (ti.axis && in.button == 0 && in.axis == ti.axis &&
            (in.axis_direction == ti.dir || in.axis_direction == 0))
            return &in;
    }
    return nullptr;
}

// Which half of a stick axis a target is, for a whole-axis label.
const char* half_word(PadTarget t) {
    switch (t) {
        case PadTarget::LxMinus: case PadTarget::RxMinus: return "left";
        case PadTarget::LxPlus: case PadTarget::RxPlus: return "right";
        case PadTarget::LyMinus: case PadTarget::RyMinus: return "down";
        case PadTarget::LyPlus: case PadTarget::RyPlus: return "up";
        default: return nullptr;
    }
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Write through a sibling temp file and rename, so a crash mid-save leaves the
// previous settings rather than half of the new ones.
bool write_atomically(const fs::path& path, const std::string& body, std::string* error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = path.parent_path().string() + ": " + ec.message();
        return false;
    }
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out || !(out << body) || !out.flush()) {
            if (error) *error = tmp.string() + ": cannot write";
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        if (error) *error = path.string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// [section] key = value, the subset input.ini and options/*.ini use.
template <typename Fn>
void for_each_ini(const std::string& body, Fn&& fn) {
    std::istringstream in(body);
    std::string line, section;
    while (std::getline(in, line)) {
        const std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        if (s.front() == '[' && s.back() == ']') {
            section = trim(s.substr(1, s.size() - 2));
            continue;
        }
        const auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        fn(section, trim(s.substr(0, eq)), trim(s.substr(eq + 1)));
    }
}

int source_value(SDL_Gamepad* g, const PadSource& s) {
    switch (s.kind) {
        case PadSource::Button:
            return SDL_GetGamepadButton(g, static_cast<SDL_GamepadButton>(s.code)) ? 32767 : 0;
        case PadSource::AxisPlus:
            return std::max(0, static_cast<int>(
                                   SDL_GetGamepadAxis(g, static_cast<SDL_GamepadAxis>(s.code))));
        case PadSource::AxisMinus:
            return std::max(0, -static_cast<int>(
                                    SDL_GetGamepadAxis(g, static_cast<SDL_GamepadAxis>(s.code))));
        case PadSource::None:
            break;
    }
    return 0;
}

bool source_pressed(SDL_Gamepad* g, const PadSource& s) {
    if (s.kind == PadSource::Button)
        return SDL_GetGamepadButton(g, static_cast<SDL_GamepadButton>(s.code));
    return source_value(g, s) > kAxisPressed;
}

// Target values (0..32768 per half-axis) folded into one rcore_pad.
void fold(rcore_pad& p, const int value[kPadTargetCount], const bool pressed[kPadTargetCount]) {
    for (int t = 0; t < kPadButtonTargets; ++t)
        if (pressed[t]) p.buttons |= kTargets[t].button;
    auto clamp16 = [](int v) { return static_cast<std::int16_t>(std::clamp(v, -32768, 32767)); };
    auto T = [](PadTarget t) { return static_cast<int>(t); };
    p.axes[RCORE_AXIS_LX] = clamp16(value[T(PadTarget::LxPlus)] - value[T(PadTarget::LxMinus)]);
    p.axes[RCORE_AXIS_LY] = clamp16(value[T(PadTarget::LyPlus)] - value[T(PadTarget::LyMinus)]);
    p.axes[RCORE_AXIS_RX] = clamp16(value[T(PadTarget::RxPlus)] - value[T(PadTarget::RxMinus)]);
    p.axes[RCORE_AXIS_RY] = clamp16(value[T(PadTarget::RyPlus)] - value[T(PadTarget::RyMinus)]);
    p.axes[RCORE_AXIS_LT] = clamp16(value[T(PadTarget::Lt)]);
    p.axes[RCORE_AXIS_RT] = clamp16(value[T(PadTarget::Rt)]);
}

} // namespace

// ---- the core's declarations ------------------------------------------------

bool parse_core_description(const std::string& text, CoreDescription& out, std::string* error) {
    out = CoreDescription{};
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    std::istringstream in(text);
    std::string line;
    bool header = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find('\t') == std::string::npos) continue; // stderr, not a record
        const std::vector<std::string> f = split_tabs(line);
        const std::string& kind = f[0];
        if (kind == "describe") {
            if (f.size() < 2 || f[1] != "1")
                return fail("unknown --describe revision '" + (f.size() > 1 ? f[1] : "") + "'");
            header = true;
        } else if (!header) {
            continue;
        } else if (kind == "core" && f.size() >= 4) {
            out.core_id = f[1];
            out.core_version = f[2];
            out.platforms = f[3];
        } else if (kind == "option" && f.size() >= 10) {
            CoreOptionDecl o;
            o.key = f[1];
            o.type = f[2];
            std::istringstream flags(f[3]);
            for (std::string fl; std::getline(flags, fl, ',');) {
                if (fl == "restart") o.restart = true;
                else if (fl == "netplay") o.netplay = true;
                else if (fl == "developer") o.developer = true;
            }
            o.has_default = f[4] == "1";
            o.default_value = f[5];
            if (!parse_i64(f[6], o.int_min) || !parse_i64(f[7], o.int_max))
                return fail("option " + o.key + ": bad integer range");
            o.label = f[8];
            o.description = f[9];
            out.options.push_back(std::move(o));
        } else if (kind == "value" && f.size() >= 3) {
            if (out.options.empty() || out.options.back().key != f[1])
                return fail("value for '" + f[1] + "' does not follow its option");
            out.options.back().values.push_back(f[2]);
        } else if (kind == "input" && f.size() >= 5) {
            std::int64_t b = 0, a = 0, dir = 0;
            if (!parse_i64(f[1], b) || !parse_i64(f[2], a) || !parse_i64(f[3], dir))
                return fail("input '" + f[4] + "': bad number");
            CoreInputDecl d;
            d.button = static_cast<std::uint32_t>(b);
            d.axis = static_cast<std::uint32_t>(a);
            d.axis_direction = static_cast<std::int32_t>(dir);
            d.label = f[4];
            out.inputs.push_back(std::move(d));
        }
    }
    if (!header) return fail("no 'describe' header");
    out.ok = true;
    return true;
}

CoreDescription describe_core(const fs::path& runner, const fs::path& core,
                              const fs::path& package, int timeout_ms) {
    CoreDescription d;
    std::error_code ec;
    std::mt19937_64 rng(static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path report =
        fs::temp_directory_path(ec) / ("retro-hub-describe-" + std::to_string(rng()) + ".txt");
    corelink::SpawnSpec spec;
    spec.args = {corelink::path_utf8(runner), "--describe", "--core", corelink::path_utf8(core)};
    if (!package.empty()) {
        spec.args.push_back("--package");
        spec.args.push_back(corelink::path_utf8(package));
    }
    spec.log = report;
    std::string err;
    const auto code = corelink::run_to_completion(spec, timeout_ms, &err);
    const std::string text = read_text(report);
    fs::remove(report, ec);
    if (!code) {
        d.error = err;
        return d;
    }
    std::string perr;
    if (*code == 0 && parse_core_description(text, d, &perr)) {
        d.raw = text;
        return d;
    }
    // The runner's own last words say why: an unknown flag from one that
    // predates --describe, or the core's load failure.
    std::string last;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);)
        if (!trim(l).empty()) last = trim(l);
    d = CoreDescription{};
    d.error = "retro-core-runner --describe " +
              (*code == 0 ? "printed no description (" + perr + ")"
                          : "exited " + std::to_string(*code)) +
              (last.empty() ? std::string() : ": " + last) +
              ". A runner that reports 'describe 1' in --version is needed.";
    return d;
}

// ---- input bindings ---------------------------------------------------------

const char* pad_target_key(PadTarget t) { return info(t).key; }
const char* pad_target_generic_name(PadTarget t) { return info(t).generic; }

std::string pad_target_label(PadTarget t, const CoreDescription& d) {
    const CoreInputDecl* in = decl_for(t, d);
    if (!in || in->label.empty()) return info(t).generic;
    if (in->axis_direction == 0 && half_word(t)) return in->label + ": " + half_word(t);
    return in->label;
}

bool pad_target_declared(PadTarget t, const CoreDescription& d) { return decl_for(t, d) != nullptr; }

InputBindings default_input_bindings() {
    InputBindings b;
    auto P = [&](PadTarget t) -> PadSource& { return b.pad[static_cast<int>(t)]; };
    auto K = [&](PadTarget t) -> SDL_Scancode& { return b.key[static_cast<int>(t)]; };
    auto btn = [](SDL_GamepadButton x) { return PadSource{PadSource::Button, x}; };
    auto plus = [](SDL_GamepadAxis x) { return PadSource{PadSource::AxisPlus, x}; };
    auto minus = [](SDL_GamepadAxis x) { return PadSource{PadSource::AxisMinus, x}; };
    P(PadTarget::South) = btn(SDL_GAMEPAD_BUTTON_SOUTH);
    P(PadTarget::East) = btn(SDL_GAMEPAD_BUTTON_EAST);
    P(PadTarget::West) = btn(SDL_GAMEPAD_BUTTON_WEST);
    P(PadTarget::North) = btn(SDL_GAMEPAD_BUTTON_NORTH);
    P(PadTarget::L1) = btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    P(PadTarget::R1) = btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    P(PadTarget::L2) = plus(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    P(PadTarget::R2) = plus(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    P(PadTarget::L3) = btn(SDL_GAMEPAD_BUTTON_LEFT_STICK);
    P(PadTarget::R3) = btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    P(PadTarget::Start) = btn(SDL_GAMEPAD_BUTTON_START);
    P(PadTarget::Select) = btn(SDL_GAMEPAD_BUTTON_BACK);
    P(PadTarget::DpadUp) = btn(SDL_GAMEPAD_BUTTON_DPAD_UP);
    P(PadTarget::DpadDown) = btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    P(PadTarget::DpadLeft) = btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    P(PadTarget::DpadRight) = btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    // SDL's stick Y is positive down; rcore's is positive up.
    P(PadTarget::LxMinus) = minus(SDL_GAMEPAD_AXIS_LEFTX);
    P(PadTarget::LxPlus) = plus(SDL_GAMEPAD_AXIS_LEFTX);
    P(PadTarget::LyMinus) = plus(SDL_GAMEPAD_AXIS_LEFTY);
    P(PadTarget::LyPlus) = minus(SDL_GAMEPAD_AXIS_LEFTY);
    P(PadTarget::RxMinus) = minus(SDL_GAMEPAD_AXIS_RIGHTX);
    P(PadTarget::RxPlus) = plus(SDL_GAMEPAD_AXIS_RIGHTX);
    P(PadTarget::RyMinus) = plus(SDL_GAMEPAD_AXIS_RIGHTY);
    P(PadTarget::RyPlus) = minus(SDL_GAMEPAD_AXIS_RIGHTY);
    P(PadTarget::Lt) = plus(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    P(PadTarget::Rt) = plus(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

    K(PadTarget::South) = SDL_SCANCODE_X;
    K(PadTarget::East) = SDL_SCANCODE_C;
    K(PadTarget::West) = SDL_SCANCODE_Z;
    K(PadTarget::North) = SDL_SCANCODE_S;
    K(PadTarget::L1) = SDL_SCANCODE_Q;
    K(PadTarget::R1) = SDL_SCANCODE_E;
    K(PadTarget::L2) = SDL_SCANCODE_LSHIFT;
    K(PadTarget::Start) = SDL_SCANCODE_RETURN;
    K(PadTarget::Select) = SDL_SCANCODE_BACKSPACE;
    K(PadTarget::DpadUp) = SDL_SCANCODE_UP;
    K(PadTarget::DpadDown) = SDL_SCANCODE_DOWN;
    K(PadTarget::DpadLeft) = SDL_SCANCODE_LEFT;
    K(PadTarget::DpadRight) = SDL_SCANCODE_RIGHT;
    K(PadTarget::LxMinus) = SDL_SCANCODE_J;
    K(PadTarget::LxPlus) = SDL_SCANCODE_L;
    K(PadTarget::LyMinus) = SDL_SCANCODE_K;
    K(PadTarget::LyPlus) = SDL_SCANCODE_I;
    // The right stick had no keys in that table, which left a keyboard player
    // without the N64's C buttons (n64lle declares them on the right stick).
    // TFGH, beside the left stick's IJKL.
    K(PadTarget::RyPlus) = SDL_SCANCODE_T;
    K(PadTarget::RyMinus) = SDL_SCANCODE_G;
    K(PadTarget::RxMinus) = SDL_SCANCODE_F;
    K(PadTarget::RxPlus) = SDL_SCANCODE_H;
    return b;
}

std::string pad_source_to_string(const PadSource& s) {
    switch (s.kind) {
        case PadSource::Button:
            if (const char* n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(s.code)))
                return n;
            break;
        case PadSource::AxisPlus:
        case PadSource::AxisMinus:
            if (const char* n = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(s.code)))
                return std::string(n) + (s.kind == PadSource::AxisPlus ? "+" : "-");
            break;
        case PadSource::None:
            break;
    }
    return {};
}

bool pad_source_from_string(const std::string& in, PadSource& out) {
    const std::string s = trim(in);
    if (s.empty()) {
        out = PadSource{};
        return true;
    }
    const char last = s.back();
    if (last == '+' || last == '-') {
        const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(s.substr(0, s.size() - 1).c_str());
        if (a == SDL_GAMEPAD_AXIS_INVALID) return false;
        out = PadSource{last == '+' ? PadSource::AxisPlus : PadSource::AxisMinus, a};
        return true;
    }
    const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(s.c_str());
    if (b == SDL_GAMEPAD_BUTTON_INVALID) return false;
    out = PadSource{PadSource::Button, b};
    return true;
}

std::string pad_source_display(const PadSource& s) {
    switch (s.kind) {
        case PadSource::None: return "-";
        case PadSource::Button:
            switch (static_cast<SDL_GamepadButton>(s.code)) {
                case SDL_GAMEPAD_BUTTON_SOUTH: return "South (A / Cross)";
                case SDL_GAMEPAD_BUTTON_EAST: return "East (B / Circle)";
                case SDL_GAMEPAD_BUTTON_WEST: return "West (X / Square)";
                case SDL_GAMEPAD_BUTTON_NORTH: return "North (Y / Triangle)";
                case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB / L1";
                case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB / R1";
                case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "Left stick press";
                case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "Right stick press";
                case SDL_GAMEPAD_BUTTON_START: return "Start";
                case SDL_GAMEPAD_BUTTON_BACK: return "Back / Select";
                case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
                case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad up";
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad down";
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad left";
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad right";
                default: return pad_source_to_string(s);
            }
        case PadSource::AxisPlus:
        case PadSource::AxisMinus: {
            const bool plus = s.kind == PadSource::AxisPlus;
            switch (static_cast<SDL_GamepadAxis>(s.code)) {
                case SDL_GAMEPAD_AXIS_LEFTX: return plus ? "Left stick right" : "Left stick left";
                case SDL_GAMEPAD_AXIS_LEFTY: return plus ? "Left stick down" : "Left stick up";
                case SDL_GAMEPAD_AXIS_RIGHTX: return plus ? "Right stick right" : "Right stick left";
                case SDL_GAMEPAD_AXIS_RIGHTY: return plus ? "Right stick down" : "Right stick up";
                case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "LT / L2";
                case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "RT / R2";
                default: return pad_source_to_string(s);
            }
        }
    }
    return {};
}

fs::path platform_settings_dir(const fs::path& data_dir, const std::string& platform) {
    // The same root the PSX and SNES Configure pages keep theirs under.
    return data_dir / "platform" / platform;
}

namespace {

const char* seat_source_key(SeatAssign::Source s) {
    switch (s) {
        case SeatAssign::Auto: return "auto";
        case SeatAssign::Keyboard: return "keyboard";
        case SeatAssign::Gamepad: return "gamepad";
        case SeatAssign::None: return "none";
    }
    return "auto";
}

// Pad and key lines for one map, as [<prefix>gamepad] / [<prefix>keyboard].
void write_map(std::ostringstream& o, const std::string& prefix, const InputBindings& b) {
    o << "\n[" << prefix << "gamepad]\n";
    for (int t = 0; t < kPadTargetCount; ++t)
        o << kTargets[t].key << " = " << pad_source_to_string(b.pad[t]) << "\n";
    o << "\n[" << prefix << "keyboard]\n";
    for (int t = 0; t < kPadTargetCount; ++t) {
        const char* n = b.key[t] == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(b.key[t]);
        o << kTargets[t].key << " = " << (n ? n : "") << "\n";
    }
}

// One line of a [gamepad] / [keyboard] section into `b`.
void read_map_line(InputBindings& b, bool gamepad, const std::string& key,
                   const std::string& value, const fs::path& path,
                   std::vector<std::string>* warnings) {
    int t = 0;
    while (t < kPadTargetCount && key != kTargets[t].key) ++t;
    if (t == kPadTargetCount) {
        if (warnings) warnings->push_back(path.string() + ": unknown target '" + key + "'");
        return;
    }
    if (gamepad) {
        PadSource src;
        if (pad_source_from_string(value, src)) b.pad[t] = src;
        else if (warnings)
            warnings->push_back(path.string() + ": " + key + " = '" + value +
                                "' is not a gamepad input SDL knows; kept the default");
        return;
    }
    if (value.empty()) {
        b.key[t] = SDL_SCANCODE_UNKNOWN;
        return;
    }
    const SDL_Scancode sc = SDL_GetScancodeFromName(value.c_str());
    if (sc != SDL_SCANCODE_UNKNOWN) b.key[t] = sc;
    else if (warnings)
        warnings->push_back(path.string() + ": " + key + " = '" + value +
                            "' is not a key SDL knows; kept the default");
}

// Stick deadzone on one half-axis value (0..32768): below it is rest, above it
// is rescaled so full throw is still full throw.
int apply_deadzone(int v, int dz) {
    if (dz <= 0) return v;
    if (v <= dz) return 0;
    return static_cast<int>((static_cast<long long>(v - dz) * 32768) / (32768 - dz));
}

bool is_stick_target(int t) {
    return t >= static_cast<int>(PadTarget::LxMinus) && t <= static_cast<int>(PadTarget::RyPlus);
}

} // namespace

PlatformInput::PlatformInput() {
    for (InputBindings& m : maps) m = default_input_bindings();
}

PlatformInput load_platform_input(const fs::path& data_dir, const std::string& platform,
                                  std::vector<std::string>* warnings) {
    PlatformInput in;
    const fs::path path = platform_settings_dir(data_dir, platform) / "input.ini";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return in;
    for_each_ini(read_text(path), [&](const std::string& section, const std::string& key,
                                      const std::string& value) {
        if (section == "input") {
            if (key == "deadzone") {
                std::int64_t v = 0;
                if (parse_i64(value, v)) in.deadzone_pct = static_cast<int>(std::clamp<std::int64_t>(v, 0, 90));
            }
            return;
        }
        // [seatN], [seatN.gamepad], [seatN.keyboard]
        if (section.size() < 5 || section.compare(0, 4, "seat") != 0) return;
        const int n = section[4] - '1';
        if (n < 0 || n >= kInputSeats) return;
        const std::string rest = section.substr(5);
        SeatAssign& seat = in.seats[static_cast<size_t>(n)];
        if (rest.empty()) {
            if (key == "source") {
                if (value == "keyboard") seat.source = SeatAssign::Keyboard;
                else if (value == "gamepad") seat.source = SeatAssign::Gamepad;
                else if (value == "none") seat.source = SeatAssign::None;
                else seat.source = SeatAssign::Auto;
            } else if (key == "guid") {
                seat.guid = value;
            } else if (key == "name") {
                seat.name = value;
            }
        } else if (rest == ".gamepad" || rest == ".keyboard") {
            read_map_line(in.maps[static_cast<size_t>(n)], rest == ".gamepad", key, value, path,
                          warnings);
        }
    });
    for (SeatAssign& seat : in.seats)
        if (seat.source == SeatAssign::Gamepad && seat.guid.empty()) seat.source = SeatAssign::Auto;
    return in;
}

bool save_platform_input(const fs::path& data_dir, const std::string& platform,
                         const PlatformInput& in, std::string* error) {
    std::ostringstream o;
    o << "# Retro Launcher controller seats for every " << platform << " title.\n"
      << "# Written by retro-hub's settings page. A missing line is the default;\n"
      << "# an empty binding is unbound. Gamepad inputs use SDL's names\n"
      << "# (a, dpup, leftx+, lefttrigger+ ...), keys SDL's key names.\n\n"
      << "[input]\ndeadzone = " << in.deadzone_pct << "\n";
    for (int n = 0; n < kInputSeats; ++n) {
        const SeatAssign& seat = in.seats[static_cast<size_t>(n)];
        const std::string sec = "seat" + std::to_string(n + 1);
        o << "\n[" << sec << "]\nsource = " << seat_source_key(seat.source) << "\n";
        if (seat.source == SeatAssign::Gamepad) {
            if (seat.name.find_first_of("\r\n") != std::string::npos) {
                if (error) *error = "a gamepad name this file cannot hold";
                return false;
            }
            o << "guid = " << seat.guid << "\nname = " << seat.name << "\n";
        }
        write_map(o, sec + ".", in.maps[static_cast<size_t>(n)]);
    }
    return write_atomically(platform_settings_dir(data_dir, platform) / "input.ini", o.str(),
                            error);
}

std::array<SeatPlan, kInputSeats> plan_seats(const PlatformInput& in,
                                             const std::vector<std::string>& pad_guids) {
    std::array<SeatPlan, kInputSeats> plan{};
    std::vector<bool> taken(pad_guids.size(), false);
    for (int n = 0; n < kInputSeats; ++n) {
        const SeatAssign& seat = in.seats[static_cast<size_t>(n)];
        if (seat.source != SeatAssign::Gamepad) continue;
        for (size_t i = 0; i < pad_guids.size(); ++i) {
            if (!taken[i] && pad_guids[i] == seat.guid) {
                taken[i] = true;
                plan[static_cast<size_t>(n)].pad = static_cast<int>(i);
                break;
            }
        }
    }
    bool keyboard_seat = false;
    for (int n = 0; n < kInputSeats; ++n) {
        const SeatAssign& seat = in.seats[static_cast<size_t>(n)];
        if (seat.source == SeatAssign::Keyboard) {
            plan[static_cast<size_t>(n)].keyboard = true;
            keyboard_seat = true;
        }
        if (seat.source != SeatAssign::Auto) continue;
        for (size_t i = 0; i < pad_guids.size(); ++i) {
            if (!taken[i]) {
                taken[i] = true;
                plan[static_cast<size_t>(n)].pad = static_cast<int>(i);
                break;
            }
        }
    }
    if (in.seats[0].source == SeatAssign::Auto && plan[0].pad < 0 && !keyboard_seat)
        plan[0].keyboard = true;
    return plan;
}

std::string gamepad_guid_string(SDL_JoystickID id) {
    char buf[64] = {};
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), buf, sizeof buf);
    return buf;
}

void fill_pads_from_input(const PlatformInput& in, rcore_pad pads[RCORE_MAX_SEATS],
                          std::uint32_t max_seats) {
    for (std::uint32_t i = 0; i < RCORE_MAX_SEATS; ++i) {
        pads[i] = rcore_pad{};
        pads[i].struct_size = sizeof(rcore_pad);
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    std::vector<std::string> guids;
    for (int i = 0; ids && i < count; ++i) guids.push_back(gamepad_guid_string(ids[i]));
    const auto plan = plan_seats(in, guids);
    const int dz = std::clamp(in.deadzone_pct, 0, 90) * 32768 / 100;
    const bool* k = SDL_GetKeyboardState(nullptr);
    const std::uint32_t seats = std::min<std::uint32_t>(
        std::min<std::uint32_t>(max_seats, RCORE_MAX_SEATS), kInputSeats);
    for (std::uint32_t n = 0; n < seats; ++n) {
        const SeatPlan& sp = plan[n];
        const InputBindings& b = in.maps[n];
        SDL_Gamepad* g = sp.pad >= 0 ? SDL_GetGamepadFromID(ids[sp.pad]) : nullptr;
        if (!g && !sp.keyboard) continue;
        rcore_pad& p = pads[n];
        p.connected = 1;
        int value[kPadTargetCount];
        bool pressed[kPadTargetCount];
        for (int t = 0; t < kPadTargetCount; ++t) {
            if (g) {
                value[t] = source_value(g, b.pad[t]);
                if (is_stick_target(t)) value[t] = apply_deadzone(value[t], dz);
                pressed[t] = b.pad[t].kind != PadSource::None &&
                             (b.pad[t].kind == PadSource::Button ? source_pressed(g, b.pad[t])
                                                                 : value[t] > kAxisPressed);
            } else {
                pressed[t] = b.key[t] != SDL_SCANCODE_UNKNOWN && k && k[b.key[t]];
                value[t] = pressed[t] ? 32767 : 0;
            }
        }
        fold(p, value, pressed);
    }
    SDL_free(ids);
}

bool save_description_cache(const fs::path& data_dir, const std::string& platform,
                            const CoreDescription& d, std::string* error) {
    if (!d.ok || d.raw.empty()) {
        if (error) *error = "nothing to cache";
        return false;
    }
    return write_atomically(platform_settings_dir(data_dir, platform) / "core_description.txt",
                            d.raw, error);
}

CoreDescription load_description_cache(const fs::path& data_dir, const std::string& platform) {
    CoreDescription d;
    const fs::path path = platform_settings_dir(data_dir, platform) / "core_description.txt";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        d.error = "no core has described itself for " + platform + " yet";
        return d;
    }
    std::string err;
    if (!parse_core_description(read_text(path), d, &err)) {
        d = CoreDescription{};
        d.error = path.string() + ": " + err;
        return d;
    }
    d.cached = true;
    return d;
}

// ---- option values ----------------------------------------------------------

namespace {
fs::path options_path(const fs::path& data_dir, const std::string& platform,
                      const std::string& title_key) {
    if (title_key.empty()) return platform_settings_dir(data_dir, platform) / "options.ini";
    return platform_settings_dir(data_dir, platform) / "options" / (title_key + ".ini");
}
} // namespace

std::map<std::string, std::string> load_core_options(const fs::path& data_dir,
                                                     const std::string& platform,
                                                     const std::string& title_key) {
    std::map<std::string, std::string> out;
    const fs::path path = options_path(data_dir, platform, title_key);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return out;
    for_each_ini(read_text(path), [&](const std::string& section, const std::string& key,
                                      const std::string& value) {
        if (section == "options" && !key.empty()) out[key] = value;
    });
    return out;
}

bool save_core_options(const fs::path& data_dir, const std::string& platform,
                       const std::string& title_key,
                       const std::map<std::string, std::string>& values, std::string* error) {
    std::ostringstream o;
    o << "# Retro Launcher core options for "
      << (title_key.empty() ? "every " + platform + " title" : title_key) << ".\n"
      << "# Written by retro-hub's settings page; passed to the core at load.\n"
      << "# Only changed options are listed -- the rest are the core's defaults.\n\n"
      << "[options]\n";
    for (const auto& [k, v] : values) {
        if (k.find_first_of("=\n\r[") != std::string::npos ||
            v.find_first_of("\n\r") != std::string::npos) {
            if (error) *error = "option '" + k + "': a key or value this file cannot hold";
            return false;
        }
        o << k << " = " << v << "\n";
    }
    return write_atomically(options_path(data_dir, platform, title_key), o.str(), error);
}

std::map<std::string, std::string> layer_core_options(
    const std::map<std::string, std::string>& platform,
    const std::map<std::string, std::string>& title,
    const std::map<std::string, std::string>& command_line) {
    std::map<std::string, std::string> out = platform;
    for (const auto& [k, v] : title) out[k] = v;
    for (const auto& [k, v] : command_line) out[k] = v;
    return out;
}

} // namespace retcomm::hub
