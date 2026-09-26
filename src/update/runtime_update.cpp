#include "retcomm/runtime_update.hpp"

#include "retcomm/hash.hpp"
#include "retcomm/http.hpp"
#include "retcomm/install.hpp"
#include "retcomm/release_tags.hpp"

#include "link_protocol.hpp"
#include "runner_probe.hpp"
#include "transport.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(__linux__)
#  include <gnu/libc-version.h>
#elif defined(__APPLE__)
#  include <sys/sysctl.h>
#endif

namespace retcomm {
namespace {

using nlohmann::json;
namespace corelink = retro::corelink;

constexpr const char* kDefaultManifestUrl =
    "https://github.com/RetroPortingToolKit/Retro-Runtime/releases/latest/download/"
    "runtime-manifest.json";
constexpr int kManifestSchema = 1;

bool is_file_url(const std::string& url) { return url.rfind("file://", 0) == 0; }

fs::path file_url_path(const std::string& url) {
    std::string p = url.substr(7);
#if defined(_WIN32)
    if (p.size() > 2 && p[0] == '/' && p[2] == ':') p.erase(0, 1); // file:///C:/...
#endif
    return corelink::utf8_path(p);
}

// A release version orders by its numbers; anything else (a "dev" build, an
// empty report) is older than every release.
bool is_release_version(const std::string& v) {
    return !v.empty() && std::isdigit(static_cast<unsigned char>(v[0]));
}
int version_cmp(const std::string& a, const std::string& b) {
    const bool ra = is_release_version(a), rb = is_release_version(b);
    if (ra != rb) return ra ? 1 : -1;
    if (!ra) return 0;
    return release_tag_cmp(a, b);
}

// "2.35" >= "2.31", number by number.
bool version_at_least(const std::string& have, const std::string& need) {
    return release_tag_cmp(have, need) >= 0;
}

std::string os_version() {
#if defined(__linux__)
    return gnu_get_libc_version();
#elif defined(__APPLE__)
    char buf[64] = {};
    size_t len = sizeof buf;
    if (sysctlbyname("kern.osproductversion", buf, &len, nullptr, 0) == 0) return buf;
    return "";
#else
    return "";
#endif
}

// Why this machine cannot run the entry, or empty.
std::string unmet_requirement(const json& reqs) {
    if (!reqs.is_object()) return "";
#if defined(__linux__)
    if (reqs.contains("glibc")) {
        const std::string need = reqs["glibc"].get<std::string>();
        const std::string have = os_version();
        if (!version_at_least(have, need)) return "it needs glibc " + need + ", this system has " + have;
    }
#elif defined(__APPLE__)
    if (reqs.contains("macos")) {
        const std::string need = reqs["macos"].get<std::string>();
        const std::string have = os_version();
        if (!have.empty() && !version_at_least(have, need))
            return "it needs macOS " + need + ", this Mac has " + have;
    }
#endif
    return "";
}

struct Candidate {
    fs::path path;
    corelink::RunnerVersion info;
    std::string source;
};

} // namespace

std::string runtime_manifest_url() {
    const char* env = std::getenv("RETRO_RUNTIME_MANIFEST_URL");
    return env && *env ? env : kDefaultManifestUrl;
}

std::string runtime_platform_key() {
#if defined(_WIN32)
#  if defined(_M_ARM64) || defined(__aarch64__)
    return "windows-arm64";
#  else
    return "windows-x86_64";
#  endif
#elif defined(__APPLE__)
#  if defined(__aarch64__) || defined(__arm64__)
    return "macos-arm64";
#  else
    return "macos-x86_64";
#  endif
#else
#  if defined(__aarch64__)
    return "linux-arm64";
#  else
    return "linux-x86_64";
#  endif
#endif
}

fs::path runtime_dir(const Paths& paths) { return paths.data_dir / "runtime"; }

ResolvedRunner resolve_runner(const Paths& paths, const fs::path& exe_dir) {
    ResolvedRunner out;
    if (const char* env = std::getenv("RETRO_CORE_RUNNER"); env && *env) {
        out.path = corelink::utf8_path(env);
        out.source = "override";
        corelink::RunnerVersion v;
        std::string err;
        if (corelink::probe_runner(out.path, v, &err)) out.version = v.version;
        out.note = "RETRO_CORE_RUNNER names it" + (err.empty() ? "" : " (" + err + ")");
        return out;
    }

    std::vector<Candidate> found;
    std::vector<std::string> skipped;
    auto consider = [&](const fs::path& p, const std::string& source) {
        corelink::RunnerVersion v;
        std::string err;
        if (!corelink::probe_runner(p, v, &err)) {
            skipped.push_back(err);
            return;
        }
        if (!v.compatible()) {
            skipped.push_back(corelink::path_utf8(p) + ": link " + std::to_string(v.link_major) +
                              ", ABI " + std::to_string(v.abi_major) + "; this launcher speaks link " +
                              std::to_string(corelink::kProtocolMajor) + ", ABI " +
                              std::to_string(RCORE_ABI_MAJOR));
            return;
        }
        found.push_back({p, v, source});
    };

    const fs::path bundled = exe_dir / corelink::runner_file_name();
    std::error_code ec;
    if (fs::is_regular_file(bundled, ec)) consider(bundled, "bundled");
    // Updated runners, newest first; the first usable one is the only one needed.
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(runtime_dir(paths), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory() && is_release_version(name)) dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return release_tag_cmp(a.filename().string(), b.filename().string()) > 0;
    });
    const std::size_t before = found.size();
    for (const auto& d : dirs) {
        consider(d / corelink::runner_file_name(), "updated");
        if (found.size() > before) break;
    }

    if (found.empty()) {
        out.note = "no usable retro-core-runner";
        for (const auto& s : skipped) out.note += "; " + s;
        return out;
    }
    // Newest wins; on a tie the bundled one (found first) stays.
    const Candidate* best = &found[0];
    for (const auto& c : found)
        if (version_cmp(c.info.version, best->info.version) > 0) best = &c;
    out.path = best->path;
    out.version = best->info.version;
    out.source = best->source;
    out.note = best->source + " runner " + best->info.version;
    for (const auto& s : skipped) out.note += "; skipped " + s;
    return out;
}

