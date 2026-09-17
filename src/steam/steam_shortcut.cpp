#include "retcomm/steam_shortcut.hpp"

#include "retcomm/self_update.hpp"

#include "miniz/miniz.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace retcomm {
namespace {

// ---------------------------------------------------------------------------
// Binary VDF. shortcuts.vdf is a tree of typed, NUL-key-terminated nodes:
//   0x00 <key> NUL <children…> 0x08   nested map
//   0x01 <key> NUL <value> NUL        string
//   0x02 <key> NUL <int32 little end> int
//   0x08                              end of map
// The file is parsed whole and written back whole, so fields this build does
// not know about — ones Steam or another shortcut tool wrote — survive a
// round trip instead of being dropped.
// ---------------------------------------------------------------------------
struct VdfNode {
    enum class Type { Map, Str, Int } type = Type::Map;
    std::string key;
    std::string str;
    int32_t num = 0;
    std::vector<VdfNode> children;

    VdfNode* find(const char* k) {
        for (VdfNode& c : children)
            if (c.key.size() == std::strlen(k) &&
                std::equal(c.key.begin(), c.key.end(), k, [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                }))
                return &c;
        return nullptr;
    }
};

bool read_cstr(const std::string& buf, size_t& i, std::string* out) {
    const size_t end = buf.find('\0', i);
    if (end == std::string::npos) return false;
    out->assign(buf, i, end - i);
    i = end + 1;
    return true;
}

bool parse_children(const std::string& buf, size_t& i, std::vector<VdfNode>* out, int depth) {
    if (depth > 32) return false;
    for (;;) {
        if (i >= buf.size()) return false;
        const unsigned char tag = static_cast<unsigned char>(buf[i++]);
        if (tag == 0x08) return true;
        VdfNode node;
        if (!read_cstr(buf, i, &node.key)) return false;
        if (tag == 0x00) {
            node.type = VdfNode::Type::Map;
            if (!parse_children(buf, i, &node.children, depth + 1)) return false;
        } else if (tag == 0x01) {
            node.type = VdfNode::Type::Str;
            if (!read_cstr(buf, i, &node.str)) return false;
        } else if (tag == 0x02) {
            node.type = VdfNode::Type::Int;
            if (i + 4 > buf.size()) return false;
            uint32_t v = 0;
            for (int b = 0; b < 4; ++b)
                v |= static_cast<uint32_t>(static_cast<unsigned char>(buf[i + b])) << (8 * b);
            i += 4;
            node.num = static_cast<int32_t>(v);
        } else {
            return false;  // 0x03..0x07 do not occur in shortcuts.vdf
        }
        out->push_back(std::move(node));
    }
}

void write_children(const std::vector<VdfNode>& nodes, std::string* out) {
    for (const VdfNode& n : nodes) {
        switch (n.type) {
            case VdfNode::Type::Map:
                out->push_back('\x00');
                out->append(n.key);
                out->push_back('\0');
                write_children(n.children, out);
                out->push_back('\x08');
                break;
            case VdfNode::Type::Str:
                out->push_back('\x01');
                out->append(n.key);
                out->push_back('\0');
                out->append(n.str);
                out->push_back('\0');
                break;
            case VdfNode::Type::Int: {
                out->push_back('\x02');
                out->append(n.key);
                out->push_back('\0');
                const uint32_t v = static_cast<uint32_t>(n.num);
                for (int b = 0; b < 4; ++b)
                    out->push_back(static_cast<char>((v >> (8 * b)) & 0xFF));
                break;
            }
        }
    }
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_file_atomic(const fs::path& p, const std::string& data, std::string* error) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".retcomm-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + tmp.string();
            return false;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            if (error) *error = "short write to " + tmp.string();
            return false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        std::error_code rec;
        fs::remove(tmp, rec);
        if (error) *error = "cannot replace " + p.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// Keep the state Retro found, once. A second write must not overwrite the only
// copy of what Steam had before Retro ever touched it.
void back_up_once(const fs::path& vdf) {
    std::error_code ec;
    if (!fs::exists(vdf, ec)) return;
    const fs::path bak = vdf.string() + ".retcomm-bak";
    if (fs::exists(bak, ec)) return;
    fs::copy_file(vdf, bak, ec);
}

// --- text VDF (loginusers.vdf) ---------------------------------------------
// Only enough of it to read persona names and MostRecent out of a file Steam
// writes as quoted key/value pairs.
void parse_login_users(const std::string& text,
                       std::vector<std::pair<std::string, SteamUser>>* out) {
    std::istringstream in(text);
    std::string line;
    std::string current;
    for (; std::getline(in, line);) {
        std::vector<std::string> quoted;
        for (size_t i = 0; i < line.size();) {
            if (line[i] != '"') {
                ++i;
                continue;
            }
            const size_t end = line.find('"', i + 1);
            if (end == std::string::npos) break;
            quoted.push_back(line.substr(i + 1, end - i - 1));
            i = end + 1;
        }
        if (quoted.size() == 1) {
            current = quoted[0];
            continue;
        }
        if (quoted.size() < 2 || current.empty()) continue;
        for (auto& [id, user] : *out) {
            if (id != current) continue;
            if (quoted[0] == "PersonaName" || quoted[0] == "AccountName") {
                if (user.persona.empty() || quoted[0] == "PersonaName") user.persona = quoted[1];
            } else if (quoted[0] == "MostRecent" && quoted[1] == "1") {
                user.most_recent = true;
            }
        }
    }
}

// Steam's 64-bit account id in loginusers.vdf vs. the 32-bit directory name
// under userdata: the low 32 bits are the same number.
std::string account_id_from_steamid64(const std::string& id64) {
    unsigned long long v = 0;
    for (char c : id64) {
        if (c < '0' || c > '9') return {};
        v = v * 10 + static_cast<unsigned long long>(c - '0');
    }
    return std::to_string(static_cast<uint32_t>(v & 0xFFFFFFFFull));
}

std::vector<fs::path> steam_root_candidates() {
    std::vector<fs::path> out;
    if (const char* env = std::getenv("RETCOMM_STEAM_ROOT"); env && *env) out.emplace_back(env);
#if defined(_WIN32)
    for (const wchar_t* key : {L"Software\\Valve\\Steam"}) {
        for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            HKEY h = nullptr;
            if (RegOpenKeyExW(hive, key, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
            wchar_t buf[MAX_PATH]{};
            DWORD n = sizeof(buf);
            DWORD type = 0;
            for (const wchar_t* value : {L"SteamPath", L"InstallPath"}) {
                n = sizeof(buf);
                if (RegQueryValueExW(h, value, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buf), &n) == ERROR_SUCCESS &&
                    type == REG_SZ)
                    out.emplace_back(buf);
            }
            RegCloseKey(h);
        }
    }
    if (const char* pf = std::getenv("ProgramFiles(x86)"); pf && *pf)
        out.emplace_back(fs::path(pf) / "Steam");
#else
    const char* home = std::getenv("HOME");
    if (home && *home) {
        const fs::path h(home);
#if defined(__APPLE__)
        out.push_back(h / "Library" / "Application Support" / "Steam");
#else
        out.push_back(h / ".steam" / "steam");
        out.push_back(h / ".steam" / "root");
        out.push_back(h / ".local" / "share" / "Steam");
        // Flatpak Steam keeps its own home; SteamOS game mode does not use it,
        // but a desktop-mode install might be the only one present.
        out.push_back(h / ".var" / "app" / "com.valvesoftware.Steam" / "data" / "Steam");
#endif
    }
#endif
    return out;
}

const char* kRetcommTag = "Retro Launcher";

std::string quote_exe(const fs::path& p) { return "\"" + p.string() + "\""; }

fs::path grid_dir(const SteamUser& user) { return user.dir / "config" / "grid"; }

fs::path shortcuts_vdf(const SteamUser& user) { return user.dir / "config" / "shortcuts.vdf"; }

bool copy_art(const fs::path& src, const fs::path& dest, std::vector<fs::path>* files) {
    if (src.empty()) return true;
    std::error_code ec;
    if (!fs::exists(src, ec)) return true;
    fs::create_directories(dest.parent_path(), ec);
    fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    files->push_back(dest);
    return true;
}

// Every name Steam might have read art under, for a given id.
void remove_art_for(const SteamUser& user, uint32_t id) {
    const fs::path grid = grid_dir(user);
    std::error_code ec;
    for (const char* suffix : {"p", "", "_hero", "_logo", "_icon"})
        for (const char* ext : {".png", ".jpg", ".jpeg"})
            fs::remove(grid / (std::to_string(id) + suffix + ext), ec);
}

VdfNode make_str(const char* k, std::string v) {
    VdfNode n;
    n.type = VdfNode::Type::Str;
    n.key = k;
    n.str = std::move(v);
    return n;
}

VdfNode make_int(const char* k, int32_t v) {
    VdfNode n;
    n.type = VdfNode::Type::Int;
    n.key = k;
    n.num = v;
    return n;
}

void set_str(VdfNode* entry, const char* k, const std::string& v) {
    if (VdfNode* e = entry->find(k)) {
        e->type = VdfNode::Type::Str;
        e->str = v;
        return;
    }
    entry->children.push_back(make_str(k, v));
}

void set_int(VdfNode* entry, const char* k, int32_t v) {
    if (VdfNode* e = entry->find(k)) {
        e->type = VdfNode::Type::Int;
        e->num = v;
        return;
    }
    entry->children.push_back(make_int(k, v));
}

// The "shortcuts" map, created if the file was missing or unreadable.
struct ShortcutsFile {
    VdfNode root;       // the whole document
    VdfNode* list = nullptr;
    bool parsed = false;
};

ShortcutsFile load_shortcuts(const fs::path& vdf) {
    ShortcutsFile out;
    out.root.type = VdfNode::Type::Map;
    const std::string raw = read_file(vdf);
    if (!raw.empty()) {
        size_t i = 0;
        std::vector<VdfNode> top;
        // The document is a single unnamed map; parse_children reads it as a
        // sequence, so the outer "shortcuts" node comes back as top[0].
        if (parse_children(raw, i, &top, 0) && !top.empty()) {
            out.root.children = std::move(top);
            out.parsed = true;
        }
    }
    out.list = out.root.find("shortcuts");
    if (!out.list) {
        VdfNode n;
        n.type = VdfNode::Type::Map;
        n.key = "shortcuts";
        out.root.children.push_back(std::move(n));
        out.list = out.root.find("shortcuts");
    }
    return out;
}

bool save_shortcuts(const fs::path& vdf, const ShortcutsFile& file, std::string* error) {
    std::string out;
    write_children(file.root.children, &out);
    out.push_back('\x08');  // close the document's implicit outer map
    back_up_once(vdf);
    return write_file_atomic(vdf, out, error);
}

// Entries are keyed by their index as a decimal string; Steam tolerates gaps
// but its own writer renumbers, so do the same.
void renumber(VdfNode* list) {
    for (size_t i = 0; i < list->children.size(); ++i)
        list->children[i].key = std::to_string(i);
}

const VdfNode* str_field(const VdfNode& e, const char* key) {
    const VdfNode* n = const_cast<VdfNode&>(e).find(key);
    return n && n->type == VdfNode::Type::Str ? n : nullptr;
}

// Our own stamp wins. Failing that, adopt an entry with the same display name
// that nothing else claims — someone who added the game by hand first should
// get it updated in place rather than duplicated.
VdfNode* find_entry(VdfNode* list, const std::string& app_name, const std::string& owner_id) {
    if (!owner_id.empty())
        for (VdfNode& e : list->children) {
            if (e.type != VdfNode::Type::Map) continue;
            const VdfNode* own = str_field(e, "DevkitGameID");
            if (own && own->str == owner_id) return &e;
        }
    for (VdfNode& e : list->children) {
        if (e.type != VdfNode::Type::Map) continue;
        const VdfNode* own = str_field(e, "DevkitGameID");
        if (own && !own->str.empty()) continue;
        const VdfNode* name = str_field(e, "appname");
        if (name && name->str == app_name) return &e;
    }
    return nullptr;
}

}  // namespace

bool steam_launcher_command(fs::path* exe, std::string* args_prefix, std::string* why) {
    const RetcommInstallInfo info = retcomm_install_info();
    if (info.channel == RetcommInstallChannel::LinuxAppImage && !info.path.empty()) {
        // AppRun forwards a leading "cli" to the bundled retcomm binary, and the
        // AppImage path survives a self-update, which replaces it in place.
        *exe = info.path;
        *args_prefix = "cli launch ";
        return true;
    }
    fs::path self;
#if defined(_WIN32)
    wchar_t mod[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, mod, MAX_PATH) > 0) self = mod;
#else
    std::error_code ec;
    self = fs::read_symlink("/proc/self/exe", ec);
#endif
    if (self.empty()) {
        *why = "cannot find this launcher's own path";
        return false;
    }
#if defined(_WIN32)
    const fs::path cli = self.parent_path() / "retcomm.exe";
#else
    const fs::path cli = self.parent_path() / "retcomm";
#endif
    std::error_code ec2;
    if (!fs::is_regular_file(cli, ec2)) {
        *why = "the retcomm command-line binary is not next to this launcher (" +
               cli.string() + ")";
        return false;
    }
    *exe = cli;
    *args_prefix = "launch ";
    return true;
}

