#include "retcomm/netplay_module.hpp"

#include "manifest_util.hpp"

#include "retcomm/fs_util.hpp"
#include "retcomm/install.hpp"
#include "retcomm/release_tags.hpp"
#include "retcomm/runtime_update.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace retcomm {
namespace {

using nlohmann::json;
using update_detail::is_file_url;
using update_detail::is_release_version;
using update_detail::unmet_requirement;
using update_detail::version_cmp;

constexpr const char* kSlug = "RetroPortingToolKit/recomp-net";
constexpr const char* kAsset = "netplay-module-manifest.json";
constexpr int kManifestSchema = 1;

// recomp-net module.h RNetModuleInfo, 64-bit layout (pinned by its
// layout_check in n64lle). Read by size, never beyond what the module wrote.
struct RawInfo {
    std::uint32_t struct_size, abi_version, abi_minor, wire_version, features;
    std::uint32_t major, minor, patch;
    const char* build_id;
};

std::string utf8(const fs::path& p) { return p.string(); }

} // namespace

bool probe_netplay_module(const fs::path& library, NetplayModuleInfo& out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = utf8(library) + ": " + m;
        return false;
    };
#if defined(_WIN32)
    return fail("netplay modules are not supported on this platform yet");
#else
    if (sizeof(void*) != 8) return fail("a 64-bit build is required");
    void* h = dlopen(utf8(library).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        return fail(e ? e : "cannot open");
    }
    using InfoFn = std::size_t (*)(RawInfo*, std::size_t);
    auto fn = reinterpret_cast<InfoFn>(dlsym(h, "rnet_module_info"));
    if (!fn) {
        dlclose(h);
        return fail("has no rnet_module_info (not a netplay module)");
    }
    RawInfo raw{};
    const std::size_t got = fn(&raw, sizeof raw);
    if (got < 5 * sizeof(std::uint32_t)) {
        dlclose(h);
        return fail("module info truncated");
    }
    out.abi_version = raw.abi_version;
    out.abi_minor = raw.abi_minor;
    out.wire_version = raw.wire_version;
    out.features = raw.features;
    out.version = std::to_string(raw.major) + "." + std::to_string(raw.minor) + "." +
                  std::to_string(raw.patch);
    out.build_id = (got >= sizeof raw && raw.build_id) ? raw.build_id : "unknown";
    dlclose(h);
    return true;
#endif
}

std::string netplay_module_file_name() {
#if defined(_WIN32)
    return "recomp_net_module.dll";
#elif defined(__APPLE__)
    return "librecomp_net_module.dylib";
#else
    return "librecomp_net_module.so";
#endif
}

fs::path netplay_module_dir(const Paths& paths) { return paths.data_dir / "netplay-module"; }

std::string netplay_module_manifest_url() {
    std::string err;
    const std::string u = update_detail::resolve_manifest_url("RETRO_NETPLAY_MODULE_MANIFEST_URL", kSlug,
                                                              kAsset, &err);
    return u;
}

ResolvedNetplayModule resolve_netplay_module(const Paths& paths, const fs::path& exe_dir) {
    ResolvedNetplayModule out;
    if (const char* env = std::getenv("RETRO_NETPLAY_MODULE"); env && *env) {
        out.path = env;
        out.source = "override";
        std::string err;
        if (!probe_netplay_module(out.path, out.info, &err)) out.note = err;
        else out.note = "RETRO_NETPLAY_MODULE names it";
        return out;
    }
    struct Cand {
        fs::path path;
        NetplayModuleInfo info;
        std::string source;
    };
    std::vector<Cand> found;
    std::vector<std::string> skipped;
    auto consider = [&](const fs::path& p, const char* source) {
        NetplayModuleInfo i;
        std::string err;
        if (!probe_netplay_module(p, i, &err)) {
            skipped.push_back(err);
            return false;
        }
        if (i.abi_version != kNetplayModuleAbi) {
            skipped.push_back(utf8(p) + ": module ABI " + std::to_string(i.abi_version) +
                              ", the runners here need " + std::to_string(kNetplayModuleAbi));
            return false;
        }
        found.push_back({p, i, source});
        return true;
    };
    std::error_code ec;
    const fs::path bundled = exe_dir / netplay_module_file_name();
    if (fs::is_regular_file(bundled, ec)) consider(bundled, "bundled");

    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(netplay_module_dir(paths), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory() && is_release_version(it->path().filename().string()))
            dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return release_tag_cmp(a.filename().string(), b.filename().string()) > 0;
    });
    const std::size_t before = found.size();
    for (const auto& d : dirs) {
        consider(d / netplay_module_file_name(), "updated");
        if (found.size() > before) break;
    }
    if (found.empty()) {
        out.note = "no usable netplay module";
        for (const auto& s : skipped) out.note += "; " + s;
        return out;
    }
    const Cand* best = &found[0];
    for (const auto& c : found)
        if (version_cmp(c.info.version, best->info.version) > 0) best = &c;
    out.path = best->path;
    out.info = best->info;
    out.source = best->source;
    out.note = best->source + " netplay module " + best->info.version;
    for (const auto& s : skipped) out.note += "; skipped " + s;
    return out;
}

