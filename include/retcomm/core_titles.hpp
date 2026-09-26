#pragma once

// Titles that run through an rcore core in the hub's own window
// (docs/CORE_LIBRARY.md). A core's generated sidecar (<stem>.rcore.toml)
// already says which title it is and which ROMs it was generated from, so a
// core title is a catalog Title synthesized from it: the existing ROM scan,
// library binding, rows and cover art then treat it like any other title.

#include "retcomm/catalog.hpp"
#include "retcomm/config.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace retcomm {

namespace fs = std::filesystem;

struct CoreTitle {
    std::string id;       // [title] id
    std::string name;     // display name: the registration's, else the id
    std::string platform; // [core] platforms[0]
    fs::path manifest;    // the .rcore.toml
    fs::path library;     // beside it, as [core] library names it
    fs::path title_dir;   // [title] dir, resolved against the manifest's folder
    std::vector<std::string> content_sha256;
    std::string core_id, core_version;
    bool engine_dirty = false;
};

// Reads a per-title core's sidecar. Refuses a generic core (no [title]),
// which needs a game package this launcher does not build yet.
bool read_core_title(const fs::path& manifest, CoreTitle& out, std::string* error);

// A catalog Title for a registered core title: kind "core", its platform,
// ROM identity = the manifest's content hashes, core_manifest set.
Title title_from_core(const CoreTitle& ct);

// Adds every registered core title to `catalog`. Where the catalog already has
// a title with the same id, that title gains the core (core_manifest) instead
// of a duplicate row. Problems are appended to *log, one line each.
void merge_core_titles(Catalog& catalog, const AppConfig& cfg, std::vector<std::string>* log);

// The sidecar a title runs through, if any: a core title's own, or a
// *.rcore.toml with a [title] matching the title's id inside its install.
fs::path core_manifest_for(const Title& t, const fs::path& install_dir);

} // namespace retcomm
