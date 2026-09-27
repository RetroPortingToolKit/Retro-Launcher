#include "retcomm/mods.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace retcomm {
namespace {

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim(std::string s) {
    const auto ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

// TOML values, to the extent a manifest uses them: a quoted string, a bool, or
// a bare token. Enough for the fields shown here and nothing more — a real
// parser belongs in the engine, which is the thing that has to resolve them.
std::string unquote(std::string v) {
    v = trim(std::move(v));
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                          (v.front() == '\'' && v.back() == '\''))) {
        v = v.substr(1, v.size() - 2);
    }
    // Only the escapes a manifest realistically carries in prose fields.
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            switch (v[++i]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                default: out += v[i]; break;
            }
        } else {
            out += v[i];
        }
    }
    return out;
}

bool truthy(const std::string& v) {
    const std::string t = unquote(v);
    return t == "true" || t == "1" || t == "yes";
}

// Walk a TOML document, reporting (section, is_array_of_tables, key, value).
// Section is the raw header text, e.g. "feature" for [[feature]]. Top-level
// keys arrive with an empty section.
template <typename Fn>
void for_each_toml_pair(const std::string& body, Fn&& fn) {
    std::istringstream in(body);
    std::string line;
    std::string section;
    bool array_of_tables = false;
    while (std::getline(in, line)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        if (s.front() == '[') {
            array_of_tables = s.size() > 1 && s[1] == '[';
            const size_t open = array_of_tables ? 2 : 1;
            const size_t close = s.find(array_of_tables ? "]]" : "]", open);
            section = close == std::string::npos ? std::string()
                                                 : trim(s.substr(open, close - open));
            fn(section, array_of_tables, std::string(), std::string());
            continue;
        }
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(s.substr(0, eq));
        std::string val = trim(s.substr(eq + 1));
        // Strip a trailing comment outside quotes.
        bool in_str = false;
        for (size_t i = 0; i < val.size(); ++i) {
            if (val[i] == '"') in_str = !in_str;
            else if (val[i] == '#' && !in_str) { val = trim(val.substr(0, i)); break; }
        }
        fn(section, array_of_tables, key, val);
    }
}

bool read_manifest(const fs::path& path, ModPackageInfo& out, std::string* error) {
    const std::string body = read_file(path);
    if (body.empty()) {
        if (error) *error = path.string() + ": unreadable or empty";
        return false;
    }
    ModFeatureInfo pending;
    ModOptionInfo pending_opt;
    bool in_feature = false;
    bool in_option = false;
    auto flush = [&] {
        if (in_feature && !pending.id.empty()) {
            pending.enabled = pending.default_enabled;
            out.features.push_back(pending);
        }
        if (in_option && !pending_opt.id.empty()) {
            pending_opt.value = pending_opt.default_value;
            out.options.push_back(pending_opt);
        }
        pending = ModFeatureInfo{};
        pending_opt = ModOptionInfo{};
    };

    for_each_toml_pair(body, [&](const std::string& section, bool aot, const std::string& key,
                                 const std::string& val) {
        if (key.empty()) {                       // a section header
            // [[option.choice]] belongs to the option above it, so the option
            // must stay open across it rather than being flushed.
            if (section == "option.choice" && aot) {
                if (in_option) pending_opt.choices.push_back(retcomm::ModChoice{});
                return;
            }
            flush();
            in_feature = (section == "feature" && aot);
            in_option = (section == "option" && aot);
            return;
        }
        if (in_option && !pending_opt.choices.empty() && section == "option.choice") {
            ModChoice& c = pending_opt.choices.back();
            if (key == "value") c.value = unquote(val);
            else if (key == "label") c.label = unquote(val);
            return;
        }
        if (in_feature) {
            if (key == "id") pending.id = unquote(val);
            else if (key == "name") pending.name = unquote(val);
            else if (key == "description") pending.description = unquote(val);
            else if (key == "group") pending.group = unquote(val);
            else if (key == "channel") pending.channel = unquote(val);
            else if (key == "default_enabled") pending.default_enabled = truthy(val);
            return;
        }
        if (in_option) {
            if (key == "feature") pending_opt.feature_id = unquote(val);
            else if (key == "id") pending_opt.id = unquote(val);
            else if (key == "label") pending_opt.label = unquote(val);
            else if (key == "description") pending_opt.description = unquote(val);
            else if (key == "group") pending_opt.group = unquote(val);
            else if (key == "type") pending_opt.type = unquote(val);
            else if (key == "default") pending_opt.default_value = unquote(val);
            else if (key == "disabled_by") pending_opt.disabled_by = unquote(val);
            else if (key == "min") { pending_opt.min = std::atol(unquote(val).c_str());
                                     pending_opt.has_range = true; }
            else if (key == "max") { pending_opt.max = std::atol(unquote(val).c_str());
                                     pending_opt.has_range = true; }
            else if (key == "step") pending_opt.step = std::atol(unquote(val).c_str());
            return;
        }
        if (!section.empty()) return;            // [[target]], [[patch]], … not shown here
        if (key == "id") out.id = unquote(val);
        else if (key == "version") out.version = unquote(val);
        else if (key == "name") out.name = unquote(val);
        else if (key == "author") out.author = unquote(val);
        else if (key == "description") out.description = unquote(val);
        else if (key == "license") out.license = unquote(val);
        else if (key == "channel") out.channel = unquote(val);
        else if (key == "resolver") out.builtin = (unquote(val) == "builtin");
    });
    flush();

    if (out.id.empty()) {
        if (error) *error = path.string() + ": manifest has no id";
        return false;
    }
    if (out.name.empty()) out.name = out.id;
    for (ModFeatureInfo& f : out.features) {
        if (f.channel.empty()) f.channel = out.channel;
        if (f.name.empty()) f.name = f.id;
        if (f.group.empty()) f.group = "General";
    }
    for (ModOptionInfo& o : out.options) {
        if (o.label.empty()) o.label = o.id;
        for (ModChoice& c : o.choices)
            if (c.label.empty()) c.label = c.value;
    }
    return true;
}

