#include "core_manifest.hpp"

#include <cstring>
#include <fstream>
#include <sstream>

namespace retcomm::runner {

namespace {

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

// A quoted string starting at v[i] == '"'; advances i past the closing quote.
bool parse_string(const std::string& v, size_t& i, std::string& out) {
    if (i >= v.size() || v[i] != '"') return false;
    out.clear();
    for (++i; i < v.size(); ++i) {
        const char c = v[i];
        if (c == '"') {
            ++i;
            return true;
        }
        if (c == '\\' && i + 1 < v.size()) {
            const char e = v[++i];
            out += (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
        } else {
            out += c;
        }
    }
    return false;
}

bool only_trailing_comment(const std::string& v, size_t i) {
    const std::string rest = trim(v.substr(i));
    return rest.empty() || rest[0] == '#';
}

bool parse_array(const std::string& v, std::vector<std::string>& out) {
    out.clear();
    size_t i = 1; // past '['
    for (;;) {
        while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
        if (i < v.size() && v[i] == ']') return only_trailing_comment(v, i + 1);
        std::string s;
        if (!parse_string(v, i, s)) return false;
        out.push_back(s);
        while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
        if (i < v.size() && v[i] == ',') {
            ++i;
            continue;
        }
        if (i < v.size() && v[i] == ']') return only_trailing_comment(v, i + 1);
        return false;
    }
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) {
        if (!out.empty()) out += ',';
        out += s;
    }
    return out;
}

} // namespace

fs::path manifest_path_for(const fs::path& library) {
    return library.parent_path() / (library.stem().string() + ".rcore.toml");
}

bool read_manifest(const fs::path& path, CoreManifest& out, std::string* error) {
    out = CoreManifest{};
    out.path = path;
    std::ifstream in(path);
    if (!in) {
        if (error) *error = path.string() + ": missing (the core's build generates it)";
        return false;
    }
    std::string line, section;
    int n = 0;
    auto bad = [&](const std::string& why) {
        if (error) *error = path.string() + ":" + std::to_string(n) + ": " + why;
        return false;
    };
    while (std::getline(in, line)) {
        ++n;
        const std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        if (s[0] == '[') {
            const auto close = s.find(']');
            if (close == std::string::npos || (s.size() > 1 && s[1] == '[')) {
                return bad("expected a [section] header");
            }
            section = trim(s.substr(1, close - 1));
            if (section != "core" && section != "title" && section != "build") {
                return bad("unknown section [" + section + "]");
            }
            if (section == "title") out.has_title = true;
            continue;
        }
        const auto eq = s.find('=');
        if (eq == std::string::npos) return bad("expected key = value");
        const std::string key = trim(s.substr(0, eq));
        const std::string val = trim(s.substr(eq + 1));
        if (section.empty()) return bad("key '" + key + "' outside any section");

        std::string str;
        std::vector<std::string> arr;
        size_t i = 0;
        const bool is_str = !val.empty() && val[0] == '"' && parse_string(val, i, str) &&
                            only_trailing_comment(val, i);
        const bool is_arr = !val.empty() && val[0] == '[' && parse_array(val, arr);
        const bool is_bool = val == "true" || val == "false";
        char* end = nullptr;
        const long num = std::strtol(val.c_str(), &end, 10);
        const bool is_int = !val.empty() && end && only_trailing_comment(val, end - val.c_str());

        auto want_str = [&](std::string& dst) {
            if (!is_str) return bad("'" + key + "' must be a quoted string");
            dst = str;
            return true;
        };
        auto want_arr = [&](std::vector<std::string>& dst) {
            if (!is_arr) return bad("'" + key + "' must be an array of strings");
            dst = arr;
            return true;
        };
        auto want_int = [&](long& dst) {
            if (!is_int) return bad("'" + key + "' must be an integer");
            dst = num;
            return true;
        };
        auto want_bool = [&](bool& dst) {
            if (!is_bool) return bad("'" + key + "' must be true or false");
            dst = val == "true";
            return true;
        };

        bool ok = true;
        if (section == "core") {
            if (key == "abi_major") ok = want_int(out.abi_major);
            else if (key == "draft_revision") ok = want_int(out.draft_revision);
            else if (key == "id") ok = want_str(out.id);
            else if (key == "version") ok = want_str(out.version);
            else if (key == "library") ok = want_str(out.library);
            else if (key == "platforms") ok = want_arr(out.platforms);
            else if (key == "capabilities") ok = want_arr(out.capabilities);
            else if (key == "state_compat_id") {
                ok = want_str(out.state_compat_id);
                out.has_state_compat_id = true;
            } else return bad("unknown [core] key '" + key + "'");
        } else if (section == "title") {
            if (key == "id") ok = want_str(out.title_id);
            else if (key == "content_sha256") ok = want_arr(out.content_sha256);
            else if (key == "dir") ok = want_str(out.title_dir);
            else return bad("unknown [title] key '" + key + "'");
        } else {
            if (key == "engine_commit") ok = want_str(out.engine_commit);
            else if (key == "engine_dirty") ok = want_bool(out.engine_dirty);
            else if (key == "toolchain") ok = want_str(out.toolchain);
            else if (key == "generated_utc") ok = want_str(out.generated_utc);
            else return bad("unknown [build] key '" + key + "'");
        }
        if (!ok) return false;
    }
    if (out.abi_major < 0 || out.id.empty() || out.library.empty()) {
        if (error) *error = path.string() + ": [core] abi_major, id and library are required";
        return false;
    }
    return true;
}

std::vector<std::string> verify_manifest(const CoreManifest& m, const LoadedCore& core) {
    std::vector<std::string> diffs;
    const rcore_core_info& info = *core.info;
    auto differ = [&](const char* field, const std::string& manifest, const std::string& lib) {
        if (manifest != lib) {
            diffs.push_back(std::string(field) + ": manifest '" + manifest + "', library '" +
                            lib + "'");
        }
    };
    auto cstr = [](const char* p) { return p ? std::string(p) : std::string(); };

    differ("abi_major", std::to_string(m.abi_major), std::to_string(info.abi_major));
    differ("id", m.id, cstr(info.core_id));
    differ("version", m.version, cstr(info.core_version));
    differ("library", m.library, core.path.filename().string());

    // platforms: the info string is comma-separated with optional spaces.
    std::vector<std::string> plats;
    {
        std::stringstream ss(cstr(info.platforms));
        std::string p;
        while (std::getline(ss, p, ',')) plats.push_back(trim(p));
    }
    differ("platforms", join(m.platforms), join(plats));

    std::uint64_t unnamed = 0;
    const std::string caps = capability_names(info.capabilities, &unnamed);
    if (unnamed) {
        diffs.push_back("capabilities: the library declares bits 0x" +
                        [&] {
                            std::ostringstream o;
                            o << std::hex << unnamed;
                            return o.str();
                        }() +
                        " this runner cannot name");
    }
    differ("capabilities", join(m.capabilities), caps);

    const bool lib_has_compat = info.state_compat_id != nullptr;
    if (m.has_state_compat_id != lib_has_compat) {
        diffs.push_back(std::string("state_compat_id: manifest ") +
                        (m.has_state_compat_id ? "'" + m.state_compat_id + "'" : "absent") +
                        ", library " + (lib_has_compat ? "'" + cstr(info.state_compat_id) + "'"
                                                       : "NULL"));
    } else if (lib_has_compat) {
        differ("state_compat_id", m.state_compat_id, cstr(info.state_compat_id));
    }
    return diffs;
}

} // namespace retcomm::runner
