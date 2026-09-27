#pragma once

// Title-app mode (docs/RELEASES.md, "Title-app mode"): the hub as one title's
// app. A port's framework stages a title payload -- title.json, the core, the
// game package -- and packaging/title/ wraps it with this hub and a runner
// into an AppImage, a .app or a portable .exe. At launch the hub finds the
// payload beside itself and opens that title's home page.
//
// Everything here needs no window, so `retro-hub --check-title` (the
// packaging gate) and the unit test run it headless:
//
//   - title.json, schema 1, and where it is found;
//   - "the app" (the AppImage, the portable .exe, the .app bundle, else the
//     hub itself) and the data directory beside it, <app dir>/<id>-data/,
//     falling back to <user data>/<id>/ where that is not writable;
//   - the ROM: which one, and whether it is the image the title was built
//     from (size and sha256 from title.json; a mismatch is refused).

#include "retcomm/paths.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace retcomm::hub {

namespace fs = std::filesystem;

// The title.json schema this hub reads. A greater one is refused.
constexpr int kTitleSchema = 1;

struct TitleRom {
    std::vector<std::string> file_names; // looked for beside the app, then in <data>/roms
    bool has_size = false;
    std::uint64_t size = 0;
    std::string sha256; // lowercase hex; empty = not checked
    std::string label;  // what the picker asks for
};

struct TitleInfo {
    fs::path json; // absolute path of title.json
    fs::path root; // the directory holding it; relative paths resolve here
    int schema = 0;
    std::string id, name, version, platform;
    fs::path core;      // absolute
    fs::path package;   // absolute, or empty
    fs::path title_dir; // absolute: `title_dir`, else the package's (or core's) directory
    // `opts`: ["key=value", ...] or {"key": "value"}; the command line's --opt
    // wins over these.
    std::vector<std::pair<std::string, std::string>> opts;
    bool has_rom = false;
    TitleRom rom;
};

// `p` is title.json or the directory holding it.
fs::path title_json_path(const fs::path& p);

// Reads and checks a descriptor. False with *error set when it is unreadable,
// not schema 1 (or older), or names no id / core.
bool load_title(const fs::path& path, TitleInfo& out, std::string* error);

// Where a hub finds its own title when --title is not given: <exe_dir>/title/
// and, on macOS, <exe_dir>/../Resources/title/. Empty when there is none.
fs::path find_bundled_title(const fs::path& exe_dir);

// This process's executable (absolute, symlinks resolved). Empty on failure.
fs::path current_exe_path();

// "The app": what the player launched, whose directory holds the data dir.
struct AppAnchor {
    fs::path app;       // the AppImage / portable .exe / .app bundle / the hub itself
    fs::path dir;       // the directory holding it
    std::string source; // RETRO_HUB_APP | APPIMAGE | RETCOMM_PORTABLE_EXE | bundle | exe
};
// In order: $RETRO_HUB_APP (set by a hub that re-executed another with
// --hub), $APPIMAGE, $RETCOMM_PORTABLE_EXE (the Windows portable stub), the
// .app bundle `exe` is inside (macOS), else `exe` itself.
AppAnchor locate_app(const fs::path& exe);

struct TitleDataDir {
    fs::path dir;
    bool beside_app = true; // false: the <user data>/<id> fallback
    std::string note;       // why the fallback, for the log
};
// <anchor dir>/<id>-data when that is writable (probed by writing; nothing is
// created), else <user data>/<id>. `fallback_base` overrides <user data>
// (tests).
TitleDataDir resolve_title_data_dir(const AppAnchor& anchor, const std::string& id,
                                    const fs::path& fallback_base = {});
// Creates it; when that fails beside the app, falls back as above.
bool ensure_title_data_dir(TitleDataDir& d, const std::string& id, std::string* error,
                           const fs::path& fallback_base = {});

// Paths for a title app: config and data both in the title's data dir, so the
// hub's own files (config.json, sessions/, saves/, platform/) land there.
Paths title_paths(const fs::path& data_dir);

// ---- the ROM ----------------------------------------------------------------

struct RomCheck {
    bool ok = false;
    std::string error; // expected vs got, when !ok (without the path)
    std::uint64_t size = 0;
    std::string sha256; // computed only when a sha256 is expected
};
// Checks `rom` against rom.size and rom.sha256 when title.json has them.
RomCheck check_rom(const fs::path& rom, const TitleRom& want);

struct RomChoice {
    fs::path path;      // empty: none resolved
    std::string source; // --rom | remembered | beside the app | data dir
    bool refused = false; // an explicit --rom did not match; path is empty
    std::string error;    // the refusal (expected vs got)
    std::vector<std::string> notes; // candidates skipped, and why
};
// --rom (checked; a mismatch is refused, not skipped) -> the remembered ROM if
// it still exists and matches -> each rom.file_names entry beside the app,
// then in <data dir>/roms.
RomChoice resolve_rom(const TitleInfo& t, const fs::path& cli_rom, const fs::path& app_dir,
                      const fs::path& data_dir);

// <data dir>/rom.json: {"schema": 1, "path": "<absolute>"}.
fs::path remembered_rom(const fs::path& data_dir);
bool remember_rom(const fs::path& data_dir, const fs::path& rom, std::string* error);

// A path from the command line, made absolute against the launch directory.
fs::path absolute_from(const fs::path& p, const fs::path& cwd);

// `key value` lines of a program's --version, run with a timeout. Empty map
// when it could not be run or exited non-zero.
std::map<std::string, std::string> probe_version(const fs::path& program,
                                                 std::string* error = nullptr);

} // namespace retcomm::hub