// state.toml: [[package]] carries id/enabled for an all-or-nothing package,
// [[feature]] carries package_id/feature_id/enabled for a featured one.
void apply_state(const fs::path& state_path, std::vector<ModPackageInfo>& packages) {
    const std::string body = read_file(state_path);
    if (body.empty()) return;

    std::string sec;
    std::string pkg_id, feat_id;
    bool have_enabled = false, enabled = false;
    // The block a following `.values` sub-table belongs to, remembered across
    // the commit that the sub-table's own header triggers.
    std::string values_pkg, values_feat;

    auto commit = [&] {
        if (sec == "package" && !pkg_id.empty() && have_enabled) {
            for (ModPackageInfo& p : packages)
                if (p.id == pkg_id) p.enabled = enabled;
        } else if (sec == "feature" && !pkg_id.empty() && !feat_id.empty() && have_enabled) {
            for (ModPackageInfo& p : packages) {
                if (p.id != pkg_id) continue;
                for (ModFeatureInfo& f : p.features)
                    if (f.id == feat_id) f.enabled = enabled;
            }
        }
        if (sec == "package" || sec == "feature") {
            values_pkg = pkg_id;
            values_feat = (sec == "feature") ? feat_id : std::string();
        }
        pkg_id.clear();
        feat_id.clear();
        have_enabled = false;
        enabled = false;
    };

    // `id` means different things by block: the package in [[package]], the
    // FEATURE in [[feature]] (whose package is `package_id`). That is what
    // ModPackageManager::save_state writes and what its loader reads back —
    // there is no `feature_id` key, so one is not accepted here either.
    for_each_toml_pair(body, [&](const std::string& section, bool, const std::string& key,
                                 const std::string& val) {
        if (key.empty()) {
            commit();
            sec = section;
            return;
        }
        if (sec == "package") {
            if (key == "id") pkg_id = unquote(val);
        } else if (sec == "feature") {
            if (key == "package_id") pkg_id = unquote(val);
            else if (key == "id") feat_id = unquote(val);
        }
        if (sec == "feature.values" || sec == "package.values") {
            for (ModPackageInfo& p : packages) {
                if (p.id != values_pkg) continue;
                for (ModOptionInfo& o : p.options) {
                    if (o.id != key) continue;
                    if (!values_feat.empty() && o.feature_id != values_feat) continue;
                    o.value = unquote(val);
                }
            }
            return;
        }
        if (key == "enabled") { enabled = truthy(val); have_enabled = true; }
    });
    commit();
}

