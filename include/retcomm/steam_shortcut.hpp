#pragma once

#include "retcomm/paths.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace retcomm {

// A Steam profile on this machine. Non-Steam shortcuts and their art are stored
// per profile, not per install, so every write names the profiles it touched.
struct SteamUser {
    fs::path dir;             // <steam>/userdata/<account id>
    std::string account_id;
    std::string persona;      // from loginusers.vdf when it knows the name
    bool most_recent = false; // Steam's own MostRecent flag
};

struct SteamInstall {
    bool found = false;
    fs::path root;
    std::vector<SteamUser> users;
    std::string hint;         // why nothing usable was found
};

SteamInstall find_steam_install();

// Best effort, and false when it cannot be determined. Steam rewrites
// shortcuts.vdf from memory when it exits, so a write made while it is running
// is lost — the caller is expected to say so rather than fail.
bool steam_is_running();

// What a shortcut launches. `exe` is stored quoted, the way Steam writes it,
// and the app id is derived from that exact string.
struct SteamShortcutSpec {
    std::string app_name;
    fs::path exe;
    fs::path start_dir;
    std::string launch_options;
    std::vector<std::string> tags;
    // Stable identity for the entry, stamped into the unused DevkitGameID
    // field. Without it the only handle on an entry is its display name, so two
    // titles that happen to share one would overwrite each other, and a game
    // the player renames in Steam would be added a second time. Steam ignores
    // the field while Devkit is 0, and it does not show up anywhere in the UI —
    // unlike tags, which are the player's own collections.
    std::string owner_id;
};

// The command a shortcut must run to start a title through this launcher: the
// retcomm CLI, or the AppImage's "cli" entry point. Going through it rather
// than the game binary means the boot disc, BIOS preference, canonical save and
// memory cards resolve exactly as pressing Play does — and the CLI waits for
// the game, so Steam's session stays attached for as long as it runs.
// `args_prefix` is what precedes the title id in LaunchOptions.
bool steam_launcher_command(fs::path* exe, std::string* args_prefix, std::string* why);

// Steam's derivation for a non-Steam shortcut: crc32 of the stored Exe field
// concatenated with AppName, top bit set. Art files in <user>/config/grid are
// named after it, and shortcuts.vdf stores it as a signed int32.
uint32_t steam_shortcut_app_id(const SteamShortcutSpec& spec);
// The id that steam://rungameid/ takes.
uint64_t steam_shortcut_run_id(uint32_t app_id);

// Composed images, sized for the slots Steam reads. Any of them may be empty.
struct SteamArtSet {
    fs::path portrait;  // 600x900   -> grid/<id>p.png     (library grid)
    fs::path capsule;   // 460x215   -> grid/<id>.png      (recent / Big Picture)
    fs::path hero;      // 1920x620  -> grid/<id>_hero.png (library header)
    fs::path icon;      // 256x256   -> grid/<id>_icon.png (and the icon field)
};

struct SteamShortcutResult {
    bool ok = false;
    uint32_t app_id = 0;
    std::string message;          // one line, already written for the log
    std::vector<fs::path> files;  // every file created or rewritten
};

// Add the shortcut, or rewrite it in place if a shortcut with this name is
// already there. Writes to every profile the install knows about: one Deck has
// one, and a shared machine should get the entry for whoever is logged in.
SteamShortcutResult steam_write_shortcut(const SteamInstall& install,
                                         const SteamShortcutSpec& spec, const SteamArtSet& art,
                                         std::string* error = nullptr);

// Remove the shortcut and the art files named after its id.
SteamShortcutResult steam_remove_shortcut(const SteamInstall& install,
                                          const SteamShortcutSpec& spec,
                                          std::string* error = nullptr);

// Every owner id Retro has stamped, across every profile, read in one pass.
// Asking per title would reparse the file once per title.
std::vector<std::string> steam_shortcut_owner_ids(const SteamInstall& install);

}  // namespace retcomm