uint32_t steam_shortcut_app_id(const SteamShortcutSpec& spec) {
    const std::string key = quote_exe(spec.exe) + spec.app_name;
    const mz_ulong crc = mz_crc32(MZ_CRC32_INIT,
                                  reinterpret_cast<const unsigned char*>(key.data()), key.size());
    return static_cast<uint32_t>(crc) | 0x80000000u;
}

uint64_t steam_shortcut_run_id(uint32_t app_id) {
    return (static_cast<uint64_t>(app_id) << 32) | 0x02000000ull;
}

SteamInstall find_steam_install() {
    SteamInstall out;
    std::error_code ec;
    for (const fs::path& cand : steam_root_candidates()) {
        if (cand.empty()) continue;
        const fs::path userdata = cand / "userdata";
        if (!fs::is_directory(userdata, ec)) continue;

        std::vector<std::pair<std::string, SteamUser>> users;
        for (const fs::directory_entry& d : fs::directory_iterator(userdata, ec)) {
            if (!d.is_directory(ec)) continue;
            const std::string name = d.path().filename().string();
            if (name == "0" || name == "anonymous") continue;
            if (name.find_first_not_of("0123456789") != std::string::npos) continue;
            if (!fs::is_directory(d.path() / "config", ec)) continue;
            SteamUser u;
            u.dir = d.path();
            u.account_id = name;
            users.emplace_back(name, std::move(u));
        }
        if (users.empty()) continue;

        // loginusers.vdf keys profiles by 64-bit Steam id; the userdata folder
        // is named with its low 32 bits. Collect the ids the file mentions that
        // we actually have a folder for, read their names, then copy back.
        const std::string logins = read_file(cand / "config" / "loginusers.vdf");
        if (!logins.empty()) {
            std::vector<std::pair<std::string, SteamUser>> probe;
            std::istringstream in(logins);
            for (std::string line; std::getline(in, line);) {
                const size_t a = line.find('"');
                if (a == std::string::npos) continue;
                const size_t b = line.find('"', a + 1);
                if (b == std::string::npos) continue;
                const std::string id64 = line.substr(a + 1, b - a - 1);
                if (id64.size() < 15 ||
                    id64.find_first_not_of("0123456789") != std::string::npos)
                    continue;
                const std::string id32 = account_id_from_steamid64(id64);
                for (const auto& [have, u] : users)
                    if (have == id32) probe.emplace_back(id64, SteamUser{});
            }
            parse_login_users(logins, &probe);
            for (const auto& [id64, filled] : probe) {
                const std::string id32 = account_id_from_steamid64(id64);
                for (auto& [have, u] : users)
                    if (have == id32) {
                        u.persona = filled.persona;
                        u.most_recent = filled.most_recent;
                    }
            }
        }

        out.found = true;
        out.root = cand;
        for (auto& [id, u] : users) {
            (void)id;
            out.users.push_back(std::move(u));
        }
        std::sort(out.users.begin(), out.users.end(), [](const SteamUser& a, const SteamUser& b) {
            if (a.most_recent != b.most_recent) return a.most_recent;
            return a.account_id < b.account_id;
        });
        return out;
    }
    out.hint =
        "no Steam profile found (looked for a userdata folder under the usual Steam "
        "locations); set RETCOMM_STEAM_ROOT to point at one";
    return out;
}