// Walk anything under mods/ looking for manifests, for trees that are not the
// two-level <id>/<version> shape: psxrecomp's pre-split mods/packages, a
// hand-dropped package at mods/<id>/manifest.toml, and whatever a different
// engine lays out. Bounded depth, because this runs inside a game directory
// that may also hold a large asset tree.
void scan_recursive(const fs::path& root, ModScanResult& out,
                    std::vector<std::string>& seen) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    constexpr int kMaxDepth = 6;
    auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() >= kMaxDepth) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        if (it->path().filename() != "manifest.toml") continue;
        const std::string key = it->path().string();
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);

        ModPackageInfo pkg;
        // Origin from where it sits: anything not under installed/ is build
        // output as far as "do not put player files here" is concerned.
        pkg.origin = key.find("/installed/") != std::string::npos ? ModOrigin::Installed
                                                                 : ModOrigin::Bundled;
        if (key.find("/provided/") != std::string::npos) pkg.builtin = true;
        pkg.manifest = it->path();
        std::string err;
        if (read_manifest(it->path(), pkg, &err)) {
            if (pkg.version.empty())
                pkg.version = it->path().parent_path().filename().string();
            out.packages.push_back(std::move(pkg));
        } else {
            out.errors.push_back(err);
        }
    }
}

void scan_origin(const fs::path& root, ModOrigin origin, ModScanResult& out) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    for (auto id_it = fs::directory_iterator(root, ec);
         !ec && id_it != fs::directory_iterator(); id_it.increment(ec)) {
        if (!id_it->is_directory(ec)) continue;
        for (auto ver_it = fs::directory_iterator(id_it->path(), ec);
             !ec && ver_it != fs::directory_iterator(); ver_it.increment(ec)) {
            if (!ver_it->is_directory(ec)) continue;
            const fs::path manifest = ver_it->path() / "manifest.toml";
            if (!fs::is_regular_file(manifest, ec)) continue;
            ModPackageInfo pkg;
            pkg.origin = origin;
            pkg.manifest = manifest;
            std::string err;
            if (read_manifest(manifest, pkg, &err)) {
                if (pkg.version.empty()) pkg.version = ver_it->path().filename().string();
                out.packages.push_back(std::move(pkg));
            } else {
                out.errors.push_back(err);
            }
        }
    }
}

// One [[package]] / [[feature]] block: the header line and everything up to the
// next top-level header. Sub-tables ([package.values], [feature.values]) belong
// to the block they follow and are carried along untouched.
struct StateBlock {
    std::string kind;     // "package" | "feature"
    size_t begin = 0;     // index of the header line
    size_t end = 0;       // one past the block's last line
    std::string id;       // package id, or feature id in a [[feature]]
    std::string package_id;
    long enabled_line = -1;
};

std::vector<std::string> split_lines(const std::string& body) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : body) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool is_top_header(const std::string& line, std::string* kind, bool* aot) {
    const std::string t = trim(line);
    if (t.empty() || t.front() != '[') return false;
    const bool a = t.size() > 1 && t[1] == '[';
    const size_t open = a ? 2 : 1;
    const size_t close = t.find(a ? "]]" : "]", open);
    if (close == std::string::npos) return false;
    const std::string name = trim(t.substr(open, close - open));
    // A sub-table keeps its parent block; only a top-level name starts a new one.
    if (!a && name.find('.') != std::string::npos) return false;
    if (kind) *kind = name;
    if (aot) *aot = a;
    return true;
}

