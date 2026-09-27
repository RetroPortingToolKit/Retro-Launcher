#include "hub/hub_title.hpp"
#include "retcomm/fs_util.hpp"

#include "retcomm/data_root.hpp"
#include "retcomm/hash.hpp"

#include "transport.hpp" // Retro-Runtime corelink: run_to_completion, path_utf8

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#else
#include <climits>
#include <unistd.h>
#endif

namespace retcomm::hub {

namespace corelink = ::retro::corelink;
using json = nlohmann::json;

namespace {

bool valid_id(const std::string& id) {
    if (id.empty()) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

// owner/repo, each part GitHub's own alphabet: it goes into URLs verbatim.
bool valid_github_slug(const std::string& s) {
    const auto slash = s.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == s.size() ||
        s.find('/', slash + 1) != std::string::npos)
        return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ||
               c == '/';
    });
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A descriptor path: relative to title.json's directory, UTF-8, and never
// allowed to climb out of it (the payload is one directory the kit copies
// whole; a path outside it would not be in the app).
bool payload_path(const fs::path& root, const std::string& rel, fs::path& out, std::string* err,
                  const char* key) {
    if (rel.empty()) {
        if (err) *err = std::string(key) + " is empty";
        return false;
    }
    const fs::path p = corelink::utf8_path(rel);
    if (p.is_absolute() || p.has_root_name()) {
        if (err) *err = std::string(key) + " must be relative to title.json: " + rel;
        return false;
    }
    for (const auto& part : p.lexically_normal()) {
        if (part == "..") {
            if (err) *err = std::string(key) + " leaves the title directory: " + rel;
            return false;
        }
    }
    out = (root / p).lexically_normal();
    return true;
}

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

} // namespace

fs::path title_json_path(const fs::path& p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return p / "title.json";
    return p;
}

