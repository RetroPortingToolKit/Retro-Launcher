#pragma once

// What every release-manifest reader here shares: the runtime updater
// (retro-core-runner), and Direct mode's Update page (the hub itself and an
// n64lle core, src/hub/hub_update.cpp). The manifests are one family
// (Retro-Runtime runtime-manifest.json, Retro-Launcher hub-manifest.json,
// n64lle n64lle-release-manifest.json) and are read by the same rules:
// newest release from the feed, size and SHA-256 checked before anything is
// opened, requirements checked against this machine.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace retcomm::update_detail {

namespace fs = std::filesystem;

bool is_file_url(const std::string& url);
fs::path file_url_path(const std::string& url);

// A release version orders by its numbers; anything else (a "dev" build, an
// empty report) is older than every release.
bool is_release_version(const std::string& v);
int version_cmp(const std::string& a, const std::string& b);

// Why this machine cannot run a manifest entry (its `requires`), or empty.
std::string unmet_requirement(const nlohmann::json& reqs);

// The newest vX.Y.Z tag of a repository, pre-releases included: the highest
// tag in https://github.com/<slug>/releases.atom (n64lle docs/RELEASES.md §2:
// `releases/latest` skips pre-releases, and the REST API's anonymous quota is
// 60 an hour). Falls back to the API -- which a GITHUB_TOKEN reaches for a
// private repository -- when the feed cannot be read.
std::string newest_release_tag(const std::string& slug, std::string* error);

// Where a manifest is: $<env_var> when set (any URL libcurl reads, file://
// included, for testing), else <asset> under the newest release's tag. Empty
// with *error set when neither answers.
std::string resolve_manifest_url(const char* env_var, const std::string& slug,
                                 const std::string& asset, std::string* error);

// GET a manifest (http(s) or file://) and parse it. False with *error set.
bool fetch_manifest(const std::string& url, nlohmann::json& out, std::string* error);

// Downloads `url` to `dest` and checks size and SHA-256 before returning; a
// mismatch deletes the file. A file:// archive is accepted only from a
// file:// manifest (`local_manifest`), which only a test names.
bool download_checked(const std::string& url, const fs::path& dest, std::uint64_t want_size,
                      const std::string& want_sha256, bool local_manifest, std::string* error);

// Keeps the newest `keep` <root>/<release version> directories, removes the
// rest. A directory still in use (Windows) fails quietly and is retried later.
void prune_versions(const fs::path& root, std::size_t keep);

} // namespace retcomm::update_detail