std::vector<StateBlock> find_blocks(const std::vector<std::string>& lines) {
    std::vector<StateBlock> blocks;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string kind;
        bool aot = false;
        if (!is_top_header(lines[i], &kind, &aot)) continue;
        if (!aot || (kind != "package" && kind != "feature")) continue;
        StateBlock b;
        b.kind = kind;
        b.begin = i;
        size_t j = i + 1;
        for (; j < lines.size(); ++j) {
            std::string next_kind;
            bool next_aot = false;
            if (is_top_header(lines[j], &next_kind, &next_aot)) break;
            const std::string t = trim(lines[j]);
            const size_t eq = t.find('=');
            if (t.empty() || t[0] == '#' || eq == std::string::npos) continue;
            const std::string key = trim(t.substr(0, eq));
            const std::string val = trim(t.substr(eq + 1));
            if (key == "id") b.id = unquote(val);
            else if (key == "package_id") b.package_id = unquote(val);
            else if (key == "enabled") b.enabled_line = static_cast<long>(j);
        }
        b.end = j;
        blocks.push_back(std::move(b));
        i = j - 1;
    }
    return blocks;
}

} // namespace

bool set_mod_enabled(const fs::path& game_dir, const std::string& package_id,
                     const std::string& feature_id, bool enabled, std::string* error) {
    if (game_dir.empty() || package_id.empty()) {
        if (error) *error = "no game directory or package id";
        return false;
    }
    const fs::path root = game_dir / "mods";
    const fs::path path = root / "state.toml";
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        if (error) *error = "cannot create " + root.string() + ": " + ec.message();
        return false;
    }

    std::vector<std::string> lines = split_lines(read_file(path));
    const std::string want = enabled ? "enabled = true" : "enabled = false";

    // [[feature]] blocks are only read at format_version 2, so a file still
    // declaring 1 has to be upgraded or the write would be silently ignored.
    bool saw_version = false;
    for (std::string& l : lines) {
        const std::string t = trim(l);
        if (t.rfind("format_version", 0) != 0) continue;
        saw_version = true;
        if (!feature_id.empty()) l = "format_version = 2";
        break;
    }
    if (!saw_version) lines.insert(lines.begin(), "format_version = 2");

    const std::vector<StateBlock> blocks = find_blocks(lines);
    const StateBlock* hit = nullptr;
    for (const StateBlock& b : blocks) {
        if (feature_id.empty()) {
            if (b.kind == "package" && b.id == package_id) { hit = &b; break; }
        } else if (b.kind == "feature" && b.package_id == package_id && b.id == feature_id) {
            hit = &b;
            break;
        }
    }

    if (hit && hit->enabled_line >= 0) {
        lines[static_cast<size_t>(hit->enabled_line)] = want;
    } else if (hit) {
        // Block exists without the key (a hand-edited file); put it directly
        // under the header, ahead of any sub-table the block carries.
        lines.insert(lines.begin() + static_cast<long>(hit->begin) + 1, want);
    } else {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
        if (feature_id.empty()) {
            lines.push_back("[[package]]");
            lines.push_back("id = \"" + package_id + "\"");
        } else {
            lines.push_back("[[feature]]");
            lines.push_back("package_id = \"" + package_id + "\"");
            lines.push_back("id = \"" + feature_id + "\"");
        }
        lines.push_back(want);
    }

    std::string out;
    for (const std::string& l : lines) { out += l; out += '\n'; }

    // Same publish the engine uses: write beside, then rename over, so a
    // half-written state file never becomes the state file.
    const fs::path temp = root / "state.toml.tmp";
    {
        std::ofstream f(temp, std::ios::trunc | std::ios::binary);
        if (!f) {
            if (error) *error = "cannot write " + temp.string();
            return false;
        }
        f << out;
        if (!f) {
            if (error) *error = "cannot finish " + temp.string();
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        ec.clear();
        fs::remove(path, ec);
        ec.clear();
        fs::rename(temp, path, ec);
    }
    if (ec) {
        if (error) *error = "cannot publish state: " + ec.message();
        return false;
    }
    return true;
}

