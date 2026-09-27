#include "hub/hub_update.hpp"
#include "retcomm/fs_util.hpp"

#include "hub/hub_title.hpp"
#include "update/manifest_util.hpp"

#include "retcomm/install.hpp"
#include "retcomm/runtime_update.hpp"
#include "retcomm/self_update.hpp"

#include "link_protocol.hpp" // Retro-Runtime corelink: kProtocolMajor
#include "rcore/rcore.h"
#include "transport.hpp"     // Retro-Runtime corelink: path_utf8

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <vector>

namespace retcomm::hub {

namespace {

using nlohmann::json;
using update_detail::is_file_url;
using update_detail::is_release_version;
using update_detail::version_cmp;
namespace corelink = retro::corelink;

// A core this hub knows where to find releases of, by its sidecar id.
struct CoreChannel {
    const char* id;
    const char* slug;
    const char* manifest;
};
constexpr CoreChannel kCoreChannels[] = {
    {"n64lle", "RetroPortingToolKit/n64lle", "n64lle-release-manifest.json"},
};

std::string strip_v(const std::string& tag) {
    return !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
}

std::string hub_file_name() {
#if defined(_WIN32)
    return "retro-hub.exe";
#else
    return "retro-hub";
#endif
}

// `key = value` lines of one [section], values unquoted: the generated TOML
// the sidecars are, never TOML in general.
std::map<std::string, std::string> toml_section(const fs::path& p, const std::string& want) {
    std::map<std::string, std::string> out;
    std::ifstream in(p);
    std::string line, section;
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
        return s;
    };
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t[0] == '[') {
            const auto e = t.find(']');
            section = e == std::string::npos ? "" : t.substr(1, e - 1);
            continue;
        }
        const auto eq = t.find('=');
        if (section != want || eq == std::string::npos) continue;
        std::string v = trim(t.substr(eq + 1));
        if (!v.empty() && v[0] == '"') {
            const auto close = v.find('"', 1);
            v = close == std::string::npos ? v.substr(1) : v.substr(1, close - 1);
        } else {
            v = trim(v.substr(0, v.find('#')));
        }
        out[trim(t.substr(0, eq))] = v;
    }
    return out;
}

std::string field(const std::map<std::string, std::string>& m, const char* k) {
    const auto it = m.find(k);
    return it == m.end() ? std::string() : it->second;
}

int to_int(const std::string& s, int fallback) {
    try {
        size_t used = 0;
        const int v = std::stoi(s, &used);
        return used == s.size() ? v : fallback;
    } catch (...) {
        return fallback;
    }
}

// The release directories under `root`, newest first.
std::vector<fs::path> version_dirs(const fs::path& root) {
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory() && is_release_version(name)) dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return version_cmp(a.filename().string(), b.filename().string()) > 0;
    });
    return dirs;
}

// Why the entry for this platform cannot be installed, or empty. Sets
// item.ok/message for the cases that are an answer rather than a failure.
bool platform_entry(const json& m, const char* table, UpdateItem& item, const std::string& what,
                    const json** entry) {
    const std::string key = runtime_platform_key();
    if (!m.contains(table) || !m[table].is_object() || !m[table].contains(key)) {
        item.message = what + ": the manifest has no entry for " + key;
        return false;
    }
    const json& e = m[table][key];
    if (e.contains("unavailable")) {
        item.ok = true;
        item.message = what + " " + item.latest + " is not published for " + key + " (" +
                       e["unavailable"].get<std::string>() + ")";
        return false;
    }
    if (const std::string why = update_detail::unmet_requirement(e.value("requires", json::object()));
        !why.empty()) {
        item.ok = true;
        item.message = what + " " + item.latest + " cannot run here: " + why;
        return false;
    }
    *entry = &e;
    return true;
}

bool safe_name(const std::string& n) {
    return !n.empty() && n != "." && n != ".." && n.find_first_of("/\\") == std::string::npos;
}

} // namespace

fs::path hub_updates_dir(const Paths& paths) { return paths.data_dir / "hub"; }
fs::path core_updates_dir(const Paths& paths) { return paths.data_dir / "core"; }

