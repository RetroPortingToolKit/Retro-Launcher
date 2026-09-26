#include "retcomm/core_titles.hpp"

#include "../runner/core_manifest.hpp"

#include <algorithm>
#include <fstream>

namespace retcomm {

namespace runner = ::retro::runner;

namespace {

// [game] name from the title's game.toml, the port's own display name.
std::string game_toml_name(const fs::path& title_dir) {
    std::ifstream in(title_dir / "game.toml");
    std::string line;
    bool in_game = false;
    while (std::getline(in, line)) {
        const auto b = line.find_first_not_of(" \t");
        if (b == std::string::npos || line[b] == '#') continue;
        if (line[b] == '[') {
            in_game = line.compare(b, 6, "[game]") == 0;
            continue;
        }
        if (!in_game || line.compare(b, 4, "name") != 0) continue;
        const auto q1 = line.find('"', b);
        const auto q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
        if (q2 != std::string::npos) return line.substr(q1 + 1, q2 - q1 - 1);
    }
    return {};
}

} // namespace

bool read_core_title(const fs::path& manifest, CoreTitle& out, std::string* error) {
    runner::CoreManifest m;
    if (!runner::read_manifest(manifest, m, error)) return false;
    if (!m.has_title || m.title_id.empty()) {
        if (error) {
            *error = manifest.string() +
                     ": a generic core (no [title]); it needs a game package, which this "
                     "launcher does not build yet";
        }
        return false;
    }
    out = CoreTitle{};
    out.id = m.title_id;
    out.name = m.title_id;
    out.platform = m.platforms.empty() ? std::string() : m.platforms.front();
    out.manifest = fs::absolute(manifest);
    out.library = out.manifest.parent_path() / m.library;
    out.title_dir = (out.manifest.parent_path() / (m.title_dir.empty() ? "." : m.title_dir))
                        .lexically_normal();
    if (std::string n = game_toml_name(out.title_dir); !n.empty()) out.name = n;
    out.content_sha256 = m.content_sha256;
    for (auto& h : out.content_sha256) {
        std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });
    }
    out.core_id = m.id;
    out.core_version = m.version;
    out.engine_dirty = m.engine_dirty;
    return true;
}

Title title_from_core(const CoreTitle& ct) {
    Title t;
    t.id = ct.id;
    t.name = ct.name;
    t.kind = "core";
    t.platform = ct.platform;
    t.description = "Runs in Retro through the " + ct.core_id + " core (" + ct.core_version +
                    (ct.engine_dirty ? ", development build" : "") + ").";
    t.rom_identity.sha256 = ct.content_sha256;
    // The content hashes are of the big-endian image; only that byte order
    // can match them.
    if (ct.platform == "n64") t.rom_extensions = {".z64"};
    t.install_dir_name = ct.id;
    t.core_manifest = ct.manifest.string();
    return t;
}

void merge_core_titles(Catalog& catalog, const AppConfig& cfg, std::vector<std::string>* log) {
    for (const CoreTitleRef& ref : cfg.core_titles) {
        CoreTitle ct;
        std::string err;
        if (!read_core_title(ref.manifest, ct, &err)) {
            if (log) log->push_back("core title skipped: " + err);
            continue;
        }
        if (!ref.name.empty()) ct.name = ref.name;
        auto existing = std::find_if(catalog.titles.begin(), catalog.titles.end(),
                                     [&](const Title& t) { return t.id == ct.id; });
        if (existing != catalog.titles.end()) {
            // The catalog knows this title: it gains the core, keeping its
            // own identity and art rather than growing a twin.
            existing->core_manifest = ct.manifest.string();
            continue;
        }
        catalog.titles.push_back(title_from_core(ct));
    }
}

fs::path core_manifest_for(const Title& t, const fs::path& install_dir) {
    if (!t.core_manifest.empty()) return t.core_manifest;
    std::error_code ec;
    if (install_dir.empty() || !fs::is_directory(install_dir, ec)) return {};
    for (const auto& e : fs::directory_iterator(install_dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() <= 11 || name.compare(name.size() - 11, 11, ".rcore.toml") != 0) continue;
        CoreTitle ct;
        if (read_core_title(e.path(), ct, nullptr) && ct.id == t.id) return e.path();
    }
    return {};
}

} // namespace retcomm