bool set_mod_option(const fs::path& game_dir, const std::string& package_id,
                    const std::string& feature_id, const std::string& option_id,
                    const std::string& value, std::string* error) {
    if (game_dir.empty() || package_id.empty() || option_id.empty()) {
        if (error) *error = "no game directory, package or option";
        return false;
    }
    const fs::path root = game_dir / "mods";
    const fs::path path = root / "state.toml";
    std::error_code ec;
    fs::create_directories(root, ec);

    std::vector<std::string> lines = split_lines(read_file(path));
    const bool featured = !feature_id.empty();
    const std::string sub = featured ? "[feature.values]" : "[package.values]";
    const std::string want = option_id + " = \"" + value + "\"";

    bool saw_version = false;
    for (std::string& l : lines) {
        if (trim(l).rfind("format_version", 0) != 0) continue;
        saw_version = true;
        if (featured) l = "format_version = 2";
        break;
    }
    if (!saw_version) lines.insert(lines.begin(), "format_version = 2");

    // The block must exist before it can carry values; enabling creates it.
    std::vector<StateBlock> blocks = find_blocks(lines);
    const StateBlock* hit = nullptr;
    for (const StateBlock& b : blocks) {
        if (featured) {
            if (b.kind == "feature" && b.package_id == package_id && b.id == feature_id) {
                hit = &b;
                break;
            }
        } else if (b.kind == "package" && b.id == package_id) {
            hit = &b;
            break;
        }
    }
    if (!hit) {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
        const size_t begin = lines.size();
        if (featured) {
            lines.push_back("[[feature]]");
            lines.push_back("package_id = \"" + package_id + "\"");
            lines.push_back("id = \"" + feature_id + "\"");
            // The loader requires `enabled` on a [[feature]]; a block without
            // it throws and takes the whole state file with it.
            lines.push_back("enabled = true");
        } else {
            lines.push_back("[[package]]");
            lines.push_back("id = \"" + package_id + "\"");
        }
        lines.push_back(sub);
        lines.push_back(want);
        (void)begin;
    } else {
        // Find the values sub-table inside the block, then the key inside it.
        long sub_line = -1;
        for (size_t i = hit->begin; i < hit->end; ++i)
            if (trim(lines[i]) == sub) { sub_line = static_cast<long>(i); break; }
        if (sub_line < 0) {
            lines.insert(lines.begin() + static_cast<long>(hit->end), sub);
            lines.insert(lines.begin() + static_cast<long>(hit->end) + 1, want);
        } else {
            bool replaced = false;
            for (size_t i = static_cast<size_t>(sub_line) + 1; i < hit->end; ++i) {
                const std::string t = trim(lines[i]);
                if (t.empty() || t[0] == '#') continue;
                if (t[0] == '[') break;             // next sub-table
                const size_t eq = t.find('=');
                if (eq == std::string::npos) continue;
                if (trim(t.substr(0, eq)) != option_id) continue;
                lines[i] = want;
                replaced = true;
                break;
            }
            if (!replaced)
                lines.insert(lines.begin() + sub_line + 1, want);
        }
    }

    std::string out;
    for (const std::string& l : lines) { out += l; out += '\n'; }
    const fs::path temp = root / "state.toml.tmp";
    {
        std::ofstream f(temp, std::ios::trunc | std::ios::binary);
        if (!f) {
            if (error) *error = "cannot write " + temp.string();
            return false;
        }
        f << out;
        if (!f) {
            if (error) *error = "cannot finish " + temp.string();
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        ec.clear();
        fs::remove(path, ec);
        ec.clear();
        fs::rename(temp, path, ec);
    }
    if (ec) {
        if (error) *error = "cannot publish state: " + ec.message();
        return false;
    }
    return true;
}

const char* mod_origin_name(ModOrigin origin) {
    return origin == ModOrigin::Bundled ? "bundled" : "installed";
}

namespace {

// Mods trees under the install that are not the game's own. A build stages
// packages into the release dir; when it has not, they sit in the source tree
// and the game cannot see them. Finding them is how an empty list explains
// itself rather than just being empty.
void find_unstaged(const fs::path& install_root, const fs::path& game_root, ModScanResult& r) {
    if (install_root.empty()) return;
    std::error_code ec;
    if (!fs::is_directory(install_root, ec)) return;
    const fs::path game_canon = fs::weakly_canonical(game_root, ec);
    constexpr int kMaxDepth = 5;
    auto it = fs::recursive_directory_iterator(
        install_root / "src", fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() >= kMaxDepth) { it.disable_recursion_pending(); continue; }
        if (!it->is_directory(ec) || it->path().filename() != "mods") continue;
        std::error_code cec;
        if (fs::weakly_canonical(it->path(), cec) == game_canon) continue;
        int n = 0;
        std::error_code mec;
        auto mit = fs::recursive_directory_iterator(
            it->path(), fs::directory_options::skip_permission_denied, mec);
        for (; !mec && mit != fs::recursive_directory_iterator(); mit.increment(mec))
            if (mit->is_regular_file(mec) && mit->path().filename() == "manifest.toml") ++n;
        if (n > 0) {
            r.unstaged_roots.push_back(it->path());
            r.unstaged_manifests += n;
        }
        it.disable_recursion_pending();
    }
}

// ---- n64lle guarded-write packages ------------------------------------------
//
// Read to show and toggle them, never to apply them: the resolver that decides
// what actually lands (guards, contested bytes, the target image) is n64lle's
// (crates/n64lle-host mod_catalog / mod_plan), and a refusal is reported by the
// game at boot. What this reader mirrors is only what a page must not get
// wrong: which packages the game will see, and what mods.toml means.

std::string unquote_key(const std::string& k) {
    std::string t = trim(k);
    if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'') && t.back() == t.front())
        t = t.substr(1, t.size() - 2);
    return t;
}

