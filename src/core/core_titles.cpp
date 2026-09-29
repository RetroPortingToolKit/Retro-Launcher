#include "retcomm/core_titles.hpp"
#include "retcomm/fs_util.hpp"

#include "core_manifest.hpp" // Retro-Runtime: retro_core_support

#include <nlohmann/json.hpp>

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

namespace {

// A title app's payload title.json (schema 1): what Play needs to know about
// it, and the ROM identity the library binds by.
bool read_title_json(const fs::path& json_path, CoreTitle& out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = json_path.string() + ": " + m;
        return false;
    };
    nlohmann::json j;
    try {
        std::ifstream in(json_path);
        if (!in) return fail("cannot be read");
        j = nlohmann::json::parse(in);
    } catch (const std::exception& e) {
        return fail(std::string("not JSON: ") + e.what());
    }
    if (j.value("schema", 0) != 1) return fail("not a schema 1 title.json");
    const fs::path root = fs::absolute(json_path).parent_path();
    out = CoreTitle{};
    out.id = j.value("id", "");
    if (out.id.empty()) return fail("names no id");
    out.name = j.value("name", out.id);
    out.platform = j.value("platform", "");
    out.manifest = fs::absolute(json_path);
    const std::string core = j.value("core", "");
    if (core.empty()) return fail("names no core");
    out.library = (root / core).lexically_normal();
    if (j.contains("package") && j["package"].is_string())
        out.package = (root / j["package"].get<std::string>()).lexically_normal();
    if (j.contains("title_dir") && j["title_dir"].is_string())
        out.title_dir = (root / j["title_dir"].get<std::string>()).lexically_normal();
    else
        out.title_dir = (out.package.empty() ? out.library : out.package).parent_path();
    if (j.contains("rom") && j["rom"].is_object()) {
        std::string sha = j["rom"].value("sha256", "");
        std::transform(sha.begin(), sha.end(), sha.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!sha.empty()) out.content_sha256.push_back(sha);
    }
    std::error_code ec;
    if (!fs::is_regular_file(out.library, ec)) return fail("its core " + out.library.string() + " is missing");
    // The core's own sidecar says which core and build it is.
    fs::path sidecar = out.library;
    sidecar.replace_extension(".rcore.toml");
    runner::CoreManifest m;
    if (runner::read_manifest(sidecar, m, nullptr)) {
        out.core_id = m.id;
        out.core_version = m.version;
        out.engine_dirty = m.engine_dirty;
        if (out.platform.empty() && !m.platforms.empty()) out.platform = m.platforms.front();
    }
    return true;
}

bool is_port_root(const fs::path& d) {
    std::error_code ec;
    return fs::is_regular_file(d / "game.toml", ec) && fs::is_regular_file(d / "CMakeLists.txt", ec) &&
           fs::is_regular_file(d / "tools" / "build_app.sh", ec);
}

} // namespace

bool read_core_title(const fs::path& manifest, CoreTitle& out, std::string* error) {
    if (manifest.extension() == ".json") return read_title_json(manifest, out, error);
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
    t.core_app = ct.app.string();
    t.core_generated = ct.generated;
    t.core_project = ct.project.string();
    t.core_rom = ct.rom.string();
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
        ct.app = ref.app;
        ct.generated = ref.generated;
        ct.project = ref.project;
        ct.rom = ref.rom;
        auto existing = std::find_if(catalog.titles.begin(), catalog.titles.end(),
                                     [&](const Title& t) { return t.id == ct.id; });
        if (existing != catalog.titles.end()) {
            // The catalog knows this title: it gains the core, keeping its
            // own identity and art rather than growing a twin.
            existing->core_manifest = ct.manifest.string();
            existing->core_app = ct.app.string();
            existing->core_generated = ct.generated;
            existing->core_project = ct.project.string();
            existing->core_rom = ct.rom.string();
            continue;
        }
        catalog.titles.push_back(title_from_core(ct));
    }
}

void unmerge_core_title(Catalog& catalog, const std::string& id) {
    for (auto it = catalog.titles.begin(); it != catalog.titles.end();) {
        if (it->id != id) {
            ++it;
            continue;
        }
        if (it->kind == "core") {
            it = catalog.titles.erase(it);
            continue;
        }
        it->core_manifest.clear();
        it->core_app.clear();
        it->core_generated = false;
        it->core_project.clear();
        it->core_rom.clear();
        ++it;
    }
}

bool find_adoptable_project(const fs::path& executable, AdoptableProject& out,
                            std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    std::error_code ec;
    const fs::path app = fs::weakly_canonical(fs::absolute(executable), ec);
    if (ec || !fs::is_regular_file(app, ec))
        return fail(executable.string() + " is not a file");
    // The project: the nearest folder above the app with a port's shape.
    fs::path root;
    for (fs::path d = app.parent_path(); !d.empty(); d = d.parent_path()) {
        if (is_port_root(d)) {
            root = d;
            break;
        }
        if (d == d.parent_path()) break;
    }
    if (root.empty())
        return fail(app.filename().string() + " is not inside a port project: no folder above "
                    "it has game.toml, CMakeLists.txt and tools/build_app.sh (an n64lle port "
                    "scaffolded by its tools/new_project).");
    // Its payload: title/ beside the app, where tools/build_app.sh stages it.
    const fs::path title_json = app.parent_path() / "title" / "title.json";
    if (!fs::is_regular_file(title_json, ec))
        return fail("no title/title.json beside " + app.filename().string() +
                    ": pick the app tools/build_app.sh built, in its app/ folder beside the "
                    "staged payload.");
    CoreTitle ct;
    std::string err;
    if (!read_title_json(title_json, ct, &err)) return fail(err);
    out.root = root;
    out.app = app;
    out.title_json = title_json;
    out.title = ct;
    out.title.app = app;
    return true;
}

bool move_folder(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    if (fs::exists(to, ec)) {
        if (error) *error = to.string() + " already exists";
        return false;
    }
    fs::create_directories(to.parent_path(), ec);
    retcomm::robust_rename(from, to, ec);
    if (!ec) return true;
    // Another filesystem (a second drive, a share): copy, then remove.
    ec.clear();
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) {
        const std::string why = ec.message();
        std::error_code ec2;
        fs::remove_all(to, ec2);
        if (error) *error = "cannot copy " + from.string() + " to " + to.string() + ": " + why;
        return false;
    }
    fs::remove_all(from, ec);
    if (ec) {
        if (error)
            *error = "copied to " + to.string() + ", but the original could not be removed (" +
                     ec.message() + "); remove " + from.string() + " yourself";
        return true; // the new copy is complete and is the one registered
    }
    return true;
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
