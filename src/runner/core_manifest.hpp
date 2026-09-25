#pragma once

// The sidecar manifest (docs/CORE_ABI.md, "Sidecar manifest"): what the host
// reads at boot without loading the library, and what the runner checks,
// field for field, against the library's own rcore_core_info at load.

#include "core_library.hpp"

#include <map>
#include <string>
#include <vector>

namespace retcomm::runner {

struct CoreManifest {
    fs::path path;
    // [core]
    long abi_major = -1;
    long draft_revision = -1;
    std::string id;
    std::string version;
    std::string library;
    std::vector<std::string> platforms;
    std::vector<std::string> capabilities;
    bool has_state_compat_id = false;
    std::string state_compat_id;
    // [title], present only for a per-title core
    bool has_title = false;
    std::string title_id;
    std::vector<std::string> content_sha256;
    std::string title_dir;
    // [build]
    std::string engine_commit;
    bool engine_dirty = false;
    std::string toolchain;
    std::string generated_utc;
};

// <dir>/<library stem>.rcore.toml
fs::path manifest_path_for(const fs::path& library);

// Reads the generated grammar and nothing looser: [section] headers,
// `key = value` with a quoted string, integer, bool, or one-line array of
// quoted strings. Anything else is a refusal naming the line, because the
// file is generated and a line it does not recognise means the generator and
// this reader disagree.
bool read_manifest(const fs::path& path, CoreManifest& out, std::string* error);

// Every disagreement between the manifest and the loaded core, one line each,
// naming both values. Empty = they agree.
std::vector<std::string> verify_manifest(const CoreManifest& m, const LoadedCore& core);

} // namespace retcomm::runner