bool load_title(const fs::path& path, TitleInfo& out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = corelink::path_utf8(out.json) + ": " + m;
        return false;
    };
    out = TitleInfo{};
    std::error_code ec;
    out.json = fs::absolute(title_json_path(path), ec).lexically_normal();
    out.root = out.json.parent_path();
    std::ifstream in(out.json, std::ios::binary);
    if (!in) return fail("cannot read (no title.json there)");
    json j;
    try {
        j = json::parse(in);
    } catch (const std::exception& e) {
        return fail(std::string("not JSON: ") + e.what());
    }
    if (!j.is_object()) return fail("not a JSON object");
    try {
        if (!j.contains("schema") || !j["schema"].is_number_integer())
            return fail("no integer \"schema\"");
        out.schema = j["schema"].get<int>();
        if (out.schema > kTitleSchema)
            return fail("schema " + std::to_string(out.schema) + "; this hub reads schema " +
                        std::to_string(kTitleSchema) + " (update Retro Launcher)");
        if (out.schema < 1) return fail("schema " + std::to_string(out.schema) + " is not valid");
        out.id = j.value("id", "");
        if (!valid_id(out.id)) return fail("\"id\" must match [a-z0-9_-]+ (got '" + out.id + "')");
        out.name = j.value("name", "");
        if (out.name.empty()) out.name = out.id;
        out.version = j.value("version", "");
        out.platform = j.value("platform", "");
        std::string err;
        if (!payload_path(out.root, j.value("core", ""), out.core, &err, "core")) return fail(err);
        if (j.contains("package") && !j["package"].is_null()) {
            if (!payload_path(out.root, j.value("package", ""), out.package, &err, "package"))
                return fail(err);
        }
        if (j.contains("title_dir") && !j["title_dir"].is_null()) {
            if (!payload_path(out.root, j.value("title_dir", ""), out.title_dir, &err, "title_dir"))
                return fail(err);
        } else {
            out.title_dir = (out.package.empty() ? out.core : out.package).parent_path();
        }
        if (j.contains("opts")) {
            const json& o = j["opts"];
            if (o.is_array()) {
                for (const auto& e : o) {
                    if (!e.is_string()) return fail("\"opts\" entries must be \"key=value\" strings");
                    const std::string kv = e.get<std::string>();
                    const auto eq = kv.find('=');
                    if (eq == std::string::npos || eq == 0)
                        return fail("\"opts\" entry is not key=value: " + kv);
                    out.opts.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
                }
            } else if (o.is_object()) {
                for (auto it = o.begin(); it != o.end(); ++it) {
                    out.opts.emplace_back(it.key(), it.value().is_string()
                                                        ? it.value().get<std::string>()
                                                        : it.value().dump());
                }
            } else if (!o.is_null()) {
                return fail("\"opts\" must be an array of \"key=value\" strings");
            }
        }
        if (j.contains("rom") && j["rom"].is_object()) {
            const json& r = j["rom"];
            out.has_rom = true;
            if (r.contains("file_names") && r["file_names"].is_array()) {
                for (const auto& f : r["file_names"]) {
                    if (!f.is_string()) continue;
                    const std::string name = f.get<std::string>();
                    // A bare file name: it is looked up in two directories.
                    if (name.empty() || name.find('/') != std::string::npos ||
                        name.find('\\') != std::string::npos || name == "." || name == "..")
                        return fail("rom.file_names entries must be bare file names: " + name);
                    out.rom.file_names.push_back(name);
                }
            }
            if (r.contains("size") && !r["size"].is_null()) {
                if (!r["size"].is_number_unsigned() && !r["size"].is_number_integer())
                    return fail("rom.size must be an integer");
                out.rom.has_size = true;
                out.rom.size = r["size"].get<std::uint64_t>();
            }
            out.rom.sha256 = lower(r.value("sha256", ""));
            if (!out.rom.sha256.empty() &&
                (out.rom.sha256.size() != 64 ||
                 out.rom.sha256.find_first_not_of("0123456789abcdef") != std::string::npos))
                return fail("rom.sha256 must be 64 hex digits");
            out.rom.label = r.value("label", "");
        }
        if (j.contains("boxart") && !j["boxart"].is_null()) {
            if (!payload_path(out.root, j.value("boxart", ""), out.boxart, &err, "boxart"))
                return fail(err);
        }
        if (j.contains("update") && !j["update"].is_null()) {
            const json& u = j["update"];
            if (!u.is_object()) return fail("\"update\" must be an object");
            out.update_github = u.value("github", "");
            if (!out.update_github.empty() && !valid_github_slug(out.update_github))
                return fail("update.github must be owner/repo (got '" + out.update_github + "')");
        }
    } catch (const std::exception& e) {
        return fail(std::string("unexpected value: ") + e.what());
    }
    return true;
}

fs::path find_bundled_title(const fs::path& exe_dir) {
    if (exe_dir.empty()) return {};
    std::error_code ec;
    std::vector<fs::path> cands = {exe_dir / "title" / "title.json"};
#if defined(__APPLE__)
    cands.push_back(exe_dir / ".." / "Resources" / "title" / "title.json");
#endif
    for (const fs::path& c : cands) {
        if (fs::is_regular_file(c, ec)) return fs::weakly_canonical(c, ec);
    }
    return {};
}

fs::path current_exe_path() {
    std::error_code ec;
#if defined(_WIN32)
    DWORD cap = MAX_PATH;
    std::wstring buf(cap, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), cap);
        if (n == 0) return {};
        if (n < cap) {
            buf.resize(n);
            return fs::path(buf);
        }
        if (cap >= 32768) return {};
        cap *= 2;
        buf.assign(cap, L'\0');
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    buf.resize(std::char_traits<char>::length(buf.c_str()));
    const fs::path c = fs::weakly_canonical(fs::path(buf), ec);
    return ec ? fs::path(buf) : c;
#else
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p;
#endif
}