// mods/<origin>/<id>/manifest.toml whose document has a [target] rom_sha256:
// the shape only an n64lle package has.
bool is_n64lle_manifest(const fs::path& manifest) {
    bool target_sha = false;
    for_each_toml_pair(read_file(manifest), [&](const std::string& section, bool aot,
                                                 const std::string& key, const std::string&) {
        if (!aot && section == "target" && key == "rom_sha256") target_sha = true;
    });
    return target_sha;
}

bool read_n64lle_manifest(const fs::path& path, ModPackageInfo& out, std::string* error) {
    const std::string body = read_file(path);
    if (body.empty()) {
        if (error) *error = path.string() + ": unreadable or empty";
        return false;
    }
    std::string game_id;
    std::vector<std::pair<std::string, int>> writes; // feature -> guarded writes
    std::string patch_feature;
    auto count_patch = [&] {
        if (patch_feature.empty()) return;
        for (auto& w : writes)
            if (w.first == patch_feature) { ++w.second; patch_feature.clear(); return; }
        writes.emplace_back(patch_feature, 1);
        patch_feature.clear();
    };
    for_each_toml_pair(body, [&](const std::string& section, bool, const std::string& key,
                                 const std::string& val) {
        if (key.empty()) {
            count_patch();
            if (section.rfind("feature.", 0) == 0) {
                ModFeatureInfo f;
                f.id = section.substr(8);
                f.name = f.id; // n64lle manifests carry no display names
                out.features.push_back(std::move(f));
            }
            return;
        }
        if (section.empty()) {
            if (key == "id") out.id = unquote(val);
            else if (key == "version") out.version = unquote(val);
        } else if (section == "target" && key == "game_id") {
            game_id = unquote(val);
        } else if (section.rfind("feature.", 0) == 0 && key == "default_enabled" &&
                   !out.features.empty()) {
            out.features.back().default_enabled = truthy(val);
        } else if (section.rfind("patch.", 0) == 0 && key == "feature") {
            patch_feature = unquote(val);
        }
    });
    count_patch();
    if (out.id.empty()) {
        if (error) *error = path.string() + ": no id";
        return false;
    }
    // mod_catalog.rs: a directory whose name is not its manifest's id is refused.
    if (path.parent_path().filename().string() != out.id) {
        if (error)
            *error = path.string() + ": id '" + out.id + "' is not its directory's name '" +
                     path.parent_path().filename().string() + "'; the game refuses it";
        return false;
    }
    out.name = out.id;
    if (!game_id.empty()) out.description = "For " + game_id + ".";
    for (ModFeatureInfo& f : out.features) {
        int n = 0;
        for (const auto& w : writes)
            if (w.first == f.id) n = w.second;
        f.description = std::to_string(n) + " guarded write" + (n == 1 ? "" : "s") + ".";
        f.group = out.id; // no groups in this format: the page heads each by its package
        f.enabled = f.default_enabled;
    }
    return true;
}

