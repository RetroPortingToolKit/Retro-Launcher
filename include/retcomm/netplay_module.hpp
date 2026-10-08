#pragma once

// The netplay module (recomp-net's librecomp_net_module, recomp-net
// include/recomp_net/module.h), kept current separately from the launcher, the
// runner and the cores.
//
// recomp-net publishes it per platform with a netplay-module-manifest.json
// (recomp-net docs/module.md). This follows the same rule as the runtime
// updater: the module ABI must be the one this launcher's runners are built
// for, the machine must meet `requires`, size and SHA-256 are checked before
// the archive is opened, and the extracted library must itself report the
// manifest's version, commit and wire version before it is used. Updates land
// in <data_dir>/netplay-module/<version>/. A running session keeps the module
// it started with; a new one is used from the next match.
//
// Two peers can play only when their modules' wire versions are equal. The
// runner folds it into the match identity, so a mismatch is refused at the
// handshake; the launcher reports it so a player can update.

#include "retcomm/paths.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace retcomm {

namespace fs = std::filesystem;

struct NetplayModuleInfo {
    std::uint32_t abi_version = 0, abi_minor = 0, wire_version = 0, features = 0;
    std::string version; // "major.minor.patch"
    std::string build_id; // the source commit the module reports
};

// What the library reports about itself, read by opening it (POSIX only; false
// with *error elsewhere). Opening runs no netplay code.
bool probe_netplay_module(const fs::path& library, NetplayModuleInfo& out, std::string* error);

// librecomp_net_module.so / .dylib, recomp_net_module.dll
std::string netplay_module_file_name();
// <data_dir>/netplay-module
fs::path netplay_module_dir(const Paths& paths);
// RETRO_NETPLAY_MODULE_MANIFEST_URL when set, else the newest release's.
std::string netplay_module_manifest_url();

struct ResolvedNetplayModule {
    fs::path path;           // empty when none usable
    NetplayModuleInfo info;
    std::string source;      // "override" | "updated" | "bundled"
    std::string note;        // why this one; what was skipped
};

// $RETRO_NETPLAY_MODULE when set; otherwise the newest usable of the module
// bundled beside the launcher and the updated ones, among those whose ABI is
// `wanted_abi`: the one the runner reports (ResolvedRunner::netplay_module_abi).
ResolvedNetplayModule resolve_netplay_module(const Paths& paths, const fs::path& exe_dir,
                                             std::uint32_t wanted_abi);

struct NetplayModuleUpdateResult {
    bool ok = false;
    bool updated = false;
    bool available = false;
    std::string current_version, latest_version, message;
    fs::path installed;
};

// Installs the module for the ABI the resolved runner loads. A runner with
// netplay linked in (ABI 0) needs none: nothing is fetched.
NetplayModuleUpdateResult update_netplay_module(const Paths& paths, const fs::path& exe_dir,
                                                bool check_only = false);

} // namespace retcomm
