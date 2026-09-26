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

#include <filesystem>
#include <string>

namespace retcomm {

namespace fs = std::filesystem;

// The newest non-prerelease manifest, unless RETRO_RUNTIME_MANIFEST_URL names
// another (any URL libcurl reads, file:// included -- for testing).
std::string runtime_manifest_url();

// This build's key in the manifest's `platforms`: linux-x86_64, linux-arm64,
// macos-arm64, macos-x86_64, windows-x86_64 or windows-arm64.
std::string runtime_platform_key();

// <data_dir>/runtime
fs::path runtime_dir(const Paths& paths);

struct ResolvedRunner {
    fs::path path;       // empty when no usable runner was found
    std::string version; // as the runner reports it
    std::string source;  // "override" | "updated" | "bundled"
    std::string note;    // why this one; what was skipped
};

// The runner to start a core with: $RETRO_CORE_RUNNER when set; otherwise the
// newest of the bundled runner (beside the launcher) and the updated ones that
// this build can drive. A bundled "dev" build counts as older than any
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