AppAnchor locate_app(const fs::path& exe) {
    AppAnchor a;
    std::error_code ec;
    auto take = [&](const fs::path& app, const char* source) {
        a.app = fs::absolute(app, ec).lexically_normal();
        a.dir = a.app.parent_path();
        a.source = source;
    };
    // SHIPPING.md §5: inside an AppImage /proc/self/exe is in the read-only
    // mount; $APPIMAGE is the file the player has.
    for (const char* var : {"RETRO_HUB_APP", "APPIMAGE", "RETCOMM_PORTABLE_EXE"}) {
        const std::string v = env_or_empty(var);
        if (!v.empty() && fs::exists(corelink::utf8_path(v), ec)) {
            take(corelink::utf8_path(v), var);
            return a;
        }
    }
    // <X>.app/Contents/MacOS/retro-hub: the bundle is the app.
    const fs::path macos = exe.parent_path();
    const fs::path contents = macos.parent_path();
    const fs::path bundle = contents.parent_path();
    if (macos.filename() == "MacOS" && contents.filename() == "Contents" &&
        bundle.extension() == ".app") {
        take(bundle, "bundle");
        return a;
    }
    take(exe, "exe");
    return a;
}

TitleDataDir resolve_title_data_dir(const AppAnchor& anchor, const std::string& id,
                                    const fs::path& fallback_base) {
    TitleDataDir d;
    d.dir = anchor.dir / (id + "-data");
    std::string why;
    if (!anchor.dir.empty() && directory_is_writable(d.dir, &why)) return d;
    const fs::path base =
        fallback_base.empty() ? default_os_data_dir().parent_path() : fallback_base;
    d.note = (anchor.dir.empty() ? std::string("no app directory") : why) +
             "; using the user data directory";
    d.dir = base / id;
    d.beside_app = false;
    return d;
}

bool ensure_title_data_dir(TitleDataDir& d, const std::string& id, std::string* error,
                           const fs::path& fallback_base) {
    std::error_code ec;
    fs::create_directories(d.dir, ec);
    if (!ec && fs::is_directory(d.dir, ec)) return true;
    if (d.beside_app) {
        const fs::path base =
            fallback_base.empty() ? default_os_data_dir().parent_path() : fallback_base;
        d.note = "cannot create " + d.dir.string() + " (" + ec.message() +
                 "); using the user data directory";
        d.dir = base / id;
        d.beside_app = false;
        ec.clear();
        fs::create_directories(d.dir, ec);
        if (!ec) return true;
    }
    if (error) *error = "cannot create " + d.dir.string() + ": " + ec.message();
    return false;
}

Paths title_paths(const fs::path& data_dir) {
    Paths p = paths_for_root(data_dir, DataRootSource::Explicit);
    p.config_dir = data_dir;
    p.data_dir = data_dir;
    p.apps_dir = data_dir / "apps";
    p.toolchains_dir = data_dir / "toolchains";
    p.sdks_dir = data_dir / "sdks";
    p.engines_dir = data_dir / "engines";
    p.state_path = data_dir / "state.json";
    p.config_path = data_dir / "config.json";
    p.library_index_path = data_dir / "library-index.json";
    p.bios_index_path = data_dir / "bios-index.json";
    p.romm_rom_index_path = data_dir / "romm-rom-index.json";
    p.catalog_dir = data_dir / "catalog";
    return p;
}

RomCheck check_rom(const fs::path& rom, const TitleRom& want) {
    RomCheck c;
    std::error_code ec;
    if (!fs::is_regular_file(rom, ec)) {
        c.error = "not a file";
        return c;
    }
    c.size = fs::file_size(rom, ec);
    if (ec) {
        c.error = "cannot read its size";
        return c;
    }
    if (want.has_size && c.size != want.size) {
        c.error = "expected " + std::to_string(want.size) + " bytes" +
                  (want.sha256.empty() ? "" : ", sha256 " + want.sha256) + "; got " +
                  std::to_string(c.size) + " bytes";
        return c;
    }
    if (!want.sha256.empty()) {
        c.sha256 = file_sha256_hex(rom);
        if (c.sha256.empty()) {
            c.error = "cannot read it";
            return c;
        }
        if (c.sha256 != want.sha256) {
            c.error = "expected sha256 " + want.sha256 + "; got " + c.sha256 + " (" +
                      std::to_string(c.size) + " bytes)";
            return c;
        }
    }
    c.ok = true;
    return c;
}

