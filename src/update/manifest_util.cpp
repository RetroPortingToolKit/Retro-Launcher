#include "manifest_util.hpp"

#include "retcomm/hash.hpp"
#include "retcomm/http.hpp"
#include "retcomm/install.hpp"
#include "retcomm/release_tags.hpp"

#include "transport.hpp" // Retro-Runtime corelink: utf8_path

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

namespace retcomm::update_detail {

using nlohmann::json;

namespace {

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

// v1.2.3 or 1.2.3: only a release tag, never a pre-release suffix.
bool is_plain_release_tag(const std::string& t) {
    const size_t start = !t.empty() && (t[0] == 'v' || t[0] == 'V') ? 1 : 0;
    if (start >= t.size() || !std::isdigit(static_cast<unsigned char>(t[start]))) return false;
    for (size_t i = start; i < t.size(); ++i) {
        const char c = t[i];
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return false;
    }
    return t.back() != '.';
}

} // namespace

bool is_file_url(const std::string& url) { return url.rfind("file://", 0) == 0; }

fs::path file_url_path(const std::string& url) {
    std::string p = url.substr(7);
#if defined(_WIN32)
    if (p.size() > 2 && p[0] == '/' && p[2] == ':') p.erase(0, 1); // file:///C:/...
#endif
    return retro::corelink::utf8_path(p);
}

bool is_release_version(const std::string& v) {
    return !v.empty() && std::isdigit(static_cast<unsigned char>(v[0]));
}

int version_cmp(const std::string& a, const std::string& b) {
    const bool ra = is_release_version(a), rb = is_release_version(b);
    if (ra != rb) return ra ? 1 : -1;
    if (!ra) return 0;
    return release_tag_cmp(a, b);
}

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

std::string newest_release_tag(const std::string& slug, std::string* error) {
    const HttpResponse feed = http_get("https://github.com/" + slug + "/releases.atom");
    std::string best;
    if (feed.ok()) {
        const std::string needle = "/releases/tag/";
        for (size_t at = feed.body.find(needle); at != std::string::npos;
             at = feed.body.find(needle, at + needle.size())) {
            const size_t b = at + needle.size();
            const size_t e = feed.body.find_first_of("\"<>& \t\r\n", b);
            if (e == std::string::npos) break;
            const std::string tag = feed.body.substr(b, e - b);
            if (is_plain_release_tag(tag) && (best.empty() || release_tag_cmp(tag, best) > 0))
                best = tag;
        }
        if (!best.empty()) return best;
    }
    // A private repository serves no feed anonymously; the API does, to a token.
    std::string api_err;
    const std::string tag = fetch_latest_release_tag(slug, &api_err, /*allow_prerelease=*/true);
    if (!tag.empty()) return tag;
    if (error) {
        *error = "cannot read " + slug + "'s releases (feed: " +
                 (feed.ok() ? std::string("no release tags") : feed.error) +
                 (api_err.empty() ? std::string() : "; API: " + api_err) + ")";
    }
    return {};
}

std::string resolve_manifest_url(const char* env_var, const std::string& slug,
                                 const std::string& asset, std::string* error) {
    if (const char* env = std::getenv(env_var); env && *env) return env;
    const std::string tag = newest_release_tag(slug, error);
    if (tag.empty()) return {};
    return github_release_asset_url(slug, tag, asset);
}

bool fetch_manifest(const std::string& url, json& out, std::string* error) {
    std::string body;
    if (is_file_url(url)) {
        std::ifstream in(file_url_path(url), std::ios::binary);
        if (!in) {
            if (error) *error = "cannot read " + url;
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        body = ss.str();
    } else {
        const HttpResponse resp = http_get(url, github_http_headers());
        if (!resp.ok()) {
            if (error)
                *error = "cannot fetch " + url + " (" +
                         (resp.error.empty() ? "HTTP " + std::to_string(resp.status) : resp.error) +
                         ")";
            return false;
        }
        body = resp.body;
    }
    try {
        out = json::parse(body);
    } catch (const std::exception& e) {
        if (error) *error = std::string("the manifest is not JSON: ") + e.what();
        return false;
    }
    return true;
}

bool download_checked(const std::string& url, const fs::path& dest, std::uint64_t want_size,
                      const std::string& want_sha256, bool local_manifest, std::string* error) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::string err;
    if (is_file_url(url)) {
        if (!local_manifest) {
            if (error) *error = "a published manifest may not name a file:// archive";
            return false;
        }
        fs::copy_file(file_url_path(url), dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            if (error) *error = "cannot copy " + url + ": " + ec.message();
            return false;
        }
    } else if (!http_download(url, dest, &err, github_http_headers(), {}, want_size)) {
        if (error) *error = "download failed: " + err;
        return false;
    }
    const std::uint64_t got_size = fs::file_size(dest, ec);
    const std::string got_sha = to_lower_hex(file_sha256_hex(dest));
    if (ec || got_size != want_size || got_sha != to_lower_hex(want_sha256)) {
        fs::remove(dest, ec);
        if (error)
            *error = dest.filename().string() + " does not match the manifest (size " +
                     std::to_string(got_size) + " vs " + std::to_string(want_size) + ", sha256 " +
                     got_sha + " vs " + want_sha256 + "); deleted";
        return false;
    }
    return true;
}

void prune_versions(const fs::path& root, std::size_t keep) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory() && is_release_version(name)) dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return release_tag_cmp(a.filename().string(), b.filename().string()) > 0;
    });
    for (std::size_t i = keep; i < dirs.size(); ++i) fs::remove_all(dirs[i], ec);
}

} // namespace retcomm::update_detail