void scan_n64lle_origin(const fs::path& root, ModOrigin origin, ModScanResult& out) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const fs::path manifest = it->path() / "manifest.toml";
        if (!it->is_directory(ec) || !fs::is_regular_file(manifest, ec)) continue;
        ModPackageInfo pkg;
        pkg.origin = origin;
        pkg.manifest = manifest;
        std::string err;
        if (read_n64lle_manifest(manifest, pkg, &err)) out.packages.push_back(std::move(pkg));
        else out.errors.push_back(err);
    }
}

// mods.toml (MODDING.md §5.3): a package with a section runs exactly the
// features its `enabled` lists; one without runs its manifest defaults.
void apply_n64lle_selection(const fs::path& path, ModScanResult& r) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    const std::string body = read_file(path);
    std::vector<std::pair<std::string, std::string>> sel; // package -> enabled list
    for_each_toml_pair(body, [&](const std::string& section, bool, const std::string& key,
                                 const std::string& val) {
        const std::string pkg = unquote_key(section);
        if (key.empty()) {
            if (!pkg.empty()) sel.emplace_back(pkg, std::string());
            return;
        }
        if (key == "enabled" && !sel.empty() && sel.back().first == pkg)
            sel.back().second = unquote(val);
    });
    for (ModPackageInfo& p : r.packages) {
        for (const auto& [id, list] : sel) {
            if (id != p.id) continue;
            std::istringstream words(list);
            std::vector<std::string> on;
            for (std::string w; words >> w;) on.push_back(w);
            for (ModFeatureInfo& f : p.features)
                f.enabled = std::find(on.begin(), on.end(), f.id) != on.end();
        }
    }
}

// Rewrites (or appends) the package's section so `enabled` lists exactly
// `on`. Every other line of mods.toml stays as it was.
bool write_n64lle_selection(const fs::path& game_dir, const ModPackageInfo& pkg,
                            const std::vector<std::string>& on, std::string* error) {
    const fs::path path = game_dir / "mods.toml";
    std::string list;
    for (const std::string& f : on) list += (list.empty() ? "" : " ") + f;
    const std::string enabled_line = "enabled = \"" + list + "\"";

    std::vector<std::string> lines = split_lines(read_file(path));
    long header = -1, end = static_cast<long>(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trim(lines[i]);
        if (t.empty() || t[0] != '[') continue;
        if (header >= 0) { end = static_cast<long>(i); break; }
        const size_t close = t.find(']');
        if (close != std::string::npos && unquote_key(t.substr(1, close - 1)) == pkg.id)
            header = static_cast<long>(i);
    }
    if (header < 0) {
        if (lines.empty()) {
            lines.push_back("# Which mod features are on (n64lle docs/MODDING.md 5.3).");
            lines.push_back("# Written by the launcher's Mods page; a package with no");
            lines.push_back("# section runs its manifest's defaults.");
        }
        lines.push_back("");
        // Bare, dots and all, as n64lle's own writer emits it
        // (mod_selection.rs): its reader takes the header text whole, so a
        // quoted ["a.b"] would name a package called "\"a.b\"".
        lines.push_back("[" + pkg.id + "]");
        lines.push_back("version = \"" + pkg.version + "\"");
        lines.push_back(enabled_line);
    } else {
        bool replaced = false;
        for (long i = header + 1; i < end; ++i) {
            const std::string t = trim(lines[static_cast<size_t>(i)]);
            if (t.rfind("enabled", 0) == 0 && t.find('=') != std::string::npos) {
                lines[static_cast<size_t>(i)] = enabled_line;
                replaced = true;
                break;
            }
        }
        if (!replaced) lines.insert(lines.begin() + header + 1, enabled_line);
    }
    std::string out;
    for (const std::string& l : lines) out += l + "\n";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f || !(f << out)) {
        if (error) *error = path.string() + ": cannot write";
        return false;
    }
    return true;
}

