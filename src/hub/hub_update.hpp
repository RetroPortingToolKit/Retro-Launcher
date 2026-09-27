#pragma once

// Direct mode's Update page (docs/RELEASES.md, "Updates"): the game, its
// core, the runner and this hub, each checked against its own release and,
// where one may be installed, installed beside the data -- never over the
// app, which is read-only in an AppImage, a .app or an installed tree. The
// next start picks the newest usable copy of each, so "restart" is only
// "start again":
//
//   <data dir>/hub/<version>/retro-hub         Retro-Launcher hub-manifest.json
//   <data dir>/core/<version>/<core library>   n64lle-release-manifest.json
//   <data dir>/runtime/<version>/retro-core-runner   (runtime_update.hpp)
//
// The game itself is only checked: its package is generated from the
// player's ROM, so no release carries it (title.json `update`, hub_title.hpp).
//
// Nothing here needs a window, so the unit test drives it with file://
// manifests.

#include "retcomm/paths.hpp"

#include <filesystem>
#include <string>

namespace retcomm::hub {

namespace fs = std::filesystem;

struct UpdateItem {
    std::string current;    // what runs now; empty when unknown
    std::string latest;     // the newest published; empty when not read
    bool ok = false;        // the check (and the install, if asked) worked
    bool available = false; // newer, and this hub can install it
    bool installed = false; // a newer copy is in place: start again to use it
    bool manual = false;    // newer, but not something this hub can install
    std::string message;
    std::string link;       // a page for the player (the game's release)
};

// What this Direct-mode hub runs, and so what the page may update.
struct UpdateTarget {
    Paths paths;      // the hub's: a title app's are its data dir
    fs::path exe_dir; // this hub's directory (its bundled runner)
    bool title_mode = false;
    std::string title_version; // title.json `version`
    std::string game_github;   // title.json update.github
    fs::path core;             // the core a session starts now
    fs::path bundled_core;     // title.json's core (empty: none)
    fs::path package;          // the game package, or empty
    bool core_pinned = false;  // --run-core / --core named it
    std::string hub_version, hub_commit;
    // A developer's own build is in use, chosen on the Update page (config
    // dev_core_path / dev_hub_path). A release always replaces one: its row
    // offers the newest release whatever the versions say.
    bool core_dev = false, hub_dev = false;
};

UpdateItem check_game(const UpdateTarget& t);
UpdateItem update_core(const UpdateTarget& t, bool install);
UpdateItem update_runner(const UpdateTarget& t, bool install);
UpdateItem update_hub(const UpdateTarget& t, bool install);

// ---- what a start picks --------------------------------------------------------
//
// The config's developer paths (dev_core_path, dev_runner_path, dev_hub_path,
// set with the Update page's Browse) come first; hub_main.cpp applies them.
// Below is what a start picks without them.

fs::path hub_updates_dir(const Paths& paths);  // <data>/hub
fs::path core_updates_dir(const Paths& paths); // <data>/core

// <core>.rcore.toml's [core]: what the runner checks the library against.
struct CoreSidecar {
    std::string id, version, library;
};
bool read_core_sidecar(const fs::path& core, CoreSidecar& out, std::string* error);

// `module_abi` from the package's <stem>.n64game.toml, or -1 when it records
// none. The core refuses a package of another module ABI at load.
int package_module_abi(const fs::path& package);

struct ResolvedCore {
    fs::path path;       // the bundled core when nothing newer fits
    std::string version;
    std::string source;  // "bundled" | "updated"
    std::string note;
};
// The newest of `bundled` and the cores Update installed that are the same
// core (sidecar id) built for this package's module ABI. A core with no
// package is the game itself and is never replaced.
ResolvedCore resolve_title_core(const Paths& paths, const fs::path& bundled,
                                const fs::path& package);

struct InstalledHub {
    fs::path path;
    std::string version;
};
// The newest hub Update installed that is newer than `self_version`, runs,
// reports its manifest's version, has this Update page (`updates` in its
// --version), and has title-app mode when `title_mode`.
InstalledHub newest_installed_hub(const Paths& paths, const std::string& self_version,
                                  bool title_mode);

} // namespace retcomm::hub