RomChoice resolve_rom(const TitleInfo& t, const fs::path& cli_rom, const fs::path& app_dir,
                      const fs::path& data_dir) {
    RomChoice r;
    std::error_code ec;
    if (!cli_rom.empty()) {
        const RomCheck c = check_rom(cli_rom, t.rom);
        if (c.ok) {
            r.path = cli_rom;
            r.source = "--rom";
        } else {
            r.refused = true;
            r.error = corelink::path_utf8(cli_rom) + ": " + c.error;
        }
        return r;
    }
    auto consider = [&](const fs::path& p, const char* source) {
        if (!fs::is_regular_file(p, ec)) return false;
        const RomCheck c = check_rom(p, t.rom);
        if (!c.ok) {
            r.notes.push_back(std::string(source) + " " + corelink::path_utf8(p) + ": " + c.error);
            return false;
        }
        r.path = fs::absolute(p, ec).lexically_normal();
        r.source = source;
        return true;
    };
    if (!data_dir.empty()) {
        const fs::path remembered = remembered_rom(data_dir);
        if (!remembered.empty() && consider(remembered, "remembered")) return r;
    }
    for (const std::string& name : t.rom.file_names) {
        if (!app_dir.empty() && consider(app_dir / corelink::utf8_path(name), "beside the app"))
            return r;
    }
    for (const std::string& name : t.rom.file_names) {
        if (!data_dir.empty() &&
            consider(data_dir / "roms" / corelink::utf8_path(name), "data dir"))
            return r;
    }
    return r;
}

fs::path remembered_rom(const fs::path& data_dir) {
    std::ifstream in(data_dir / "rom.json", std::ios::binary);
    if (!in) return {};
    try {
        const json j = json::parse(in);
        const std::string p = j.value("path", "");
        if (!p.empty()) return corelink::utf8_path(p);
    } catch (const std::exception&) {
    }
    return {};
}

bool remember_rom(const fs::path& data_dir, const fs::path& rom, std::string* error) {
    std::error_code ec;
    fs::create_directories(data_dir, ec);
    json j;
    j["schema"] = 1;
    j["path"] = corelink::path_utf8(fs::absolute(rom, ec).lexically_normal());
    const fs::path tmp = data_dir / "rom.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out || !(out << j.dump(2) << "\n")) {
            if (error) *error = corelink::path_utf8(tmp) + ": cannot write";
            return false;
        }
    }
    retcomm::robust_rename(tmp, data_dir / "rom.json", ec);
    if (ec) {
        if (error) *error = corelink::path_utf8(data_dir / "rom.json") + ": " + ec.message();
        return false;
    }
    return true;
}

fs::path absolute_from(const fs::path& p, const fs::path& cwd) {
    if (p.empty() || p.is_absolute()) return p.lexically_normal();
    return (cwd / p).lexically_normal();
}

std::map<std::string, std::string> probe_version(const fs::path& program, std::string* error) {
    std::map<std::string, std::string> fields;
    std::error_code ec;
    std::mt19937_64 rng(static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path report = fs::temp_directory_path(ec) /
                            ("retro-hub-version-" + std::to_string(rng()) + ".txt");
    corelink::SpawnSpec spec;
    spec.args = {corelink::path_utf8(program), "--version"};
    spec.log = report;
    std::string err;
    const auto code = corelink::run_to_completion(spec, 5000, &err);
    {
        std::ifstream in(report);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto sp = line.find(' ');
            if (sp != std::string::npos) fields[line.substr(0, sp)] = line.substr(sp + 1);
        }
    }
    fs::remove(report, ec);
    if (!code || *code != 0) {
        if (error) *error = !code ? err : "--version exited " + std::to_string(*code);
        return {};
    }
    return fields;
}

} // namespace retcomm::hub