bool read_core_sidecar(const fs::path& core, CoreSidecar& out, std::string* error) {
    fs::path m = core;
    m.replace_extension(".rcore.toml");
    std::error_code ec;
    if (!fs::is_regular_file(m, ec)) {
        if (error) *error = corelink::path_utf8(m) + " does not exist";
        return false;
    }
    const auto sec = toml_section(m, "core");
    out.id = field(sec, "id");
    out.version = field(sec, "version");
    out.library = field(sec, "library");
    if (out.library.empty()) out.library = core.filename().string();
    if (out.id.empty()) {
        if (error) *error = corelink::path_utf8(m) + " names no [core] id";
        return false;
    }
    return true;
}

int package_module_abi(const fs::path& package) {
    if (package.empty()) return -1;
    const fs::path desc = package.parent_path() / (package.stem().string() + ".n64game.toml");
    return to_int(field(toml_section(desc, "package"), "module_abi"), -1);
}

ResolvedCore resolve_title_core(const Paths& paths, const fs::path& bundled,
                                const fs::path& package) {
    ResolvedCore out;
    out.path = bundled;
    out.source = "bundled";
    CoreSidecar b;
    std::string err;
    if (!read_core_sidecar(bundled, b, &err)) {
        out.note = err;
        return out;
    }
    out.version = b.version;
    if (package.empty()) {
        out.note = "no game package: this core is the game";
        return out;
    }
    const int abi = package_module_abi(package);
    if (abi < 0) {
        out.note = "the game package records no module_abi";
        return out;
    }
    for (const fs::path& dir : version_dirs(core_updates_dir(paths))) {
        const std::string v = dir.filename().string();
        if (version_cmp(v, out.version) <= 0) break; // newest first: nothing newer follows
        json info;
        try {
            std::ifstream in(dir / "installed.json");
            info = json::parse(in);
        } catch (...) {
            continue;
        }
        if (info.value("core_id", "") != b.id || info.value("module_abi", -1) != abi) continue;
        const fs::path lib = dir / b.library;
        CoreSidecar s;
        if (!read_core_sidecar(lib, s, nullptr) || s.id != b.id || s.version != v) continue;
        std::error_code ec;
        if (!fs::is_regular_file(lib, ec)) continue;
        out.path = lib;
        out.version = v;
        out.source = "updated";
        out.note = "updated core " + v + " (bundled " + b.version + ")";
        return out;
    }
    out.note = "bundled core " + b.version;
    return out;
}

InstalledHub newest_installed_hub(const Paths& paths, const std::string& self_version,
                                  bool title_mode) {
    for (const fs::path& dir : version_dirs(hub_updates_dir(paths))) {
        const std::string v = dir.filename().string();
        if (version_cmp(v, self_version) <= 0) break;
        const fs::path exe = dir / hub_file_name();
        std::error_code ec;
        if (!fs::is_regular_file(exe, ec)) continue;
        const auto ver = probe_version(exe);
        const auto num = [&](const char* k) {
            const auto it = ver.find(k);
            return it == ver.end() ? 0 : to_int(it->second, 0);
        };
        if (ver.empty() || field(ver, "version") != v || num("direct_mode") < 4 ||
            num("updates") < 1)
            continue;
        if (title_mode && num("title_app") < 1) continue;
        return {exe, v};
    }
    return {};
}

// ---- the game -------------------------------------------------------------------

UpdateItem check_game(const UpdateTarget& t) {
    UpdateItem item;
    item.ok = true;
    if (!t.title_mode) {
        item.message = "Direct mode plays the build its command line names; update it where it "
                       "was built.";
        return item;
    }
    item.current = t.title_version;
    if (t.game_github.empty()) {
        item.message = "This app's title.json names no update source (update.github).";
        return item;
    }
    std::string err;
    const std::string tag = update_detail::newest_release_tag(t.game_github, &err);
    if (tag.empty()) {
        item.ok = false;
        item.message = err;
        return item;
    }
    item.latest = strip_v(tag);
    item.link = "https://github.com/" + t.game_github + "/releases/tag/" + tag;
    if (version_cmp(item.latest, strip_v(t.title_version)) > 0) {
        item.manual = true;
        item.message = t.game_github + " has published " + tag +
                       ". The game is built from your ROM, so it is not downloaded: build this "
                       "app again from that release.";
    } else {
        item.message = "Up to date.";
    }
    return item;
}

// ---- the core -------------------------------------------------------------------