bool steam_is_running() {
#if defined(_WIN32)
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", 0, KEY_READ,
                      &h) != ERROR_SUCCESS)
        return false;
    DWORD pid = 0, n = sizeof(pid), type = 0;
    const bool ok = RegQueryValueExW(h, L"pid", nullptr, &type, reinterpret_cast<LPBYTE>(&pid),
                                     &n) == ERROR_SUCCESS &&
                    type == REG_DWORD;
    RegCloseKey(h);
    return ok && pid != 0;
#elif defined(__linux__)
    const char* home = std::getenv("HOME");
    if (!home || !*home) return false;
    for (const char* rel : {".steam/steam.pid", ".steam/steam/steam.pid"}) {
        const std::string text = read_file(fs::path(home) / rel);
        if (text.empty()) continue;
        const long pid = std::strtol(text.c_str(), nullptr, 10);
        if (pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    }
    return false;
#else
    return false;
#endif
}

std::vector<std::string> steam_shortcut_owner_ids(const SteamInstall& install) {
    std::vector<std::string> out;
    for (const SteamUser& u : install.users) {
        ShortcutsFile f = load_shortcuts(shortcuts_vdf(u));
        for (const VdfNode& e : f.list->children) {
            if (e.type != VdfNode::Type::Map) continue;
            const VdfNode* own = str_field(e, "DevkitGameID");
            if (own && !own->str.empty()) out.push_back(own->str);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

SteamShortcutResult steam_write_shortcut(const SteamInstall& install,
                                         const SteamShortcutSpec& spec, const SteamArtSet& art,
                                         std::string* error) {
    SteamShortcutResult result;
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        result.message = msg;
        return result;
    };
    if (!install.found || install.users.empty()) return fail(install.hint.empty()
                                                                 ? "no Steam profile"
                                                                 : install.hint);
    if (spec.app_name.empty()) return fail("shortcut needs a name");
    if (spec.exe.empty()) return fail("shortcut needs an executable");

    const uint32_t app_id = steam_shortcut_app_id(spec);
    result.app_id = app_id;

    std::vector<std::string> profiles;
    for (const SteamUser& user : install.users) {
        const fs::path vdf = shortcuts_vdf(user);
        ShortcutsFile file = load_shortcuts(vdf);

        VdfNode* entry = find_entry(file.list, spec.app_name, spec.owner_id);
        if (entry) {
            // The id is derived from exe+name, so an exe that moved leaves art
            // filed under an id nothing will read again.
            const VdfNode* old = entry->find("appid");
            if (old && old->type == VdfNode::Type::Int &&
                static_cast<uint32_t>(old->num) != app_id)
                remove_art_for(user, static_cast<uint32_t>(old->num));
        } else {
            VdfNode fresh;
            fresh.type = VdfNode::Type::Map;
            file.list->children.push_back(std::move(fresh));
            entry = &file.list->children.back();
        }

        // Key casing follows what Steam's own writer produces — "appname" and
        // "exe" lower-case, the rest mixed. Steam reads keys case-insensitively,
        // but other shortcut tools match them exactly.
        set_int(entry, "appid", static_cast<int32_t>(app_id));
        set_str(entry, "appname", spec.app_name);
        set_str(entry, "exe", quote_exe(spec.exe));
        set_str(entry, "StartDir", quote_exe(spec.start_dir));
        set_str(entry, "LaunchOptions", spec.launch_options);
        set_str(entry, "ShortcutPath", "");
        set_str(entry, "FlatpakAppID", "");
        set_str(entry, "DevkitGameID", spec.owner_id);
        set_int(entry, "IsHidden", 0);
        set_int(entry, "AllowDesktopConfig", 1);
        set_int(entry, "AllowOverlay", 1);
        set_int(entry, "OpenVR", 0);
        set_int(entry, "Devkit", 0);
        set_int(entry, "DevkitOverrideAppID", 0);
        if (!entry->find("sortas")) set_str(entry, "sortas", "");
        if (!entry->find("LastPlayTime")) set_int(entry, "LastPlayTime", 0);

        // Art first: an entry pointing at an icon that is not there yet shows
        // Steam's placeholder until the next restart.
        const fs::path grid = grid_dir(user);
        const std::string id = std::to_string(app_id);
        bool art_ok = true;
        art_ok &= copy_art(art.portrait, grid / (id + "p.png"), &result.files);
        art_ok &= copy_art(art.capsule, grid / (id + ".png"), &result.files);
        art_ok &= copy_art(art.hero, grid / (id + "_hero.png"), &result.files);
        const fs::path icon_dest = grid / (id + "_icon.png");
        art_ok &= copy_art(art.icon, icon_dest, &result.files);
        std::error_code ec;
        set_str(entry, "icon", fs::exists(icon_dest, ec) ? icon_dest.string() : std::string());
        if (!art_ok) return fail("could not write art into " + grid.string());

        VdfNode* tags = entry->find("tags");
        if (!tags) {
            VdfNode t;
            t.type = VdfNode::Type::Map;
            t.key = "tags";
            entry->children.push_back(std::move(t));
            tags = entry->find("tags");
        }
        std::vector<std::string> want = spec.tags;
        if (std::find(want.begin(), want.end(), kRetcommTag) == want.end())
            want.emplace_back(kRetcommTag);
        tags->children.clear();
        for (size_t i = 0; i < want.size(); ++i)
            tags->children.push_back(make_str(std::to_string(i).c_str(), want[i]));

        renumber(file.list);
        std::string err;
        if (!save_shortcuts(vdf, file, &err)) return fail(err);
        result.files.push_back(vdf);
        profiles.push_back(user.persona.empty() ? user.account_id : user.persona);
    }

    result.ok = true;
    std::string who;
    for (const std::string& p : profiles) {
        if (!who.empty()) who += ", ";
        who += p;
    }
    result.message = "Added \"" + spec.app_name + "\" to Steam (" + who + "), app id " +
                     std::to_string(app_id);
    return result;
}

SteamShortcutResult steam_remove_shortcut(const SteamInstall& install,
                                          const SteamShortcutSpec& spec, std::string* error) {
    SteamShortcutResult result;
    if (!install.found || install.users.empty()) {
        result.message = install.hint.empty() ? "no Steam profile" : install.hint;
        if (error) *error = result.message;
        return result;
    }
    int removed = 0;
    for (const SteamUser& user : install.users) {
        const fs::path vdf = shortcuts_vdf(user);
        ShortcutsFile file = load_shortcuts(vdf);
        for (size_t i = 0; i < file.list->children.size();) {
            VdfNode& e = file.list->children[i];
            const VdfNode* own = str_field(e, "DevkitGameID");
            const VdfNode* name = str_field(e, "appname");
            const bool ours = !spec.owner_id.empty() && own && own->str == spec.owner_id;
            const bool adopted = (!own || own->str.empty()) && name &&
                                 name->str == spec.app_name;
            if (ours || adopted) {
                const VdfNode* id = e.find("appid");
                if (id && id->type == VdfNode::Type::Int)
                    remove_art_for(user, static_cast<uint32_t>(id->num));
                file.list->children.erase(file.list->children.begin() +
                                          static_cast<long>(i));
                ++removed;
                continue;
            }
            ++i;
        }
        if (removed == 0) continue;
        renumber(file.list);
        std::string err;
        if (!save_shortcuts(vdf, file, &err)) {
            if (error) *error = err;
            result.message = err;
            return result;
        }
        result.files.push_back(vdf);
    }
    result.ok = true;
    result.message = removed > 0 ? "Removed \"" + spec.app_name + "\" from Steam"
                                 : "\"" + spec.app_name + "\" was not in Steam";
    return result;
}

}  // namespace retcomm
