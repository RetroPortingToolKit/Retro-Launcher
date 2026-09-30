// hub_update: Direct mode's Update page without a window or a network. Every
// manifest is a file:// one (RETRO_CORE_MANIFEST_URL, RETRO_HUB_MANIFEST_URL),
// every archive a tar.gz made here. Run as
// `retro-hub-update-test <scratch dir> <a built retro-hub>`: the hub install is
// checked with a real hub, which must answer --version.
//
// The "cores" are a few bytes of text with a real sidecar: nothing loads them.

#include "hub/hub_title.hpp"
#include "hub/hub_update.hpp"
#include "retcomm/config.hpp"
#include "retcomm/core_titles.hpp"
#include "retcomm/fs_util.hpp"
#include "retcomm/hash.hpp"
#include "retcomm/runtime_update.hpp"
#include "link_protocol.hpp" // Retro-Runtime corelink: kProtocolMajor
#include "rcore/rcore.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace retcomm::hub;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

void write(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}

void set_env(const char* k, const std::string& v) {
#if defined(_WIN32)
    _putenv_s(k, v.c_str());
#else
    ::setenv(k, v.c_str(), 1);
#endif
}

std::string file_url(const fs::path& p) { return "file://" + fs::absolute(p).generic_string(); }

void write_sidecar(const fs::path& lib, const std::string& version) {
    fs::path m = lib;
    m.replace_extension(".rcore.toml");
    write(m, "# GENERATED\n[core]\nabi_major = 0\nid = \"n64lle\"\nversion = \"" + version +
                 "\"\nlibrary = \"" + lib.filename().string() + "\"\nplatforms = [\"n64\"]\n");
    write(lib, "core " + version);
}

// A release prefix holding lib/<core> + sidecar, as n64lle packages it.
fs::path make_core_archive(const fs::path& scratch, const std::string& version) {
    const std::string root = "n64lle-core-" + version + "-linux-x86_64";
    const fs::path tree = scratch / "src" / version;
    write_sidecar(tree / root / "lib" / "n64lle_core.so", version);
    write(tree / root / "bin" / "romscan", "tool");
    const fs::path archive = scratch / (root + ".tar.gz");
    const std::string cmd = "tar -czf '" + archive.string() + "' -C '" + tree.string() + "' " + root;
    check(std::system(cmd.c_str()) == 0, "tar makes the core archive");
    return archive;
}

