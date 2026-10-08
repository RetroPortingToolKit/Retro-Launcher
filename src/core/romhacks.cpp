#include "retcomm/romhacks.hpp"

#include <algorithm>
#include <fstream>

namespace retcomm {
namespace {

std::string trim(std::string s) {
    const auto ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

std::string unquote(std::string v) {
    v = trim(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
}

bool valid_id(const std::string& id) {
    if (id.empty() || id[0] == '.') return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '-' || c == '_';
    });
}

// base.toml is the core's subset of TOML (n64lle romhack_base.rs): sections and
// `key = "value"` lines. Only what the picker shows is read here.
InstalledRomhack read_base(const fs::path& dir) {
    InstalledRomhack r;
    r.dir = dir;
    const std::string dname = dir.filename().string();
    std::ifstream in(dir / "base.toml");
    if (!in) {
        r.id = dname;
        r.title = dname;
        r.error = "base.toml is unreadable";
        return r;
    }
    std::string section, delta;
    for (std::string line; std::getline(in, line);) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') {
            section = trim(t.substr(1, t.size() - 2));
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(t.substr(0, eq));
        const std::string val = unquote(t.substr(eq + 1));
        if (section == "base") {
            if (key == "id") r.id = val;
            else if (key == "version") r.version = val;
            else if (key == "title") r.title = val;
            else if (key == "saves") r.saves_shared = val == "shared-with-stock";
        } else if (section == "delta" && key == "file") {
            delta = val;
        }
    }
    if (r.title.empty()) r.title = r.id.empty() ? dname : r.id;
    if (r.id.empty()) {
        r.id = dname;
        r.error = "base.toml names no [base] id";
    } else if (r.id != dname) {
        r.error = "base.toml says it is '" + r.id + "', but its directory is '" + dname + "'";
        r.id = dname;
    } else if (!valid_id(r.id)) {
        r.error = "'" + r.id + "' is not a package id";
    } else if (delta.empty() || !fs::exists(dir / delta)) {
        r.error = "its delta package is missing (" + (delta.empty() ? std::string("none named")
                                                                      : delta) + ")";
    }
    return r;
}

}  // namespace

std::vector<InstalledRomhack> scan_romhacks(const fs::path& title_dir) {
    std::vector<InstalledRomhack> out;
    std::error_code ec;
    const fs::path root = title_dir / "romhacks";
    if (!fs::is_directory(root, ec)) return out;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) continue;
        if (!fs::exists(e.path() / "base.toml", ec)) continue;
        out.push_back(read_base(e.path()));
    }
    std::sort(out.begin(), out.end(), [](const InstalledRomhack& a, const InstalledRomhack& b) {
        return a.title != b.title ? a.title < b.title : a.id < b.id;
    });
    return out;
}

const InstalledRomhack* find_romhack(const std::vector<InstalledRomhack>& list,
                                     const std::string& id) {
    for (const InstalledRomhack& r : list)
        if (r.id == id) return &r;
    return nullptr;
}

fs::path base_save_dir(const fs::path& data_dir, const std::string& title_key,
                       const std::string& base_id, bool saves_shared) {
    if (base_id.empty() || saves_shared) return data_dir / "saves" / title_key;
    return data_dir / "saves" / (title_key + "@" + base_id);
}

ModBaseStanding mod_base_standing(const ModPackageInfo& pkg, const InstalledRomhack* base) {
    if (!base) return ModBaseStanding::Stock;
    for (const std::string& b : pkg.bases) {
        const auto at = b.find('@');
        if (at == std::string::npos ? b == base->id
                                    : b.substr(0, at) == base->id && b.substr(at + 1) == base->version)
            return ModBaseStanding::Declared;
    }
    return ModBaseStanding::Unverified;
}

}  // namespace retcomm
