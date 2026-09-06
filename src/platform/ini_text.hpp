#pragma once

// Small text helpers shared by the per-platform settings modules (PSX
// settings.toml/config.ini, SNES config.ini/keybinds.ini). Line-oriented and
// comment-preserving: every writer here upserts one key inside one section and
// leaves the rest of the file exactly as the game shipped it.

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace retcomm {
namespace ini_text {

namespace fs = std::filesystem;

inline bool write_text_file(const fs::path& path, const std::string& body, std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    out << body;
    if (!body.empty() && body.back() != '\n') out << '\n';
    return static_cast<bool>(out);
}

inline std::string read_text_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline std::string trim_copy(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' ||
                          s.back() == '\t'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

inline bool ieq(const std::string& a, const char* b) {
    if (!b) return false;
    if (a.size() != std::strlen(b)) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

inline bool parse_bool(const std::string& v, bool* out) {
    if (ieq(v, "true") || ieq(v, "1") || ieq(v, "yes") || ieq(v, "on")) {
        *out = true;
        return true;
    }
    if (ieq(v, "false") || ieq(v, "0") || ieq(v, "no") || ieq(v, "off")) {
        *out = false;
        return true;
    }
    return false;
}

// Upsert key=value inside [section] (case-insensitive section/key match).
// Creates the section at EOF when missing; other lines pass through untouched.
inline void upsert_ini_key(std::string& body, const std::string& section, const std::string& key,
                           const std::string& value) {
    const std::string header = "[" + section + "]";
    std::istringstream in(body);
    std::ostringstream out;
    std::string line;
    bool in_sec = false;
    bool wrote = false;
    bool saw = false;

    while (std::getline(in, line)) {
        std::string trimmed = trim_copy(line);
        const bool is_sec = !trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']';

        if (in_sec && is_sec) {
            if (!wrote) {
                out << key << "=" << value << "\n";
                wrote = true;
            }
            in_sec = false;
        }
        if (!in_sec && ieq(trimmed, header.c_str())) {
            in_sec = true;
            saw = true;
            out << line << "\n";
            continue;
        }
        if (in_sec) {
            const auto eq = trimmed.find('=');
            if (eq != std::string::npos) {
                std::string k = trim_copy(trimmed.substr(0, eq));
                if (ieq(k, key.c_str())) {
                    out << key << "=" << value << "\n";
                    wrote = true;
                    continue;
                }
            }
        }
        out << line << "\n";
    }
    if (in_sec && !wrote) {
        out << key << "=" << value << "\n";
        wrote = true;
    }
    if (!saw) {
        std::string b = out.str();
        while (!b.empty() && (b.back() == '\n' || b.back() == '\r')) b.pop_back();
        if (!b.empty()) b.push_back('\n');
        b += "\n" + header + "\n" + key + "=" + value + "\n";
        body = std::move(b);
        return;
    }
    body = out.str();
}

// Walk every key=value pair of an INI body. `fn(section, key, value)`; section
// is the bare name without brackets. Comment lines (# ;) are skipped.
template <typename Fn>
inline void for_each_ini_pair(const std::string& body, Fn&& fn) {
    std::istringstream in(body);
    std::string line;
    std::string section;
    while (std::getline(in, line)) {
        std::string t = trim_copy(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[') {
            const auto close = t.find(']');
            section = t.substr(1, close == std::string::npos ? std::string::npos : close - 1);
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        fn(section, trim_copy(t.substr(0, eq)), trim_copy(t.substr(eq + 1)));
    }
}

} // namespace ini_text
} // namespace retcomm