void write_core_manifest(const fs::path& at, const std::string& version, int module_abi,
                         const fs::path& archive, const std::string& sha_override = {}) {
    const std::string sha = sha_override.empty()
                                ? retcomm::to_lower_hex(retcomm::file_sha256_hex(archive))
                                : sha_override;
    write(at, R"({"schema": 1, "name": "n64lle", "version": ")" + version +
                  R"(", "commit": "abc", "module_abi": )" + std::to_string(module_abi) +
                  R"(, "rcore_abi": {"major": 0, "minor": 0, "draft_revision": )" +
                  std::to_string(RCORE_DRAFT_REVISION) + R"(},
  "core": {"linux-x86_64": {"url": ")" + file_url(archive) + R"(", "archive": ")" +
                  archive.filename().string() + R"(", "sha256": ")" + sha + R"(", "size": )" +
                  std::to_string(fs::file_size(archive)) + R"(, "root": "n64lle-core-)" + version +
                  R"(-linux-x86_64"}}})");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: retro-hub-update-test <scratch dir> <retro-hub>\n");
        return 2;
    }
    const fs::path scratch = fs::absolute(argv[1]);
    const fs::path built_hub = fs::absolute(argv[2]);
    fs::remove_all(scratch);
    fs::create_directories(scratch);

    // ---- the payload: a generic core and a game package ------------------------
    const fs::path payload = scratch / "app" / "title";
    const fs::path bundled = payload / "core" / "n64lle_core.so";
    write_sidecar(bundled, "0.375.0");
    const fs::path package = payload / "package" / "pokemonstadium_game.so";
    write(package, "package");
    write(payload / "package" / "pokemonstadium_game.n64game.toml",
          "# GENERATED\n[package]\ntitle_id = \"pokemonstadium\"\nmodule_abi     = 8\n"
          "core_id = \"n64lle\"\n");

    CoreSidecar sc;
    check(read_core_sidecar(bundled, sc, nullptr) && sc.id == "n64lle" && sc.version == "0.375.0" &&
              sc.library == "n64lle_core.so",
          "read_core_sidecar reads [core]");
    check(package_module_abi(package) == 8, "package_module_abi reads [package] module_abi");
    check(package_module_abi(payload / "core" / "none.so") == -1, "no descriptor: -1");

    retcomm::Paths paths;
    paths.data_dir = scratch / "data";
    paths.config_dir = scratch / "data";

    // ---- resolve_title_core ------------------------------------------------------
    auto install_core = [&](const std::string& v, int abi, const std::string& id = "n64lle") {
        const fs::path dir = core_updates_dir(paths) / v;
        write_sidecar(dir / "n64lle_core.so", v);
        write(dir / "installed.json", R"({"schema": 1, "core_id": ")" + id + R"(", "version": ")" +
                                          v + R"(", "module_abi": )" + std::to_string(abi) + "}");
    };
    check(resolve_title_core(paths, bundled, package).source == "bundled", "nothing installed: bundled");
    install_core("0.374.0", 8);
    check(resolve_title_core(paths, bundled, package).source == "bundled", "older: bundled");
    install_core("0.377.0", 9);
    check(resolve_title_core(paths, bundled, package).source == "bundled", "other module ABI: bundled");
    install_core("0.378.0", 8, "othercore");
    check(resolve_title_core(paths, bundled, package).source == "bundled", "other core id: bundled");
    install_core("0.376.0", 8);
    {
        const ResolvedCore rc = resolve_title_core(paths, bundled, package);
        check(rc.source == "updated" && rc.version == "0.376.0" &&
                  rc.path == core_updates_dir(paths) / "0.376.0" / "n64lle_core.so",
              "newer, same core, same module ABI: updated");
    }
    check(resolve_title_core(paths, bundled, {}).source == "bundled",
          "no package: the core is the game, never replaced");
    fs::remove_all(core_updates_dir(paths));

    UpdateTarget t;
    t.paths = paths;
    t.title_mode = true;
    t.title_version = "0.0.1";
    t.core = bundled;
    t.bundled_core = bundled;
    t.package = package;
    t.hub_version = "0.0.1";
    t.hub_commit = "unknown";

    // ---- the game ---------------------------------------------------------------
    {
        const UpdateItem g = check_game(t);
        check(g.ok && !g.available && !g.manual && g.message.find("update.github") != std::string::npos,
              "a title without update.github says so");
        UpdateTarget d = t;
        d.title_mode = false;
        check(check_game(d).ok && !check_game(d).manual, "Direct mode: no game channel");
    }

    // ---- the core ---------------------------------------------------------------
    const fs::path manifest = scratch / "n64lle-release-manifest.json";
    set_env("RETRO_CORE_MANIFEST_URL", file_url(manifest));
    const fs::path archive = make_core_archive(scratch, "0.376.0");
    {
        UpdateTarget pinned = t;
        pinned.core_pinned = true;
        write_core_manifest(manifest, "0.376.0", 8, archive);
        const UpdateItem c = update_core(pinned, true);
        check(c.ok && !c.available && !c.installed, "a pinned core is never replaced");
    }
    write_core_manifest(manifest, "0.376.0", 9, archive);
    {
        const UpdateItem c = update_core(t, true);
        check(c.ok && !c.available && !c.installed && c.message.find("module ABI") != std::string::npos,
              "a core for another module ABI is reported, not installed");
        check(!fs::exists(core_updates_dir(paths) / "0.376.0"), "and nothing was written");
    }
    write_core_manifest(manifest, "0.375.0", 8, archive);
    check(update_core(t, false).ok && !update_core(t, false).available, "same version: up to date");
    {
        UpdateTarget dev = t;
        dev.core_dev = true;
        const UpdateItem c = update_core(dev, false);
        check(c.ok && c.available && c.current == "0.375.0 (dev)",
              "a dev core is offered the release even at the same version");
    }
    write_core_manifest(manifest, "0.376.0", 8, archive, std::string(64, '0'));
    {
        const UpdateItem c = update_core(t, true);
        check(!c.ok && !c.installed && c.message.find("does not match") != std::string::npos,
              "a hash mismatch is refused");
    }
    write_core_manifest(manifest, "0.376.0", 8, archive);
    {
        const UpdateItem c = update_core(t, false);
        check(c.ok && c.available && c.current == "0.375.0" && c.latest == "0.376.0",
              "check only: available");
        check(!fs::exists(core_updates_dir(paths) / "0.376.0"), "check only writes nothing");
    }
    {
        const UpdateItem c = update_core(t, true);
        check(c.ok && c.installed, "install: installed");
        const fs::path dir = core_updates_dir(paths) / "0.376.0";
        check(fs::is_regular_file(dir / "n64lle_core.so") &&
                  fs::is_regular_file(dir / "n64lle_core.rcore.toml") &&
                  fs::is_regular_file(dir / "installed.json") && !fs::exists(dir / "bin"),
              "only the core, its sidecar and installed.json are kept");
        check(resolve_title_core(paths, bundled, package).source == "updated",
              "the next start picks the installed core");
        const UpdateItem again = update_core(t, false);
        check(again.ok && again.installed && !again.available,
              "a check before the restart says it is installed");
    }

    // ---- the hub ----------------------------------------------------------------
    {
        UpdateTarget dev = t;
        dev.title_mode = false;
        const UpdateItem h = update_hub(dev, true);
        check(h.ok && !h.available && h.message.find("development") != std::string::npos,
              "Direct mode from a development build does not update the hub");
    }
    {
        // build-local.sh stamps a commit, so a local build must be told apart
        // by hub_local: offered a release, it installs one that then takes
        // over every start of it (its version is the project's, older).
        UpdateTarget local = t;
        local.title_mode = false;
        local.hub_commit = "b7f454d9208bac0f3c34648698a7bb5dc3cde2eb";
        local.hub_local = true;
        const UpdateItem h = update_hub(local, true);
        check(h.ok && !h.available && !h.installed &&
                  h.message.find("development") != std::string::npos,
              "Direct mode from a local build with a commit does not update the hub");
    }
    {
        UpdateTarget dev = t;
        dev.title_mode = false;
        dev.hub_dev = true;
        const UpdateItem h = update_hub(dev, false);
        check(h.message.find("development") == std::string::npos,
              "a dev hub picked by hand is not refused as a development build");
    }
    const auto ver = probe_version(built_hub);
    const std::string hub_ver = ver.count("version") ? ver.at("version") : "";
    check(!hub_ver.empty(), "the built hub answers --version");
    {
        const fs::path tree = scratch / "hubsrc";
        fs::create_directories(tree);
        fs::copy_file(built_hub, tree / "retro-hub");
        const fs::path hub_archive = scratch / ("retro-hub-" + hub_ver + "-linux-x86_64.tar.gz");
        const std::string cmd = "tar -czf '" + hub_archive.string() + "' -C '" + tree.string() + "' retro-hub";
        check(std::system(cmd.c_str()) == 0, "tar makes the hub archive");
        const fs::path hm = scratch / "hub-manifest.json";
        auto hub_manifest = [&](int link_major, int updates = 1) {
            write(hm, R"({"schema": 1, "name": "retro-hub", "version": ")" + hub_ver +
                          R"(", "commit": "", "link_protocol": {"major": )" + std::to_string(link_major) +
                          R"(, "minor": 0}, "rcore_abi": {"major": 0, "draft_revision": )" +
                          std::to_string(RCORE_DRAFT_REVISION) + R"(},
  "direct_mode": {"cli_revision": 4, "updates": )" + std::to_string(updates) + R"(},
  "platforms": {"linux-x86_64": {"url": ")" + file_url(hub_archive) + R"(", "archive": ")" +
                          hub_archive.filename().string() + R"(", "sha256": ")" +
                          retcomm::to_lower_hex(retcomm::file_sha256_hex(hub_archive)) +
                          R"(", "size": )" + std::to_string(fs::file_size(hub_archive)) +
                          R"(, "executable": "retro-hub"}}})");
        };
        set_env("RETRO_HUB_MANIFEST_URL", file_url(hm));
        hub_manifest(99);
        {
            const UpdateItem h = update_hub(t, true);
            check(h.ok && !h.installed && h.message.find("Not installed") != std::string::npos,
                  "a hub for another link major is not installed");
        }
        const int link = static_cast<int>(retro::corelink::kProtocolMajor); // this launcher's
        hub_manifest(link, 0);
        {
            const UpdateItem h = update_hub(t, true);
            check(h.ok && !h.installed && h.message.find("predates") != std::string::npos,
                  "a hub without the Update page is not installed");
        }
        hub_manifest(link);
        {
            UpdateTarget same = t;
            same.hub_version = hub_ver;
            check(update_hub(same, false).ok && !update_hub(same, false).available,
                  "same version: up to date");
        }
        check(update_hub(t, false).available, "check only: available");
        {
            const UpdateItem h = update_hub(t, true);
            check(h.ok && h.installed, "install: installed");
        }
        const InstalledHub ih = newest_installed_hub(paths, "0.0.1", true);
        check(ih.version == hub_ver && ih.path == hub_updates_dir(paths) / hub_ver / "retro-hub",
              "the next start finds the installed hub");
        check(newest_installed_hub(paths, hub_ver, true).path.empty(),
              "a hub no newer than this one is not used");
        const UpdateItem again = update_hub(t, false);
        check(again.installed && !again.available, "a check before the restart says it is installed");
        UpdateTarget dev = t;
        dev.hub_version = hub_ver;
        dev.hub_dev = true;
        const UpdateItem d = update_hub(dev, false);
        check(d.available && !d.installed, "a dev hub is offered the release even at the same version");
    }

    // ---- the dev runner -----------------------------------------------------------
    {
        const fs::path runner = built_hub.parent_path() / "retro-core-runner";
        set_env("RETRO_HUB_DEV_RUNNER", runner.string());
        const retcomm::ResolvedRunner rr = retcomm::resolve_runner(paths, scratch / "noexe");
        check(rr.source == "dev" && rr.path == runner, "RETRO_HUB_DEV_RUNNER is used first");
        set_env("RETRO_HUB_DEV_RUNNER", (scratch / "missing").string());
        check(retcomm::resolve_runner(paths, scratch / "noexe").source != "dev",
              "a dev runner that does not run is skipped");
    }

    // ---- the config's dev paths ------------------------------------------------------
    {
        retcomm::AppConfig cfg;
        cfg.dev_core_path = "/a/core.so";
        cfg.dev_runner_path = "/a/runner";
        cfg.dev_hub_path = "/a/hub";
        cfg.show_developer_options = true;
        const fs::path cp = scratch / "config.json";
        check(retcomm::save_app_config(cp, cfg), "config saves");
        const retcomm::AppConfig back = retcomm::load_app_config(cp);
        check(back.dev_core_path == "/a/core.so" && back.dev_runner_path == "/a/runner" &&
                  back.dev_hub_path == "/a/hub" && back.show_developer_options,
              "dev paths and developer options round-trip");
    }

    // ---- a title app's payload registers as a core title -------------------------
    {
        const fs::path title = scratch / "payload" / "title";
        write_sidecar(title / "core" / "n64lle_core.so", "0.375.0");
        write(title / "package" / "game_game.so", "pkg");
        write(title / "title.json", R"({"schema": 1, "id": "game", "name": "Game",
          "version": "0.0.1", "platform": "n64", "core": "core/n64lle_core.so",
          "package": "package/game_game.so", "title_dir": "package",
          "rom": {"sha256": "ABCDEF"}})");
        retcomm::CoreTitle ct;
        std::string err;
        check(retcomm::read_core_title(title / "title.json", ct, &err) && ct.id == "game" &&
                  ct.package.filename() == "game_game.so" && ct.content_sha256.size() == 1 &&
                  ct.content_sha256[0] == "abcdef" && ct.core_id == "n64lle" &&
                  ct.core_version == "0.375.0" &&
                  retcomm::title_from_core(ct).rom_identity.sha256.size() == 1,
              "a title.json registers as a core title");
    }

    // ---- issue #6: a portable setup's paths are the launcher folder's ------------
    {
        const fs::path L = scratch / "portable" / "Launcher";
        write(L / "retcomm-root.json", R"({"schema_version": 1, "root": "RetComM-Data"})");
        const fs::path cfg_path = L / "RetComM-Data" / "config" / "config.json";
        check(retcomm::config_launcher_dir(cfg_path) == L, "the launcher folder is found from the marker");
        // A file from before paths_relative_to: "roms" is the root's.
        write(cfg_path, R"({"library_root": "roms", "install_roots": [{"label": "Here", "path": "installed"}]})");
        retcomm::AppConfig c = retcomm::load_app_config(cfg_path);
        check(c.library_root == (L / "RetComM-Data" / "roms").lexically_normal(),
              "an unmarked file keeps resolving against the root");
        c.install_roots = {{"Here", retcomm::resolve_config_path(cfg_path, "./installed")},
                           {"NAS", "/mnt/nas/games"}};
        check(c.install_roots[0].path == (L / "installed").lexically_normal(),
              "a path typed relative now is the launcher folder's");
        check(retcomm::save_app_config(cfg_path, c), "portable config saves");
        std::string body;
        {
            std::ifstream in(cfg_path);
            body.assign(std::istreambuf_iterator<char>(in), {});
        }
        check(body.find("\"paths_relative_to\": \"launcher\"") != std::string::npos &&
                  body.find("\"RetComM-Data/roms\"") != std::string::npos &&
                  body.find("\"installed\"") != std::string::npos &&
                  body.find("/mnt/nas/games") != std::string::npos,
              "saved relative to the launcher, outside paths absolute");
        // The whole folder moves (another drive letter, another share).
        const fs::path L2 = scratch / "portable" / "Moved";
        std::error_code ec;
        fs::rename(L, L2, ec);
        const retcomm::AppConfig moved =
            retcomm::load_app_config(L2 / "RetComM-Data" / "config" / "config.json");
        check(moved.library_root == (L2 / "RetComM-Data" / "roms").lexically_normal() &&
                  moved.install_roots.size() == 2 &&
                  moved.install_roots[0].path == (L2 / "installed").lexically_normal() &&
                  moved.install_roots[1].path == fs::path("/mnt/nas/games"),
              "after the folder moves, its paths follow it");
        // Not portable: no marker, no launcher folder, the old base.
        const fs::path plain = scratch / "plain" / "config" / "config.json";
        check(retcomm::config_launcher_dir(plain).empty() &&
                  retcomm::config_paths_base(plain) == scratch / "plain",
              "an installed setup keeps the root as its base");
    }
    // ---- robust_rename --------------------------------------------------------------
    {
        std::error_code ec;
        write(scratch / "rr" / "a.txt", "a");
        check(retcomm::robust_rename(scratch / "rr" / "a.txt", scratch / "rr" / "b.txt", ec) &&
                  fs::is_regular_file(scratch / "rr" / "b.txt"),
              "robust_rename renames");
        check(!retcomm::robust_rename(scratch / "rr" / "missing", scratch / "rr" / "c", ec) &&
                  ec == std::errc::no_such_file_or_directory,
              "a missing source fails at once");
        const fs::path shm = fs::path("/dev/shm") / ("retro-rr-" + std::to_string(::getpid()));
        if (fs::is_directory("/dev/shm", ec)) {
            write(scratch / "rr" / "dir" / "f.txt", "f");
            check(retcomm::robust_rename(scratch / "rr" / "dir", shm, ec) &&
                      fs::is_regular_file(shm / "f.txt") && !fs::exists(scratch / "rr" / "dir"),
                  "across filesystems it copies and removes the source");
            fs::remove_all(shm, ec);
        }
    }

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("hub_update: ok\n");
    return 0;
}
