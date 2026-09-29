#pragma once

// retro-core-runner, kept current separately from the launcher.
//
// Retro-Runtime publishes the runner per platform with a runtime-manifest.json
// (Retro-Runtime docs/RELEASES.md). This follows that page's update rule: the
// link and ABI majors must be this build's, the machine must meet `requires`,
// the archive's size and SHA-256 are checked before it is opened, and the
// extracted runner must itself report the manifest's version and commit
// before it is used. Updates land in <data_dir>/runtime/<version>/, never
// beside the launcher (read-only in an AppImage or an install), and a running
// session keeps the runner it started with: a new one is used from the next
// launch on.

#include "retcomm/paths.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace retcomm {

namespace fs = std::filesystem;

// RETRO_RUNTIME_MANIFEST_URL when set (any URL libcurl reads, file:// included
// -- for testing), else the newest non-prerelease manifest. update_runtime()
// itself reads the newest release's manifest, pre-releases included, from the
// release feed, and falls back to this URL only when the feed cannot be read.
std::string runtime_manifest_url();

// This build's key in the manifest's `platforms`: linux-x86_64, linux-arm64,
// macos-arm64, macos-x86_64, windows-x86_64 or windows-arm64.
std::string runtime_platform_key();

// <data_dir>/runtime
fs::path runtime_dir(const Paths& paths);

struct ResolvedRunner {
    fs::path path;       // empty when no usable runner was found
    std::string version; // as the runner reports it
    std::string source;  // "override" | "dev" | "updated" | "bundled"
    // What the runner reports it can do (`--version`): 1 when it takes
    // --package for a GAME_PACKAGE core, 0 for one from before that.
    std::uint32_t game_package = 0;
    // Seats that take a Transfer Pak (--tpakN-rom): 4, or 1 for a runner from
    // before seats 2-4.
    std::uint32_t transfer_pak_seats = 1;
    // 1 when it runs netplay (--net-*; built with recomp-net).
    std::uint32_t netplay = 0;
    std::string note;    // why this one; what was skipped
};

// The runner to start a core with: $RETRO_CORE_RUNNER when set; then
// $RETRO_HUB_DEV_RUNNER (Direct mode's developer path) when it can be driven;
// otherwise the
// newest of the bundled runner (beside the launcher, or the one
// $RETRO_HUB_BUNDLED_RUNNER names: a hub that handed over to an updated hub
// passes its own) and the updated ones that this build can drive. A bundled "dev" build counts as older than any
// release; on a tie the bundled one wins.
ResolvedRunner resolve_runner(const Paths& paths, const fs::path& exe_dir);

struct RuntimeUpdateResult {
    bool ok = false;      // the check (and install, if any) finished without error
    bool updated = false; // a newer runner was installed
    bool available = false; // a newer one exists (check_only)
    std::string current_version;
    std::string latest_version;
    std::string message;
    fs::path installed; // the new runner, when updated
};

// Fetches the manifest and, unless check_only, installs a newer runner.
RuntimeUpdateResult update_runtime(const Paths& paths, const fs::path& exe_dir,
                                   bool check_only = false);

} // namespace retcomm
