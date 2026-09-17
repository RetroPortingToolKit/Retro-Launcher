#pragma once

// Reading a game's installed mod packages.
//
// The engines own mods; Retro only shows what is on disk. Layout, from
// psxrecomp runtime/src/mods/mod_packages.cpp (ModPackageManager::scan_root):
//
//   <game>/mods/bundled/<id>/<version>/manifest.toml    build output
//   <game>/mods/installed/<id>/<version>/manifest.toml  launcher-owned
//   <game>/mods/state.toml                              persisted selection
//
// The split exists because one shared tree meant a rebuild deleted every mod
// the player had installed. `bundled` is wiped and re-staged by every build;
// nothing the player owns may live there.
//
// This reader is deliberately read-only. Enablement is persisted in state.toml
// in a shape that depends on whether a package's single feature is `legacy`,
// and writing it wrong would silently change what a player has turned on — see
// recomp-ai-rules/MODS.md §6, "off must be authoritative".

#include "retcomm/paths.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

enum class ModOrigin { Bundled, Installed };

struct ModChoice {
    std::string value;
    std::string label;            // falls back to value
};

// One [[option]], with the choices it offers ([[option.choice]] entries).
struct ModOptionInfo {
    std::vector<ModChoice> choices;
    std::string feature_id;
    std::string id;
    std::string label;
    std::string description;
    std::string group;
    std::string type;             // boolean | integer | choice
    std::string default_value;
    std::string value;            // from state.toml, else default_value
    std::string disabled_by;      // another option id that suppresses this one
    long min = 0, max = 0, step = 0;
    bool has_range = false;
};

struct ModFeatureInfo {
    std::string id;
    std::string name;
    std::string description;
    std::string group;            // left-panel heading, e.g. "Quality of Life"
    std::string channel;          // stable | experimental | developer
    bool default_enabled = false;
    // Whether state.toml currently turns this feature on. Absent selection
    // falls back to default_enabled, which is what the runtime does.
    bool enabled = false;
};

struct ModPackageInfo {
    std::string id;
    std::string version;
    std::string name;             // falls back to id when the manifest omits it
    std::string author;
    std::string description;
    std::string license;
    std::string channel = "stable";
    ModOrigin origin = ModOrigin::Installed;
    fs::path manifest;
    std::vector<ModFeatureInfo> features;
    std::vector<ModOptionInfo> options;
    // A package whose manifest declares no [[feature]] is an all-or-nothing
    // one; state.toml carries its switch as [[package]] enabled.
    bool enabled = false;
    // Described by a built-in provider compiled into the game (resolver =
    // "builtin", written under mods/provided by
    // recomp_launcher_mod_emit_manifests). The provider owns these and
    // persists their state its own way — Super Metroid's writes sm-video.ini —
    // so state.toml says nothing about them and toggling one here would be a
    // write nothing reads.
    bool builtin = false;
    bool has_features() const { return !features.empty(); }
};

struct ModScanResult {
    std::vector<ModPackageInfo> packages;
    // Manifests that exist but could not be read. Surfaced rather than
    // dropped: a mod that silently fails to appear is a support question.
    std::vector<std::string> errors;
    fs::path root;                // <game>/mods, even when it does not exist
    bool root_exists = false;
    // Mods trees found elsewhere under the install (src/current/mods,
    // src/.staging/<engine>/mods) holding manifests the game will never read,
    // because the engine only looks beside the executable. Reported so an
    // empty list can say *why* it is empty instead of just being empty.
    std::vector<fs::path> unstaged_roots;
    int unstaged_manifests = 0;
};

// Turn one feature on or off in <game_dir>/mods/state.toml.
//
// A surgical edit, not a rewrite: only the matching block's `enabled` line
// changes (or the block is appended when absent), so per-feature option values
// and resource picks the player set in-game survive. Rewriting the file from a
// parsed model would drop every key this reader does not model.
//
// `feature_id` empty addresses the package itself, which is the right form for
// a manifest that declares no [[feature]] — the engine gives those a synthetic
// `legacy` feature and persists their switch as [[package]] enabled.
bool set_mod_enabled(const fs::path& game_dir, const std::string& package_id,
                     const std::string& feature_id, bool enabled,
                     std::string* error = nullptr);

// Set one option's value. Written into the block's `values` sub-table —
// [feature.values] for a featured package, [package.values] for an
// all-or-nothing one — which is where both engines read them back from.
// Surgical, like set_mod_enabled: nothing else in the file moves.
bool set_mod_option(const fs::path& game_dir, const std::string& package_id,
                    const std::string& feature_id, const std::string& option_id,
                    const std::string& value, std::string* error = nullptr);

// `game_dir` is the directory the game runs from — the launch cwd, which is
// the release dir for a build install and the AppImage data dir for one of
// those. Never throws; a missing tree yields an empty result.
// `install_root` is optional and only used for the unstaged-tree diagnostic.
ModScanResult scan_game_mods(const fs::path& game_dir, const fs::path& install_root = {});

const char* mod_origin_name(ModOrigin origin);


} // namespace retcomm