NetplayModuleUpdateResult update_netplay_module(const Paths& paths, const fs::path& exe_dir,
                                                bool check_only) {
    NetplayModuleUpdateResult r;
    auto fail = [&](const std::string& m) {
        r.ok = false;
        r.message = "Netplay module update: " + m;
        return r;
    };
    const ResolvedNetplayModule current = resolve_netplay_module(paths, exe_dir);
    r.current_version = current.path.empty() ? "none" : current.info.version;
    if (current.source == "override") {
        r.ok = true;
        r.message = "Netplay module update: skipped, RETRO_NETPLAY_MODULE is set";
        return r;
    }

    std::string err;
    const std::string url = update_detail::resolve_manifest_url("RETRO_NETPLAY_MODULE_MANIFEST_URL", kSlug,
                                                                kAsset, &err);
    if (url.empty()) return fail(err.empty() ? "no manifest found" : err);
    const bool local = is_file_url(url);
    json m;
    if (!update_detail::fetch_manifest(url, m, &err)) return fail(err);
    if (m.value("schema", 0) != kManifestSchema)
        return fail("manifest schema " + std::to_string(m.value("schema", 0)) +
                    ", this launcher reads " + std::to_string(kManifestSchema) + "; update Retro");
    r.latest_version = m.value("version", "");
    const std::string commit = m.value("commit", "");
    const std::uint32_t abi = m.contains("module_abi") ? m["module_abi"].value("version", 0u) : 0u;
    const std::uint32_t wire = m.value("wire_version", 0u);

    const std::string key = runtime_platform_key();
    if (!m.contains("platforms") || !m["platforms"].contains(key))
        return fail("the manifest has no entry for " + key);
    const json& e = m["platforms"][key];
    if (e.contains("unavailable")) {
        r.ok = true;
        r.message = "Netplay module update: " + key + " is not available (" +
                    e["unavailable"].get<std::string>() + ")";
        return r;
    }
    if (abi != kNetplayModuleAbi) {
        r.ok = true;
        r.message = "Netplay module update: module " + r.latest_version + " is ABI " +
                    std::to_string(abi) + "; the runners here need ABI " +
                    std::to_string(kNetplayModuleAbi) + ". Update Retro to use it.";
        return r;
    }
    if (const std::string why = unmet_requirement(e.value("requires", json::object())); !why.empty()) {
        r.ok = true;
        r.message = "Netplay module update: " + r.latest_version + " cannot run here: " + why;
        return r;
    }
    if (version_cmp(r.latest_version, r.current_version) <= 0 && !current.path.empty()) {
        r.ok = true;
        r.message = "Netplay module is up to date (" + r.current_version + ", " + current.source + ")";
        return r;
    }
    r.available = true;
    if (check_only) {
        r.ok = true;
        r.message = "Netplay module update available: " + r.current_version + " -> " + r.latest_version;
        return r;
    }

    const std::string archive_name = e.value("archive", "");
    const std::string archive_url = e.value("url", "");
    const std::string want_sha = e.value("sha256", "");
    const std::uint64_t want_size = e.value("size", std::uint64_t{0});
    const std::string lib_name = e.value("library", "");
    if (archive_name.empty() || archive_name.find_first_of("/\\") != std::string::npos ||
        archive_url.empty() || want_sha.size() != 64 || !want_size ||
        lib_name != netplay_module_file_name())
        return fail("the manifest entry for " + key + " is incomplete or names an unexpected file");
    if (!is_release_version(r.latest_version) ||
        r.latest_version.find_first_of("/\\") != std::string::npos)
        return fail("the manifest's version '" + r.latest_version + "' is not a release version");

    const fs::path root = netplay_module_dir(paths);
    const fs::path download = root / ".download" / archive_name;
    std::error_code ec;
    if (!update_detail::download_checked(archive_url, download, want_size, want_sha, local, &err))
        return fail(err);

    const fs::path staging = root / (".staging-" + r.latest_version);
    auto discard = [&](const std::string& msg) {
        fs::remove_all(staging, ec);
        fs::remove(download, ec);
        return fail(msg);
    };
    fs::remove_all(staging, ec);
    if (!extract_archive_to(download, staging, &err))
        return discard("cannot extract " + archive_name + ": " + err);

    // Believe only what the library itself reports.
    NetplayModuleInfo i;
    if (!probe_netplay_module(staging / lib_name, i, &err))
        return discard("the downloaded module does not open: " + err);
    if (i.version != r.latest_version || (!commit.empty() && i.build_id != commit) ||
        i.abi_version != abi || (wire && i.wire_version != wire))
        return discard("the downloaded module reports version " + i.version + " build " + i.build_id +
                       " ABI " + std::to_string(i.abi_version) + " wire " +
                       std::to_string(i.wire_version) + ", the manifest says " + r.latest_version +
                       " build " + commit + " ABI " + std::to_string(abi) + " wire " +
                       std::to_string(wire) + "; not installed");

    const fs::path dest = root / r.latest_version;
    fs::remove_all(dest, ec);
    retcomm::robust_rename(staging, dest, ec);
    if (ec) return discard("cannot move the new module into " + utf8(dest) + ": " + ec.message());
    fs::remove(download, ec);
    update_detail::prune_versions(root, 2);

    r.ok = true;
    r.updated = true;
    r.installed = dest / lib_name;
    r.message = "Netplay module updated: " + r.current_version + " -> " + r.latest_version +
                " (used from the next match)";
    return r;
}

} // namespace retcomm