ModScanResult scan_n64lle_mods(const fs::path& game_dir) {
    ModScanResult r;
    r.layout = ModLayout::N64lle;
    r.root = game_dir / "mods";
    std::error_code ec;
    r.root_exists = fs::is_directory(r.root, ec);
    scan_n64lle_origin(r.root / "bundled", ModOrigin::Bundled, r);
    scan_n64lle_origin(r.root / "installed", ModOrigin::Installed, r);
    // mod_catalog.rs: the same id in both roots refuses both; neither shadows
    // the other. Shown as an error, not as two rows the game will not run.
    std::vector<ModPackageInfo> kept;
    for (const ModPackageInfo& p : r.packages) {
        const auto n = std::count_if(r.packages.begin(), r.packages.end(),
                                     [&](const ModPackageInfo& q) { return q.id == p.id; });
        if (n == 1) kept.push_back(p);
        else if (p.origin == ModOrigin::Bundled)
            r.errors.push_back(p.id + " is in both mods/bundled and mods/installed; the game "
                                      "refuses both until one is removed");
    }
    r.packages = std::move(kept);
    apply_n64lle_selection(game_dir / "mods.toml", r);
    std::sort(r.packages.begin(), r.packages.end(),
              [](const ModPackageInfo& a, const ModPackageInfo& b) { return a.id < b.id; });
    return r;
}

// Which layout a game's mods tree is in: n64lle when mods.toml is there or any
// package directly under an origin has an n64lle manifest.
bool is_n64lle_tree(const fs::path& game_dir) {
    std::error_code ec;
    if (fs::is_regular_file(game_dir / "mods.toml", ec)) return true;
    for (const char* origin : {"bundled", "installed"}) {
        const fs::path root = game_dir / "mods" / origin;
        if (!fs::is_directory(root, ec)) continue;
        for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            const fs::path m = it->path() / "manifest.toml";
            if (fs::is_regular_file(m, ec) && is_n64lle_manifest(m)) return true;
        }
    }
    return false;
}

} // namespace

ModScanResult scan_game_mods(const fs::path& game_dir, const fs::path& install_root) {
    ModScanResult r;
    if (game_dir.empty()) return r;
    if (is_n64lle_tree(game_dir)) return scan_n64lle_mods(game_dir);
    r.root = game_dir / "mods";
    std::error_code ec;
    r.root_exists = fs::is_directory(r.root, ec);
    if (!r.root_exists) {
        find_unstaged(install_root, r.root, r);
        return r;
    }

    scan_origin(r.root / "bundled", ModOrigin::Bundled, r);
    scan_origin(r.root / "installed", ModOrigin::Installed, r);
    // The two known roots are the fast, exact path. Anything they missed gets
    // found by walking — a package in an unexpected place is still a package,
    // and "nothing here" when a manifest plainly exists is the worse answer.
    {
        std::vector<std::string> seen;
        seen.reserve(r.packages.size());
        for (const ModPackageInfo& p : r.packages) seen.push_back(p.manifest.string());
        scan_recursive(r.root, r, seen);
    }
    apply_state(r.root / "state.toml", r.packages);

    if (r.packages.empty()) find_unstaged(install_root, r.root, r);

    std::sort(r.packages.begin(), r.packages.end(),
              [](const ModPackageInfo& a, const ModPackageInfo& b) {
                  if (a.name != b.name) return a.name < b.name;
                  return a.version < b.version;
              });
    return r;
}

bool set_scanned_mod_enabled(const ModScanResult& scan, const fs::path& game_dir,
                             const ModPackageInfo& pkg, const std::string& feature_id,
                             bool enabled, std::string* error) {
    if (scan.layout == ModLayout::Engine)
        return set_mod_enabled(game_dir, pkg.id, feature_id, enabled, error);
    std::vector<std::string> on;
    for (const ModFeatureInfo& f : pkg.features)
        if (f.id == feature_id ? enabled : f.enabled) on.push_back(f.id);
    return write_n64lle_selection(game_dir, pkg, on, error);
}

bool set_scanned_mod_all(const ModScanResult& scan, const fs::path& game_dir,
                         const ModPackageInfo& pkg, bool enabled, std::string* error) {
    if (scan.layout == ModLayout::Engine) {
        if (!pkg.has_features()) return set_mod_enabled(game_dir, pkg.id, {}, enabled, error);
        bool ok = true;
        for (const ModFeatureInfo& f : pkg.features)
            ok &= set_mod_enabled(game_dir, pkg.id, f.id, enabled, error);
        return ok;
    }
    std::vector<std::string> on;
    if (enabled)
        for (const ModFeatureInfo& f : pkg.features) on.push_back(f.id);
    return write_n64lle_selection(game_dir, pkg, on, error);
}

} // namespace retcomm