UpdateItem update_core(const UpdateTarget& t, bool install) {
    UpdateItem item;
    CoreSidecar cur;
    std::string err;
    if (!read_core_sidecar(t.core, cur, &err)) {
        item.message = "Cannot read the core's sidecar: " + err;
        return item;
    }
    item.current = cur.version + (t.core_dev ? " (dev)" : "");
    if (t.core_pinned) {
        item.ok = true;
        item.message = "The command line names this core (--run-core / --core); it is not "
                       "replaced.";
        return item;
    }
    if (t.package.empty()) {
        item.ok = true;
        item.message = "This title's core is the game itself (no game package); it is updated "
                       "with the game.";
        return item;
    }
    const CoreChannel* ch = nullptr;
    for (const CoreChannel& c : kCoreChannels)
        if (cur.id == c.id) ch = &c;
    if (!ch) {
        item.ok = true;
        item.message = "No release channel is known for core '" + cur.id + "'.";
        return item;
    }
    const int abi = package_module_abi(t.package);

    std::string url = update_detail::resolve_manifest_url("RETRO_CORE_MANIFEST_URL", ch->slug,
                                                          ch->manifest, &err);
    json m;
    if (url.empty() || !update_detail::fetch_manifest(url, m, &err)) {
        item.message = "Cannot read " + std::string(ch->id) + "'s releases: " + err +
                       ". Its repository is not public yet; this works once it publishes "
                       "releases publicly (or with a GITHUB_TOKEN that can read it).";
        return item;
    }
    const bool local = is_file_url(url);
    if (m.value("schema", 0) != 1) {
        item.message = "Core manifest schema " + std::to_string(m.value("schema", 0)) +
                       "; this hub reads 1 (update the hub first)";
        return item;
    }
    if (m.value("name", "") != cur.id) {
        item.message = "The manifest is for '" + m.value("name", "") + "', not '" + cur.id + "'";
        return item;
    }
    item.latest = m.value("version", "");
    const std::string what = "Core";

    // Installed by an earlier Update and not yet started. A dev core is
    // always offered the release instead (installing it clears the dev path).
    const ResolvedCore next = resolve_title_core(t.paths, t.bundled_core, t.package);
    if (!t.core_dev && next.source == "updated" && next.path != t.core &&
        version_cmp(next.version, cur.version) > 0 && version_cmp(next.version, item.latest) >= 0) {
        item.ok = true;
        item.installed = true;
        item.message = next.version + " is installed; start again to use it.";
        return item;
    }
    if (!t.core_dev && version_cmp(item.latest, cur.version) <= 0) {
        item.ok = true;
        item.message = "Up to date.";
        return item;
    }
    // A newer core that cannot run this game is reported, never installed.
    const json& rabi = m.value("rcore_abi", json::object());
    if (rabi.value("major", -1) != static_cast<int>(RCORE_ABI_MAJOR) ||
        rabi.value("draft_revision", -1) != static_cast<int>(RCORE_DRAFT_REVISION)) {
        item.ok = true;
        item.message = what + " " + item.latest + " is built for rcore ABI " +
                       std::to_string(rabi.value("major", -1)) + " draft " +
                       std::to_string(rabi.value("draft_revision", -1)) + "; this hub is " +
                       std::to_string(RCORE_ABI_MAJOR) + " draft " +
                       std::to_string(RCORE_DRAFT_REVISION) + ". Update the hub first.";
        return item;
    }
    if (abi < 0) {
        item.ok = true;
        item.message = what + " " + item.latest + " is published, but this game package records "
                       "no module_abi, so it cannot be matched to one.";
        return item;
    }
    if (m.value("module_abi", -1) != abi) {
        item.ok = true;
        item.message = what + " " + item.latest + " loads module ABI " +
                       std::to_string(m.value("module_abi", -1)) + "; this game is built for " +
                       std::to_string(abi) + ". It needs the game rebuilt, so it is not installed.";
        return item;
    }
    const json* e = nullptr;
    if (!platform_entry(m, "core", item, what, &e)) return item;
    item.available = true;
    if (!install) {
        item.ok = true;
        item.message = t.core_dev ? "A dev core is in use; installing release " + item.latest +
                                        " replaces it."
                                  : cur.version + " -> " + item.latest + " available.";
        return item;
    }

    // ---- install: download, check, take the core and its sidecar only ----------
    const std::string archive = e->value("archive", "");
    const std::string archive_url = e->value("url", "");
    const std::string sha = e->value("sha256", "");
    const std::uint64_t size = e->value("size", std::uint64_t{0});
    if (!safe_name(archive) || archive_url.empty() || sha.size() != 64 || !size ||
        !is_release_version(item.latest) || !safe_name(item.latest)) {
        item.available = false;
        item.message = "The core manifest's entry is incomplete";
        return item;
    }
    const fs::path root = core_updates_dir(t.paths);
    const fs::path download = root / ".download" / archive;
    const fs::path extract = root / (".extract-" + item.latest);
    const fs::path staging = root / (".staging-" + item.latest);
    std::error_code ec;
    auto discard = [&](const std::string& why) {
        fs::remove_all(extract, ec);
        fs::remove_all(staging, ec);
        fs::remove(download, ec);
        item.ok = false;
        item.message = why;
        return item;
    };
    if (!update_detail::download_checked(archive_url, download, size, sha, local, &err))
        return discard(err);
    fs::remove_all(extract, ec);
    if (!extract_archive_to(download, extract, &err)) return discard("cannot extract " + archive + ": " + err);
    fs::path lib_dir = extract / e->value("root", "") / "lib";
    if (!fs::is_directory(lib_dir, ec)) lib_dir = extract / "lib";
    const fs::path lib = lib_dir / cur.library;
    CoreSidecar got;
    if (!fs::is_regular_file(lib, ec) || !read_core_sidecar(lib, got, &err))
        return discard("the release has no lib/" + cur.library + " with its sidecar");
    if (got.id != cur.id || got.version != item.latest)
        return discard("the release's core reports " + got.id + " " + got.version +
                       ", the manifest says " + cur.id + " " + item.latest + "; not installed");
    fs::path sidecar = lib;
    sidecar.replace_extension(".rcore.toml");
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    fs::copy_file(lib, staging / cur.library, ec);
    if (!ec) fs::copy_file(sidecar, staging / sidecar.filename(), ec);
    if (ec) return discard("cannot stage the core: " + ec.message());
    {
        json info = {{"schema", 1},
                     {"core_id", cur.id},
                     {"version", item.latest},
                     {"commit", m.value("commit", "")},
                     {"module_abi", abi}};
        std::ofstream(staging / "installed.json") << info.dump(2) << "\n";
    }
    const fs::path dest = root / item.latest;
    fs::remove_all(dest, ec);
    retcomm::robust_rename(staging, dest, ec);
    if (ec) return discard("cannot move the core into " + corelink::path_utf8(dest) + ": " + ec.message());
    fs::remove_all(extract, ec);
    fs::remove(download, ec);
    update_detail::prune_versions(root, 2);
    item.ok = true;
    item.available = false;
    item.installed = true;
    item.message = "Installed " + item.latest + "; start again to use it.";
    return item;
}