RuntimeUpdateResult update_runtime(const Paths& paths, const fs::path& exe_dir, bool check_only) {
    RuntimeUpdateResult r;
    auto fail = [&](const std::string& m) {
        r.ok = false;
        r.message = "Runtime update: " + m;
        return r;
    };
    const ResolvedRunner current = resolve_runner(paths, exe_dir);
    r.current_version = current.version.empty() ? "none" : current.version;
    if (current.source == "override") {
        r.ok = true;
        r.message = "Runtime update: skipped, RETRO_CORE_RUNNER is set";
        return r;
    }

    // ---- 1. the manifest ------------------------------------------------------
    const std::string url = runtime_manifest_url();
    const bool local = is_file_url(url); // a test manifest; only then may entries be file://
    std::string body;
    if (local) {
        std::ifstream in(file_url_path(url), std::ios::binary);
        if (!in) return fail("cannot read " + url);
        std::ostringstream ss;
        ss << in.rdbuf();
        body = ss.str();
    } else {
        const HttpResponse resp = http_get(url, github_http_headers());
        if (!resp.ok()) {
            return fail("cannot fetch the manifest (" +
                        (resp.error.empty() ? "HTTP " + std::to_string(resp.status) : resp.error) + ")");
        }
        body = resp.body;
    }
    json m;
    try {
        m = json::parse(body);
    } catch (const std::exception& e) {
        return fail(std::string("the manifest is not JSON: ") + e.what());
    }
    if (m.value("schema", 0) != kManifestSchema) {
        return fail("manifest schema " + std::to_string(m.value("schema", 0)) +
                    ", this launcher reads " + std::to_string(kManifestSchema) + "; update Retro");
    }
    r.latest_version = m.value("version", "");
    const std::string commit = m.value("commit", "");

    // ---- 2. this platform -----------------------------------------------------
    const std::string key = runtime_platform_key();
    if (!m.contains("platforms") || !m["platforms"].contains(key)) {
        return fail("the manifest has no entry for " + key);
    }
    const json& e = m["platforms"][key];
    if (e.contains("unavailable")) {
        r.ok = true;
        r.message = "Runtime update: " + key + " is not available (" +
                    e["unavailable"].get<std::string>() + ")";
        return r;
    }

    // ---- 3. contracts, 4. requirements ----------------------------------------
    const std::uint32_t link_major = m["link_protocol"].value("major", 0u);
    const std::uint32_t abi_major = m["rcore_abi"].value("major", 0u);
    if (link_major != corelink::kProtocolMajor || abi_major != RCORE_ABI_MAJOR) {
        r.ok = true;
        r.message = "Runtime update: runner " + r.latest_version + " speaks link " +
                    std::to_string(link_major) + ", ABI " + std::to_string(abi_major) +
                    "; this launcher speaks link " + std::to_string(corelink::kProtocolMajor) +
                    ", ABI " + std::to_string(RCORE_ABI_MAJOR) + ". Update Retro to use it.";
        return r;
    }
    if (const std::string why = unmet_requirement(e.value("requires", json::object())); !why.empty()) {
        r.ok = true;
        r.message = "Runtime update: runner " + r.latest_version + " cannot run here: " + why;
        return r;
    }

    // ---- 5. newer? ------------------------------------------------------------
    if (version_cmp(r.latest_version, current.version) <= 0) {
        r.ok = true;
        r.message = "Runtime is up to date (" + r.current_version + ", " + current.source + ")";
        return r;
    }
    r.available = true;
    if (check_only) {
        r.ok = true;
        r.message = "Runtime update available: " + r.current_version + " -> " + r.latest_version;
        return r;
    }

    // ---- 6. download, then size and hash, before anything is opened ------------
    const std::string archive_name = e.value("archive", "");
    const std::string archive_url = e.value("url", "");
    const std::string want_sha = e.value("sha256", "");
    const std::uint64_t want_size = e.value("size", std::uint64_t{0});
    const std::string exe_name = e.value("executable", "");
    if (archive_name.empty() || archive_name.find_first_of("/\\") != std::string::npos ||
        archive_url.empty() || want_sha.size() != 64 || !want_size ||
        exe_name != corelink::runner_file_name()) {
        return fail("the manifest entry for " + key + " is incomplete or names an unexpected file");
    }
    if (!is_release_version(r.latest_version) ||
        r.latest_version.find_first_of("/\\") != std::string::npos) {
        return fail("the manifest's version '" + r.latest_version + "' is not a release version");
    }
    const fs::path root = runtime_dir(paths);
    const fs::path download = root / ".download" / archive_name;
    std::error_code ec;
    fs::create_directories(download.parent_path(), ec);
    std::string err;
    if (is_file_url(archive_url)) {
        if (!local) return fail("a published manifest may not name a file:// archive");
        fs::copy_file(file_url_path(archive_url), download, fs::copy_options::overwrite_existing, ec);
        if (ec) return fail("cannot copy " + archive_url + ": " + ec.message());
    } else if (!http_download(archive_url, download, &err, github_http_headers(), {}, want_size)) {
        return fail("download failed: " + err);
    }
    const std::uint64_t got_size = fs::file_size(download, ec);
    const std::string got_sha = to_lower_hex(file_sha256_hex(download));
    if (ec || got_size != want_size || got_sha != to_lower_hex(want_sha)) {
        fs::remove(download, ec);
        return fail(archive_name + " does not match the manifest (size " + std::to_string(got_size) +
                    " vs " + std::to_string(want_size) + ", sha256 " + got_sha + " vs " + want_sha +
                    "); deleted");
    }

    // ---- 7. extract, and believe only what the runner itself reports ------------
    // From here, any refusal removes both the staging copy and the download.
    const fs::path staging = root / (".staging-" + r.latest_version);
    auto discard = [&](const std::string& m) {
        fs::remove_all(staging, ec);
        fs::remove(download, ec);
        return fail(m);
    };
    fs::remove_all(staging, ec);
    if (!extract_archive_to(download, staging, &err)) {
        return discard("cannot extract " + archive_name + ": " + err);
    }
    const fs::path staged_runner = staging / exe_name;
#if !defined(_WIN32)
    fs::permissions(staged_runner, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
#endif
    corelink::RunnerVersion v;
    if (!corelink::probe_runner(staged_runner, v, &err)) {
        return discard("the downloaded runner does not run: " + err);
    }
    if (v.version != r.latest_version || (!commit.empty() && v.commit != commit) || !v.compatible()) {
        return discard("the downloaded runner reports version " + v.version + " commit " + v.commit +
                    ", the manifest says " + r.latest_version + " commit " + commit + "; not installed");
    }

    // ---- 8. into place, beside (never over) the runner in use --------------------
    const fs::path dest = root / r.latest_version;
    fs::remove_all(dest, ec); // a half-installed copy of this same version, if any
    fs::rename(staging, dest, ec);
    if (ec) {
        return discard("cannot move the new runner into " + corelink::path_utf8(dest) + ": " +
                       ec.message());
    }
    fs::remove(download, ec);

    // Keep the new runner and the one before it (for rollback); drop the rest.
    // A running session holds its runner open; on Windows that removal fails
    // quietly and is retried next time.
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory() && is_release_version(name)) dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return release_tag_cmp(a.filename().string(), b.filename().string()) > 0;
    });
    for (std::size_t i = 2; i < dirs.size(); ++i) fs::remove_all(dirs[i], ec);

    r.ok = true;
    r.updated = true;
    r.installed = dest / exe_name;
    r.message = "Runtime updated: " + r.current_version + " -> " + r.latest_version +
                " (used from the next launch)";
    return r;
}

} // namespace retcomm
