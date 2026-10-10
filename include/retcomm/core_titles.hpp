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
    fs::path package; // a title app's game package (title.json `package`)
    fs::path app;     // the registration's title app, when it has one
};

// Reads a per-title core's sidecar, or a title app's payload title.json
// (Retro-Launcher docs/RELEASES.md, "Title-app mode": the generic core, its
// game package and the ROM identity). Refuses a generic core's sidecar (no
// [title]): alone it names no game.
bool read_core_title(const fs::path& manifest, CoreTitle& out, std::string* error);

// A catalog Title for a registered core title: kind "core", its platform,
// ROM identity = the manifest's content hashes, core_manifest set.
Title title_from_core(const CoreTitle& ct);

// Undoes merge_core_titles for one title: a title only a registration made
// leaves the catalog; a catalog title a core attached to loses the core.
void unmerge_core_title(Catalog& catalog, const std::string& id);

// Adds every registered core title to `catalog`. Where the catalog already has
// a title with the same id, that title gains the core (core_manifest) instead
// of a duplicate row. Problems are appended to *log, one line each.
void merge_core_titles(Catalog& catalog, const AppConfig& cfg, std::vector<std::string>* log);

// Whether this device can play the title. On Android only a title with a core
// (core_manifest: an installed title bundle) can: the catalog lists desktop
// builds and has no Android entry yet. Elsewhere every title.
bool title_runs_on_host(const Title& t);

// Removes the titles title_runs_on_host refuses; returns how many. The hub runs
// it after every catalog load and core change, so the library, the Home cards,
// box art, ROM scans and update checks only ever see what plays here.
std::size_t drop_titles_not_on_host(Catalog& catalog);

// The sidecar a title runs through, if any: a core title's own, or a
// *.rcore.toml with a [title] matching the title's id inside its install.
fs::path core_manifest_for(const Title& t, const fs::path& install_dir);

} // namespace retcomm