// ---- the runner -----------------------------------------------------------------

UpdateItem update_runner(const UpdateTarget& t, bool install) {
    UpdateItem item;
    const RuntimeUpdateResult r = update_runtime(t.paths, t.exe_dir, !install);
    item.current = r.current_version == "none" ? std::string() : r.current_version;
    item.latest = r.latest_version;
    item.ok = r.ok;
    item.available = r.available && !r.updated;
    item.installed = r.updated;
    item.message = r.updated ? "Installed " + r.latest_version + "; the next Play uses it."
                             : r.message;
    return item;
}

// ---- the hub --------------------------------------------------------------------

UpdateItem update_hub(const UpdateTarget& t, bool install) {
    UpdateItem item;
    item.current = t.hub_version + (t.hub_dev ? " (dev)" : "");
    if (!t.hub_dev && !t.title_mode && (t.hub_commit.empty() || t.hub_commit == "unknown")) {
        item.ok = true;
        item.message = "A development build (no commit recorded); it is updated by building it.";
        return item;
    }
    std::string err;
    const std::string url = update_detail::resolve_manifest_url(
        "RETRO_HUB_MANIFEST_URL", retcomm_github_slug(), "hub-manifest.json", &err);
    json m;
    if (url.empty() || !update_detail::fetch_manifest(url, m, &err)) {
        item.message = "Cannot read the hub's releases: " + err;
        return item;
    }
    const bool local = is_file_url(url);
    if (m.value("schema", 0) != 1) {
        item.message = "Hub manifest schema " + std::to_string(m.value("schema", 0)) +
                       "; this hub reads 1";
        return item;
    }
    item.latest = m.value("version", "");
    const std::string commit = m.value("commit", "");
    const std::string what = "Hub";

    const InstalledHub pending =
        t.hub_dev ? InstalledHub{} : newest_installed_hub(t.paths, t.hub_version, t.title_mode);
    if (!pending.path.empty() && version_cmp(pending.version, item.latest) >= 0) {
        item.ok = true;
        item.installed = true;
        item.message = pending.version + " is installed; start again to use it.";
        return item;
    }
    if (!t.hub_dev && version_cmp(item.latest, t.hub_version) <= 0) {
        item.ok = true;
        item.message = "Up to date.";
        return item;
    }
    // It has to drive the runner and cores this one does, and run a title.
    const int link = m.value("link_protocol", json::object()).value("major", -1);
    const int rabi = m.value("rcore_abi", json::object()).value("major", -1);
    const int cli = m.value("direct_mode", json::object()).value("cli_revision", 0);
    if (link != static_cast<int>(corelink::kProtocolMajor) || rabi != static_cast<int>(RCORE_ABI_MAJOR)) {
        item.ok = true;
        item.message = what + " " + item.latest + " speaks link " + std::to_string(link) +
                       ", rcore ABI " + std::to_string(rabi) + "; the runner and core here speak " +
                       std::to_string(corelink::kProtocolMajor) + ", " +
                       std::to_string(RCORE_ABI_MAJOR) + ". Not installed.";
        return item;
    }
    if (cli < 4) {
        item.ok = true;
        item.message = what + " " + item.latest + " has Direct mode revision " +
                       std::to_string(cli) + "; this app needs 4.";
        return item;
    }
    // A hub without this page would leave the app no way to update again.
    if (m.value("direct_mode", json::object()).value("updates", 0) < 1) {
        item.ok = true;
        item.message = what + " " + item.latest + " is published, but it predates in-app updates: "
                       "running it would leave this app no Update button. Not installed.";
        return item;
    }
    const json* e = nullptr;
    if (!platform_entry(m, "platforms", item, what, &e)) return item;
    item.available = true;
    if (!install) {
        item.ok = true;
        item.message = t.hub_dev ? "A dev hub is in use; installing release " + item.latest +
                                       " replaces it."
                                 : t.hub_version + " -> " + item.latest + " available.";
        return item;
    }

    const std::string archive = e->value("archive", "");
    const std::string archive_url = e->value("url", "");
    const std::string sha = e->value("sha256", "");
    const std::uint64_t size = e->value("size", std::uint64_t{0});
    if (!safe_name(archive) || archive_url.empty() || sha.size() != 64 || !size ||
        e->value("executable", "") != hub_file_name() || !is_release_version(item.latest) ||
        !safe_name(item.latest)) {
        item.available = false;
        item.message = "The hub manifest's entry is incomplete or names an unexpected file";
        return item;
    }
    const fs::path root = hub_updates_dir(t.paths);
    const fs::path download = root / ".download" / archive;
    const fs::path staging = root / (".staging-" + item.latest);
    std::error_code ec;
    auto discard = [&](const std::string& why) {
        fs::remove_all(staging, ec);
        fs::remove(download, ec);
        item.ok = false;
        item.message = why;
        return item;
    };
    if (!update_detail::download_checked(archive_url, download, size, sha, local, &err))
        return discard(err);
    fs::remove_all(staging, ec);
    if (!extract_archive_to(download, staging, &err)) return discard("cannot extract " + archive + ": " + err);
    const fs::path exe = staging / hub_file_name();
#if !defined(_WIN32)
    fs::permissions(exe, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
#endif
    // Believe only what the hub itself reports.
    std::string perr;
    const auto ver = probe_version(exe, &perr);
    if (ver.empty()) return discard("the downloaded hub does not run: " + perr);
    const auto num = [&](const char* k) {
        const auto it = ver.find(k);
        return it == ver.end() ? 0 : to_int(it->second, 0);
    };
    if (field(ver, "version") != item.latest || (!commit.empty() && field(ver, "commit") != commit) ||
        num("direct_mode") < 4 || num("updates") < 1 || (t.title_mode && num("title_app") < 1))
        return discard("the downloaded hub reports version " + field(ver, "version") + " commit " +
                       field(ver, "commit") + ", the manifest says " + item.latest + " commit " +
                       commit + "; not installed");
    const fs::path dest = root / item.latest;
    fs::remove_all(dest, ec);
    retcomm::robust_rename(staging, dest, ec);
    if (ec) return discard("cannot move the hub into " + corelink::path_utf8(dest) + ": " + ec.message());
    fs::remove(download, ec);
    update_detail::prune_versions(root, 2);
    item.ok = true;
    item.available = false;
    item.installed = true;
    item.message = "Installed " + item.latest + "; start again to use it.";
    return item;
}

} // namespace retcomm::hub
