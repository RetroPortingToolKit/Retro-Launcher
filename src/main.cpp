#include "retcomm/app_state.hpp"
#include "retcomm/bios_index.hpp"
#include "retcomm/build.hpp"
#include "retcomm/cache_gc.hpp"
#include "retcomm/catalog.hpp"
#include "retcomm/catalog_sync.hpp"
#include "retcomm/config.hpp"
#include "retcomm/core_titles.hpp"
#include "retcomm/data_root.hpp"
#include "retcomm/data_root_migrate.hpp"
#include "retcomm/http.hpp"
#include "retcomm/install.hpp"
#include "retcomm/launch.hpp"
#include "retcomm/hash.hpp"
#include "retcomm/netplay_account.hpp"
#include "retcomm/netplay_client.hpp"
#include "retcomm/netplay_ws.hpp"
#include "retcomm/library_index.hpp"
#include "retcomm/paths.hpp"
#include "retcomm/release_tags.hpp"
#include "retcomm/romm.hpp"
#include "retcomm/romm_saves.hpp"
#include "retcomm/romscan.hpp"

#include <cstdio>
#if defined(_WIN32)
#include <io.h>
#define RETCOMM_ISATTY_STDIN() (_isatty(_fileno(stdin)) != 0)
#else
#include <unistd.h>
#define RETCOMM_ISATTY_STDIN() (isatty(fileno(stdin)) != 0)
#endif
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

void print_help(const char* argv0) {
    std::cout
        << "Retro Launcher — multi-title hub for recomp/decomp projects\n\n"
        << "Usage:\n"
        << "  " << argv0 << " [--catalog DIR] <command> [args]\n\n"
        << "Commands:\n"
        << "  list                         List catalog titles\n"
        << "  status                       Show paths, library, installs\n"
        << "  config                       Show / explain config.json\n"
        << "  scan [--full] [--rom-dir DIR ...]  Scan ROM library, match catalog, update index\n"
        << "  bios scan [--full] [--bios-dir DIR]  Scan BIOS tree, match titles that need BIOS\n"
        << "  bios list                    Show indexed title → BIOS bindings\n"
        << "  library [--check-updates]    Indexed title → ROM + install + BIOS status\n"
        << "  core add <sidecar.rcore.toml> [--name NAME]\n"
        << "                               Register a title that runs through an rcore core\n"
        << "                               (it then scans, lists and launches like any title)\n"
        << "  core list                    Registered core titles, their cores and ROMs\n"
        << "  install <title-id> [opts]    Build locally when catalog has build recipe;\n"
        << "                               otherwise download prebuilt GitHub release\n"
        << "      --force                  Reinstall / rebuild even if same pin\n"
        << "      --prebuilt               Force zip install (skip local generate+build)\n"
        << "      --wine                   Linux/macOS: install Windows build via Wine\n"
        << "      --dry-run                Print install plan only\n"
        << "  build <title-id> [opts]      Local generate + cmake (requires matched ROM)\n"
        << "      --force                  Rebuild even if same source ref\n"
        << "      --force-generate         Re-run disc→C even if codegen-cache matches\n"
        << "      --rom PATH               ROM path (else library preferred)\n"
        << "  pack ensure toolchain|sdk [opts]\n"
        << "                               Fetch/cache a release pack (smoke-test downloads)\n"
        << "      --title ID               Use that title's build.toolchain / build.sdk\n"
        << "      --force                  Re-download even if cached\n"
        << "  update <title-id>|--all [opts]\n"
        << "                               Update installed title(s) if newer\n"
        << "      --force                  Force update even if pin matches\n"
        << "      --force-generate         Re-run disc→C on build updates\n"
        << "  uninstall <title-id> [opts]  Remove installed title (alias: remove)\n"
        << "      --keep-saves             Keep memcards/SRAM/savestates (default)\n"
        << "      --delete-saves           Also wipe saves / preserved stash\n"
        << "      --dry-run                Show what would be removed\n"
        << "  orphans [list|remove] [opts] List/remove installs not in the catalog\n"
        << "      --keep-saves             Keep memcards/SRAM/savestates (default)\n"
        << "      --delete-saves           Also wipe saves / preserved stash\n"
        << "      --dry-run                Show what would be removed\n"
        << "      --no-prune               Don't prune stale index/state entries\n"
        << "  cache gc                     Prune old toolchains/SDKs/engines/zips/idle builds\n"
        << "                               and disc images duplicated into install folders\n"
        << "  netplay probe [opts]         Sign in with Discord, connect to the lobby server,\n"
        << "                               list rooms/players; --title ID scopes to a game\n"
        << "  netplay selftest             Digest vectors + libcurl WebSocket availability\n"
        << "  launch <title-id> [opts]     Launch title into its dedicated launcher\n"
        << "      --rom PATH               ROM/disc path (else library index)\n"
        << "      --bios PATH              BIOS path (else bios index)\n"
        << "      --mode default|direct|netplay\n"
        << "      --detach                 Don't wait for the game to exit\n"
        << "      --dry-run                Print argv/cwd only\n"
        << "  romm                         Show RomM config / stub ping\n"
        << "  catalog update [--force]     Download/update remote catalog cache\n"
        << "  root [show|set DIR|reset]    Show / move the Retro config+data folder\n"
        << "      --move|--use-existing|--fresh   How to treat data already present\n"
        << "      --yes                           Skip the confirmation prompt\n"
        << "  help                         This message\n\n"
        << "ROM scan uses config library_root (platform folders only).\n"
        << "BIOS scan uses config bios_root (flat + per-system folders).\n"
        << "Game saves use config saves_root/<platform>/<title_id>/ when set (else install saves/).\n"
        << "--full ignores the hash cache and rebuilds the index from disk\n"
        << "(drops missing files; recomputes digests for everything scanned).\n"
        << "Indexes live under the data dir (see `retcomm status`).\n"
        << "--root DIR / RETCOMM_HOME put config+data under DIR instead of the\n"
        << "OS default (portable installs, or a second drive).\n";
}

fs::path exe_dir_from(const char* argv0) {
    std::error_code ec;
    fs::path p = fs::absolute(argv0, ec);
    if (ec) p = fs::path(argv0);
    return p.parent_path();
}

retcomm::ScanProgressFn make_progress_printer() {
    return [](const retcomm::ScanProgress& p) {
        if (p.phase == "hash" && p.total > 0) {
            std::cerr << "\r  hashing [" << p.platform << "] " << p.current << "/"
                      << p.total << "  " << p.path.filename().string() << "          "
                      << std::flush;
            if (p.current == p.total) std::cerr << "\n";
        } else if (p.phase == "cache" && p.current > 0 && (p.current % 25 == 0)) {
            std::cerr << "\r  cache-hit [" << p.platform << "] " << p.current
                      << "          " << std::flush;
        } else if (p.phase == "walk" && p.current > 0 && (p.current % 50 == 0)) {
            std::cerr << "\r  indexing [" << p.platform << "] " << p.current
                      << " files…          " << std::flush;
        } else if (p.phase == "match") {
            std::cerr << "  matching catalog…\n";
        }
    };
}

int cmd_list(const retcomm::Catalog& cat) {
    std::cout << cat.name << " (" << cat.titles.size() << " titles)\n";
    for (const auto& t : cat.titles) {
        const char* id_mark = t.has_rom_identity() ? "*" : " ";
        std::cout << "  " << id_mark << " " << t.id << "\n"
                  << "      " << t.name << "  [" << t.kind << "/" << t.platform << "]\n";
    }
    std::cout << "\n* = rom_identity present (hash/serial matchable)\n";
    return 0;
}

int cmd_status(const retcomm::Paths& paths, const retcomm::AppConfig& cfg,
               const retcomm::Catalog& cat) {
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "note: could not create data dirs (" << e.what() << ")\n";
    }
    auto idx = retcomm::load_library_index(paths.library_index_path);
    auto bios_idx = retcomm::load_bios_index(paths.bios_index_path);
    std::cout << "root:    "
              << (retcomm::using_custom_root(paths)
                      ? paths.root.string() + " (" +
                            retcomm::data_root_source_label(paths.root_source) + ")"
                      : std::string("OS default"))
              << "\n"
              << "config:  " << paths.config_path.string() << "\n"
              << "data:    " << paths.data_dir.string() << "\n"
              << "apps:    " << paths.apps_dir.string() << "\n"
              << "index:   " << paths.library_index_path.string() << " ("
              << idx.titles.size() << " bound titles, " << idx.files.size()
              << " files)\n"
              << "bios:    " << paths.bios_index_path.string() << " ("
              << bios_idx.titles.size() << " bound titles, " << bios_idx.files.size()
              << " files)\n"
              << "catalog: " << cat.root.string() << "\n"
              << "catalog cache: " << paths.catalog_dir.string()
              << (retcomm::catalog_cache_valid(paths) ? " (valid)\n" : " (empty)\n")
              << "library: "
              << (cfg.library_root.empty() ? "(unset)" : cfg.library_root.string())
              << "\n"
              << "bios_root: "
              << (cfg.bios_root.empty() ? "(unset)" : cfg.bios_root.string()) << "\n"
              << "saves_root: "
              << (cfg.saves_root.empty() ? "(unset)" : cfg.saves_root.string()) << "\n\n";

    std::cout << "Platform folders (catalog → disk):\n";
    std::unordered_set<std::string> plats;
    for (const auto& t : cat.titles) plats.insert(t.platform);
    for (const auto& plat : plats) {
        auto folders = cfg.folders_for_platform(plat);
        std::cout << "  " << plat << " → ";
        for (size_t i = 0; i < folders.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << folders[i];
            if (!cfg.library_root.empty()) {
                const fs::path p = cfg.library_root / folders[i];
                std::cout << (fs::is_directory(p) ? " ✓" : " ✗");
            }
        }
        std::cout << "\n";
    }
    std::cout << "\n";

    for (const auto& t : cat.titles) {
        auto plan = retcomm::inspect_install_any(paths, cfg, t);
        const auto rom = idx.preferred_rom(t.id);
        std::cout << "  " << t.id << " — " << plan.message;
        if (!rom.empty()) std::cout << "\n      rom: " << rom.string();
        std::cout << "\n";
    }
    return 0;
}

int cmd_config(const retcomm::Paths& paths, const retcomm::AppConfig& cfg) {
    std::cout << "Config file: " << paths.config_path.string() << "\n"
              << "library_root: "
              << (cfg.library_root.empty() ? "(unset)" : cfg.library_root.string())
              << "\n"
              << "bios_root: "
              << (cfg.bios_root.empty() ? "(unset)" : cfg.bios_root.string()) << "\n"
              << "saves_root: "
              << (cfg.saves_root.empty() ? "(unset)" : cfg.saves_root.string()) << "\n"
              << "exclude_dirs: ";
    for (size_t i = 0; i < cfg.exclude_dirs.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << cfg.exclude_dirs[i];
    }
    std::cout << "\n"
              << "romm: "
              << (cfg.romm.enabled() ? cfg.romm.base_url : "(not configured)") << "\n"
              << "github_token: "
              << (cfg.github_token.empty() ? "(empty — unauthenticated API, easy to rate-limit)"
                                           : "(set)")
              << "\n"
              << "catalog url: "
              << (cfg.catalog.url.empty() ? retcomm::default_catalog_download_url()
                                          : cfg.catalog.url)
              << "\n"
              << "catalog auto_update: " << (cfg.catalog.auto_update ? "true" : "false") << "\n\n"
              << "Example config.json:\n"
              << "{\n"
              << "  \"library_root\": \"/mnt/crucial4tb/Emulation/roms\",\n"
              << "  \"bios_root\": \"/mnt/crucial4tb/Emulation/bios\",\n"
              << "  \"saves_root\": \"/mnt/crucial4tb/Emulation/saves\",\n"
              << "  \"platform_folders\": {\n"
              << "    \"psx\": [\"ps\", \"ps1\", \"psx\"],\n"
              << "    \"snes\": [\"snes\"],\n"
              << "    \"n64\": [\"n64\"]\n"
              << "  },\n"
              << "  \"exclude_dirs\": [\"torrents\", \"emulators\"],\n"
              << "  \"github_token\": \"ghp_…\",\n"
              << "  \"romm\": {\n"
              << "    \"base_url\": \"https://your-romm.example\",\n"
              << "    \"api_token\": \"…\"\n"
              << "  },\n"
              << "  \"catalog\": {\n"
              << "    \"url\": \"https://github.com/RetroPortingToolKit/Retro-Catalog/releases/latest/download/catalog.zip\",\n"
              << "    \"github_repo\": \"RetroPortingToolKit/Retro-Catalog\",\n"
              << "    \"auto_update\": true\n"
              << "  }\n"
              << "}\n";
    return 0;
}

int cmd_library(const retcomm::Paths& paths, const retcomm::AppConfig& cfg,
                const retcomm::Catalog& cat, bool check_updates) {
    auto idx = retcomm::load_library_index(paths.library_index_path);
    auto bios_idx = retcomm::load_bios_index(paths.bios_index_path);
    retcomm::ReleaseTagCache tag_cache(retcomm::release_tags_cache_path(paths));
    std::cout << "Library index: " << paths.library_index_path.string() << "\n"
              << "library_root: "
              << (idx.library_root.empty() ? "(unset)" : idx.library_root) << "\n"
              << "files: " << idx.files.size() << "  bound titles: " << idx.titles.size()
              << "\n\n";
    if (idx.titles.empty()) {
        std::cout << "No bound titles yet. Run: retcomm scan\n";
        return 0;
    }
    for (const auto& b : idx.titles) {
        const auto* t = cat.find(b.title_id);
        std::cout << "  " << b.title_id;
        if (t) std::cout << "  (" << t->name << ")";
        std::cout << "\n"
                  << "      preferred: " << b.preferred_path << "\n";
        if (b.paths.size() > 1) {
            for (size_t i = 1; i < b.paths.size(); ++i)
                std::cout << "      also:      " << b.paths[i] << "\n";
        }
        if (t) {
            auto plan = retcomm::inspect_install_any(paths, cfg, *t);
            if (plan.installed) {
                std::cout << "      app:       installed";
                if (!plan.installed_tag.empty())
                    std::cout << " " << plan.installed_tag;
                if (check_updates && !t->release.github.empty()) {
                    std::string err;
                    const std::string latest =
                        tag_cache.latest_tag(t->release.github, t->release.allow_prerelease,
                                             /*force=*/false, &err);
                    const std::string have = retcomm::install_release_compare_tag(plan);
                    if (!latest.empty() && !have.empty() && latest != have)
                        std::cout << "  [update available: " << latest << "]";
                    else if (!latest.empty())
                        std::cout << "  [up to date]";
                    else if (!err.empty())
                        std::cout << "  [update check failed]";
                }
                std::cout << "\n";
            } else {
                std::cout << "      app:       not installed\n";
            }
            if (t->requires_bios()) {
                const auto bios = bios_idx.preferred_bios(t->id);
                if (!bios.empty())
                    std::cout << "      bios:      " << bios.string() << "\n";
                else
                    std::cout << "      bios:      missing (retcomm bios scan)\n";
            }
        }
    }
    if (check_updates) {
        tag_cache.save_if_dirty();
        std::cout << "\nUpdate check: " << tag_cache.network_fetches() << " GitHub fetch(es), "
                  << tag_cache.cache_hits() << " cache hit(s)\n";
    }
    return 0;
}

int cmd_bios_list(const retcomm::Paths& paths, const retcomm::Catalog& cat) {
    auto idx = retcomm::load_bios_index(paths.bios_index_path);
    std::cout << "BIOS index: " << paths.bios_index_path.string() << "\n"
              << "bios_root: " << (idx.bios_root.empty() ? "(unset)" : idx.bios_root)
              << "\n"
              << "files: " << idx.files.size() << "  bound titles: " << idx.titles.size()
              << "\n\n";
    if (idx.titles.empty()) {
        std::cout << "No BIOS bindings yet. Run: retcomm bios scan\n";
        return 0;
    }
    for (const auto& b : idx.titles) {
        const auto* t = cat.find(b.title_id);
        std::cout << "  " << b.title_id;
        if (t) std::cout << "  (" << t->name << ")";
        std::cout << "\n"
                  << "      preferred: " << b.preferred_path << "\n";
        for (const auto& p : b.paths) {
            if (p == b.preferred_path) continue;
            std::cout << "      also:      " << p << "\n";
        }
    }
    return 0;
}

int cmd_bios_scan(const retcomm::Paths& paths, const retcomm::Catalog& cat,
                  const retcomm::AppConfig& cfg, const std::vector<fs::path>& bios_dirs,
                  bool full_rescan) {
    retcomm::BiosIndex index = retcomm::load_bios_index(paths.bios_index_path);

    retcomm::BiosScanOptions opts;
    opts.on_progress = [](const retcomm::BiosScanProgress& p) {
        if (p.phase == "hash" && p.total > 0) {
            std::cerr << "\r  hashing [" << p.platform << "] " << p.current << "/"
                      << p.total << "  " << p.path.filename().string() << "          "
                      << std::flush;
            if (p.current == p.total) std::cerr << "\n";
        } else if (p.phase == "match") {
            std::cerr << "  matching catalog…\n";
        }
    };
    opts.full_rescan = full_rescan;
    if (full_rescan) {
        if (bios_dirs.empty()) {
            std::cerr << "Full BIOS rescan: rebuilding index from scratch…\n";
            index = retcomm::BiosIndex{};
        } else {
            std::cerr << "Full BIOS rescan: ignoring hash cache…\n";
        }
        opts.index = nullptr;
    } else {
        opts.index = &index;
    }

    retcomm::BiosScanResult result;
    fs::path bios_root = cfg.bios_root;
    if (!bios_dirs.empty()) {
        std::cerr << "Scanning " << bios_dirs.size() << " BIOS path(s)…\n";
        result = retcomm::scan_bios_roots(cat, cfg, bios_dirs, opts);
        if (bios_root.empty()) bios_root = bios_dirs[0];
    } else {
        if (cfg.bios_root.empty()) {
            std::cerr << "bios scan requires bios_root in config.json or --bios-dir DIR\n"
                      << "  tip: retcomm config\n";
            return 2;
        }
        std::cerr << "Scanning BIOS tree " << cfg.bios_root.string() << "…\n";
        result = retcomm::scan_bios_library(cat, cfg, opts);
    }

    std::cout << "Scanned roots:\n";
    for (const auto& r : result.scanned_roots) std::cout << "  " << r.string() << "\n";
    std::cout << "Candidates: " << result.files.size()
              << "  hashed: " << result.hashed_files
              << "  cache-hits: " << result.cache_hits
              << "  size-skipped: " << result.skipped_hash << "\n";
    for (const auto& err : result.errors) std::cerr << "  warning: " << err << "\n";

    merge_bios_scan_into_index(index, cat, result, bios_root);
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "note: " << e.what() << "\n";
    }
    if (!retcomm::save_bios_index(paths.bios_index_path, index)) {
        std::cerr << "warning: failed to write " << paths.bios_index_path.string() << "\n";
    } else {
        std::cout << "BIOS index updated: " << paths.bios_index_path.string() << " ("
                  << index.titles.size() << " titles, " << index.files.size()
                  << " files)\n";
    }

    if (result.matches.empty()) {
        std::cout << "No BIOS matches for catalog titles.\n"
                  << "  Tip: PSX titles expect SCPH1001.BIN (CRC32 37157331, 512 KiB).\n";
        return 0;
    }
    std::cout << "\nBIOS bindings:\n";
    for (const auto& m : result.matches) {
        const auto* t = cat.find(m.title_id);
        std::cout << "  " << m.title_id;
        if (t) std::cout << " (" << t->name << ")";
        std::cout << "\n"
                  << "      " << m.preferred_path << "\n";
    }
    return 0;
}

int cmd_scan(const retcomm::Paths& paths, const retcomm::Catalog& cat,
             const retcomm::AppConfig& cfg, const std::vector<fs::path>& rom_dirs,
             bool full_rescan) {
    retcomm::LibraryIndex index = retcomm::load_library_index(paths.library_index_path);

    retcomm::ScanOptions opts;
    opts.on_progress = make_progress_printer();
    opts.full_rescan = full_rescan;
    if (full_rescan) {
        if (rom_dirs.empty()) {
            std::cerr << "Full ROM rescan: rebuilding library index from scratch…\n";
            index = retcomm::LibraryIndex{};
        } else {
            std::cerr << "Full ROM rescan: ignoring hash cache for scanned roots…\n";
        }
        opts.index = nullptr;
    } else {
        opts.index = &index;
    }

    retcomm::ScanResult result;
    fs::path lib_root = cfg.library_root;
    if (!rom_dirs.empty()) {
        std::cerr << "Scanning " << rom_dirs.size() << " path(s) (platform-scoped)…\n";
        result = retcomm::scan_rom_roots(cat, cfg, rom_dirs, opts);
        if (lib_root.empty() && !rom_dirs.empty()) {
            // If user passed a library root, remember it on the index.
            std::error_code ec;
            if (fs::is_directory(rom_dirs[0] / "snes", ec) ||
                fs::is_directory(rom_dirs[0] / "ps", ec))
                lib_root = rom_dirs[0];
        }
    } else {
        if (cfg.library_root.empty()) {
            std::cerr << "scan requires library_root in config.json or --rom-dir DIR\n"
                      << "  tip: retcomm config\n";
            return 2;
        }
        std::cerr << "Scanning library " << cfg.library_root.string()
                  << " (catalog platforms only)…\n";
        result = retcomm::scan_rom_library(cat, cfg, opts);
    }

    std::cout << "Scanned roots:\n";
    for (const auto& r : result.scanned_roots) std::cout << "  " << r.string() << "\n";
    std::cout << "Candidates: " << result.files.size()
              << "  hashed: " << result.hashed_files
              << "  cache-hits: " << result.cache_hits
              << "  hash-skipped: " << result.skipped_hash << "\n";
    for (const auto& err : result.errors) std::cerr << "  warning: " << err << "\n";

    merge_scan_into_index(index, cat, result, lib_root);
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "note: " << e.what() << "\n";
    }
    if (!retcomm::save_library_index(paths.library_index_path, index)) {
        std::cerr << "warning: failed to write " << paths.library_index_path.string()
                  << "\n";
    } else {
        std::cout << "Index updated: " << paths.library_index_path.string() << " ("
                  << index.titles.size() << " titles, " << index.files.size()
                  << " files)\n";
    }

    if (result.matches.empty()) {
        std::cout << "No catalog matches yet.\n"
                  << "  Tip: only titles with rom_identity hashes can match "
                     "(see metal-warriors-snes).\n";
        return 0;
    }
    std::cout << "\nRecommended / matched titles:\n";
    for (const auto& m : result.matches) {
        std::cout << "  " << m.title->name << " (" << m.title->id << ")\n"
                  << "      via " << m.matched_by << ": " << m.rom_path.string() << "\n";
        for (size_t i = 1; i < m.all_paths.size(); ++i)
            std::cout << "      also: " << m.all_paths[i].string() << "\n";
    }
    return 0;
}

int cmd_install(const retcomm::Paths& paths, const retcomm::Catalog& cat,
                const std::string& id, bool force, bool dry_run, bool use_wine,
                bool prefer_prebuilt) {
    const auto* t = cat.find(id);
    if (!t) {
        std::cerr << "unknown title: " << id << "\n";
        return 1;
    }
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "note: " << e.what() << "\n";
    }
    retcomm::InstallOptions opts;
    opts.force = force;
    opts.check_latest = true;
    opts.use_wine = use_wine;
    opts.prefer_prebuilt = prefer_prebuilt || use_wine;
    if (dry_run) {
        if (t->supports_prebuilt_install()) {
            auto plan = retcomm::plan_install(paths, *t, opts);
            std::cout << "would prefer prebuilt zip install\n" << plan.message;
            if (!opts.prefer_prebuilt && t->supports_local_build())
                std::cout << "(local build fallback available from " << t->build.source.ref
                          << ")\n";
            return 0;
        }
        if (!opts.prefer_prebuilt && t->supports_local_build()) {
            std::cout << "would build " << t->id << " from " << t->build.source.ref
                      << " (sdk=" << t->build.sdk.id << ", toolchain=" << t->build.toolchain.id
                      << ")\n";
            return 0;
        }
        auto plan = retcomm::plan_install(paths, *t, opts);
        std::cout << plan.message;
        return 0;
    }
    retcomm::BuildOptions bopts;
    bopts.force = force;
    {
        const auto st = retcomm::load_app_state(paths.state_path);
        const std::string choice = retcomm::preferred_bios_for(st, id);
        if (choice == retcomm::kOpenBiosChoice) {
            bopts.use_openbios = true;
        } else if (!choice.empty()) {
            bopts.bios_path = choice;
        } else {
            auto bidx = retcomm::load_bios_index(paths.bios_index_path);
            bopts.bios_path = bidx.preferred_bios(id);
            if (bopts.bios_path.empty() &&
                t->build.generate.engine == "psxrecomp") {
                bopts.use_openbios = true;
            }
        }
    }
    auto result = retcomm::install_title_auto(paths, *t, opts, bopts);
    std::cout << result.message;
    return result.ok ? 0 : 1;
}

int cmd_pack_ensure(const retcomm::Paths& paths, const retcomm::Catalog& cat,
                    const std::string& kind, const std::string& title_id, bool force) {
    const bool toolchain = (kind == "toolchain");
    if (!toolchain && kind != "sdk") {
        std::cerr << "usage: retcomm pack ensure toolchain|sdk [--title ID] [--force]\n";
        return 2;
    }

    retcomm::TitleBuildPack pack;
    if (!title_id.empty()) {
        const auto* t = cat.find(title_id);
        if (!t) {
            std::cerr << "unknown title: " << title_id << "\n";
            return 1;
        }
        pack = toolchain ? t->build.toolchain : t->build.sdk;
        if (pack.id.empty() || pack.github.empty()) {
            std::cerr << "title " << title_id << " has no build." << kind << " pack\n";
            return 1;
        }
    } else if (toolchain) {
        // Default: shared Retro toolchain repo.
        pack.id = "cmake-clang-v1";
        pack.github = "RetroPortingToolKit/RetroPorting-Toolchains";
        pack.asset_glob_linux = "*cmake-clang-v1*linux*";
        pack.asset_glob_windows = "*cmake-clang-v1*windows*";
        pack.asset_glob_macos = "*cmake-clang-v1*macos*";
    } else {
        std::cerr << "sdk pack ensure requires --title <id>\n";
        return 2;
    }

    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "note: " << e.what() << "\n";
    }

    if (force) {
        std::error_code ec;
        const fs::path base = toolchain ? paths.toolchains_dir : paths.sdks_dir;
        fs::remove_all(base / pack.id, ec);
    }

    auto r = retcomm::ensure_pack(paths, pack, toolchain, {}, {});
    if (!r.ok) {
        std::cerr << r.message << "\n";
        return 1;
    }
    std::cout << r.message << "\n";
    std::cout << "root: " << r.root.string() << "\n";
    std::cout << "tag:  " << r.tag << "\n";

    // Light verification for toolchain packs.
    if (toolchain) {
        std::error_code ec;
        const fs::path bin = r.root / "bin";
        const fs::path cmake =
#if defined(_WIN32)
            bin / "cmake.exe";
#else
            bin / "cmake";
#endif
        if (fs::is_regular_file(cmake, ec)) {
            std::cout << "cmake: " << cmake.string() << "\n";
        } else {
            std::cerr << "warning: cmake not found under " << bin.string() << "\n";
        }
        const fs::path meta = r.root / "retcomm-toolchain.json";
        if (fs::is_regular_file(meta, ec)) std::cout << "meta:  " << meta.string() << "\n";
    }
    return 0;
}

int cmd_build(const retcomm::Paths& paths, const retcomm::Catalog& cat, const std::string& id,
              bool force, bool force_generate, const fs::path& rom_override) {
    const auto* t = cat.find(id);
    if (!t) {
        std::cerr << "unknown title: " << id << "\n";
        return 1;
    }
    if (!t->supports_local_build()) {
        std::cerr << "title has no local build recipe: " << id << "\n";
        return 1;
    }
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "note: " << e.what() << "\n";
    }
    retcomm::BuildOptions bopts;
    bopts.force = force;
    bopts.force_generate = force_generate;
    bopts.rom_path = rom_override;
    if (bopts.rom_path.empty()) {
        const auto idx = retcomm::load_library_index(paths.library_index_path);
        bopts.rom_path = retcomm::boot_disc_rom(idx, *t);
    }
    {
        const auto st = retcomm::load_app_state(paths.state_path);
        const std::string choice = retcomm::preferred_bios_for(st, id);
        if (choice == retcomm::kOpenBiosChoice) {
            bopts.use_openbios = true;
        } else if (!choice.empty()) {
            bopts.bios_path = choice;
        } else {
            auto bidx = retcomm::load_bios_index(paths.bios_index_path);
            bopts.bios_path = bidx.preferred_bios(id);
            if (bopts.bios_path.empty() && t->build.generate.engine == "psxrecomp")
                bopts.use_openbios = true;
        }
    }
    auto result = retcomm::build_title(paths, *t, bopts);
    std::cout << result.message;
    return result.ok ? 0 : 1;
}

int cmd_update(const retcomm::Paths& paths, const retcomm::AppConfig& cfg,
               const retcomm::Catalog& cat, const std::string& id_or_all, bool force,
               bool force_generate) {
    retcomm::InstallOptions opts;
    opts.force = force;
    opts.check_latest = true;

    std::vector<const retcomm::Title*> targets;
    if (id_or_all == "--all") {
        for (const auto& t : cat.titles) {
            auto plan = retcomm::inspect_install_any(paths, cfg, t);
            if (plan.installed) targets.push_back(&t);
        }
        if (targets.empty()) {
            std::cout << "No installed titles to update.\n";
            return 0;
        }
    } else {
        const auto* t = cat.find(id_or_all);
        if (!t) {
            std::cerr << "unknown title: " << id_or_all << "\n";
            return 1;
        }
        targets.push_back(t);
    }

    int failures = 0;
    for (const auto* t : targets) {
        retcomm::BuildOptions bopts;
        bopts.force = force;
        bopts.force_generate = force_generate;
        auto result = retcomm::update_title_auto(paths, *t, opts, bopts);
        std::cout << result.message;
        if (!result.ok) ++failures;
    }
    return failures ? 1 : 0;
}

int cmd_uninstall(const retcomm::Paths& paths, const retcomm::Catalog& cat,
                  const std::string& id, const retcomm::UninstallOptions& opts) {
    const auto* t = cat.find(id);
    if (!t) {
        std::cerr << "unknown title: " << id << "\n";
        return 1;
    }
    auto result = retcomm::uninstall_title(paths, *t, opts);
    std::cout << result.message;
    return result.ok ? 0 : 1;
}

int cmd_launch(const retcomm::Paths& paths, const retcomm::Catalog& cat,
               const std::string& id, retcomm::LaunchOptions opts) {
    const auto* t = cat.find(id);
    if (!t) {
        std::cerr << "unknown title: " << id << "\n";
        return 1;
    }
    // A title with a core plays inside the hub's window, not as its own
    // process: hand off to `retro-hub --play`, which exits when the game
    // closes -- so a caller that waits on this command (a Steam shortcut)
    // still waits for the game.
    {
        const auto cfg0 = retcomm::load_app_config(paths.config_path);
        const auto plan = retcomm::inspect_install_any(paths, cfg0, *t);
        if (!retcomm::core_manifest_for(*t, plan.install_root).empty()) {
#if defined(__linux__)
            std::error_code ec;
            const fs::path hub = fs::read_symlink("/proc/self/exe", ec).parent_path() / "retro-hub";
            std::cout << id << " runs through a core: starting " << hub.string() << " --play " << id
                      << "\n";
            std::cout.flush();
            const std::string hub_s = hub.string();
            char* argv[] = {const_cast<char*>(hub_s.c_str()), const_cast<char*>("--play"),
                            const_cast<char*>(id.c_str()), nullptr};
            ::execv(hub_s.c_str(), argv);
            std::cerr << "cannot start " << hub_s << ": " << std::strerror(errno) << "\n";
            return 1;
#else
            std::cerr << id << " runs through a core, which this platform cannot host yet\n";
            return 1;
#endif
        }
    }
    const auto cfg = retcomm::load_app_config(paths.config_path);
    std::string rom_source;
    std::string bios_source;
    std::string save_source;
    if (opts.rom_path.empty()) {
        auto idx = retcomm::load_library_index(paths.library_index_path);
        opts.rom_path = retcomm::boot_disc_rom(idx, *t);
        if (!opts.rom_path.empty()) rom_source = "library-index";
    } else {
        rom_source = "--rom";
    }
    if (opts.bios_path.empty()) {
        const auto st = retcomm::load_app_state(paths.state_path);
        const std::string choice = retcomm::preferred_bios_for(st, id);
        if (choice == retcomm::kOpenBiosChoice) {
            opts.use_openbios = true;
            opts.bios_path.clear();
            bios_source = "openbios";
        } else if (!choice.empty()) {
            opts.use_openbios = false;
            opts.bios_path = choice;
            bios_source = "state";
        } else if (t->has_bios_identity()) {
            auto bidx = retcomm::load_bios_index(paths.bios_index_path);
            opts.bios_path = bidx.preferred_bios(id);
            if (!opts.bios_path.empty()) bios_source = "bios-index";
        }
    } else {
        opts.use_openbios = false;
        bios_source = "--bios";
    }
    if (opts.save_path.empty()) {
        auto ensured =
            retcomm::ensure_canonical_save(paths, cfg, *t, opts.rom_path, /*mint=*/true);
        if (ensured.ok) {
            opts.save_path = ensured.save.host_path;
            save_source = ensured.created ? "created" : "canonical";
        }
        auto st = retcomm::load_app_state(paths.state_path);
        if (retcomm::title_uses_memcards(*t)) {
            const std::string card2_id = retcomm::preferred_save_card2_for(st, id);
            if (card2_id == retcomm::kBlankMemcardId) {
                opts.save_path_card2_blank = true;
            } else if (!card2_id.empty()) {
                opts.save_path_card2 = retcomm::resolve_managed_save(paths, cfg, *t, card2_id);
                // Unresolvable here: hand the raw id to bind so it reports the
                // fallback instead of looking like "no card 2 was ever chosen".
                if (opts.save_path_card2.empty()) opts.save_path_card2 = card2_id;
            }
        }
    } else {
        save_source = "--save";
    }

    auto result = retcomm::launch_title(paths, *t, opts);
    std::cout << result.message;
    if (!opts.rom_path.empty() && result.plan.ready)
        std::cout << "  media source: " << rom_source << "\n";
    else if (opts.rom_path.empty() && result.plan.ready)
        std::cout << "  tip: run retcomm scan, or pass --rom PATH\n";
    if (opts.use_openbios && result.plan.ready)
        std::cout << "  bios source:  " << bios_source << "\n";
    else if (!opts.bios_path.empty() && result.plan.ready)
        std::cout << "  bios source:  " << bios_source << "\n";
    else if (t->requires_bios() && opts.bios_path.empty() && result.plan.ready)
        std::cout << "  tip: run retcomm bios scan, or pass --bios PATH\n";
    if (!opts.save_path.empty() && result.plan.ready)
        std::cout << "  save source:  " << save_source << "\n";

    if (!result.ok) return 1;
    if (opts.dry_run || opts.detach) return 0;
    return result.exit_code;
}

int cmd_catalog_update(const retcomm::Paths& paths, const retcomm::AppConfig& cfg,
                       bool force) {
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "data dir error: " << e.what() << "\n";
        return 1;
    }
    const auto result = retcomm::sync_remote_catalog(paths, cfg, force);
    if (result.ok) {
        std::cout << result.message << "\n";
        if (!result.synced_at.empty()) std::cout << "  synced_at: " << result.synced_at << "\n";
        try {
            const auto cat = retcomm::load_catalog(paths.catalog_dir);
            // Free rebind: catalog titles added since the last scan often match
            // a ROM already hashed in the index. Costs no disk I/O. Files the
            // index never hashed still need `retcomm scan`.
            if (!result.skipped && cfg.auto_scan_after_catalog_update) {
                auto idx = retcomm::load_library_index(paths.library_index_path);
                const auto bound = retcomm::rematch_library_titles(idx, cat);
                if (bound) {
                    retcomm::save_library_index(paths.library_index_path, idx);
                    std::cout << "  matched " << bound
                              << " file(s) to new titles from cached hashes\n";
                }
                std::vector<std::string> plats;
                const auto missing =
                    retcomm::catalog_titles_without_rom(idx, cat, &plats);
                if (missing) {
                    std::cout << "  " << missing << " title(s) still unmatched";
                    if (!plats.empty()) {
                        std::cout << " — run: retcomm scan";
                        for (const auto& pl : plats) std::cout << " --platform " << pl;
                    }
                    std::cout << "\n";
                }
            }
            const auto orphans = retcomm::list_orphan_installs(paths, cat);
            if (!orphans.empty()) {
                std::cout << "  unlisted installs: " << orphans.size()
                          << " (run: retcomm orphans list / remove)\n";
            }
        } catch (...) {
        }
        return 0;
    }
    std::cerr << result.message << "\n";
    return 1;
}

// ---- netplay probe (phase 0 spike: transport + sign-in + lobby listing) ----
int cmd_netplay(const retcomm::Paths& paths, const retcomm::AppConfig& cfg,
                const retcomm::Catalog& cat, const std::vector<std::string>& args) {
    using namespace retcomm::netplay;
    using json = nlohmann::json;
    auto usage = [] {
        std::cerr
            << "usage: retcomm netplay probe [--url WS_URL] [--title CATALOG_ID | --game NAME [--version V]]\n"
               "                             [--login [--no-browser] [--login-timeout S]] [--logout] [--guest]\n"
               "                             [--name N] [--create ROOM] [--join LOBBY_ID] [--chat TEXT]\n"
               "                             [--watch SECONDS] [--verbose]\n"
               "       retcomm netplay selftest\n";
    };
    if (args.size() < 2) {
        usage();
        return 2;
    }
    const std::string sub = args[1];
    if (sub == "selftest") {
        std::cout << "sha256(\"abc\")\n  got    " << retcomm::sha256_hex("abc")
                  << "\n  expect ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
                  << "hmac_sha256(\"key\", \"The quick brown fox jumps over the lazy dog\")\n  got    "
                  << retcomm::hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog")
                  << "\n  expect f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8\n"
                  << "libcurl websocket: " << (WsConnection::supported() ? "yes" : "no") << "\n";
        return 0;
    }
    if (sub != "probe") {
        usage();
        return 2;
    }

    std::string url = cfg.netplay.lobby_url;
    std::string title_id, game, version, name, create_room, join_id, chat_text;
    bool do_login = false, no_browser = false, do_logout = false, guest = false, verbose = false;
    int login_timeout = 300, watch = 0;
    for (size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&](std::string& dst) {
            if (i + 1 >= args.size()) {
                std::cerr << a << " needs a value\n";
                return false;
            }
            dst = args[++i];
            return true;
        };
        std::string v;
        if (a == "--url") {
            if (!next(url)) return 2;
        } else if (a == "--title") {
            if (!next(title_id)) return 2;
        } else if (a == "--game") {
            if (!next(game)) return 2;
        } else if (a == "--version") {
            if (!next(version)) return 2;
        } else if (a == "--name") {
            if (!next(name)) return 2;
        } else if (a == "--create") {
            if (!next(create_room)) return 2;
        } else if (a == "--join") {
            if (!next(join_id)) return 2;
        } else if (a == "--chat") {
            if (!next(chat_text)) return 2;
        } else if (a == "--watch") {
            if (!next(v)) return 2;
            watch = std::atoi(v.c_str());
        } else if (a == "--login-timeout") {
            if (!next(v)) return 2;
            login_timeout = std::atoi(v.c_str());
        } else if (a == "--login") {
            do_login = true;
        } else if (a == "--no-browser") {
            no_browser = true;
        } else if (a == "--logout") {
            do_logout = true;
        } else if (a == "--guest") {
            guest = true;
        } else if (a == "--verbose") {
            verbose = true;
        } else {
            std::cerr << "unexpected netplay arg: " << a << "\n";
            usage();
            return 2;
        }
    }
    if (url.empty()) url = retcomm::kDefaultNetplayLobbyUrl;

    if (!title_id.empty()) {
        const retcomm::Title* t = cat.find(title_id);
        if (!t) {
            std::cerr << "unknown title id: " << title_id << "\n";
            return 2;
        }
        if (!t->supports_netplay()) {
            std::cerr << title_id << " has no usable netplay block in the catalog\n";
            return 2;
        }
        game = t->netplay.game_name;
        // Installs can live under any configured install root, not only the
        // default apps dir; the hub uses the same lookup.
        const auto plan = retcomm::inspect_install_any(paths, cfg, *t);
        const std::string installed = plan.record ? plan.record->source_ref : std::string();
        // The wire pin is what the installed binary was built from; the catalog
        // value is only the fallback for a title that is not installed here.
        version = retcomm::normalize_netplay_version(installed.empty() ? t->netplay.game_version
                                                                       : installed);
        if (!t->netplay.lobby_url.empty()) url = cfg.resolve_netplay_lobby_url(t->netplay.lobby_url);
        std::cout << "title " << t->id << " -> game_name \"" << game << "\" version " << version
                  << (plan.installed ? " (installed build)" : " (not installed; catalog version)")
                  << "\n";
    } else if (!version.empty()) {
        version = retcomm::normalize_netplay_version(version);
    }

    DiscordAccount acct(DiscordAccount::base_from_ws_url(url),
                        paths.data_dir / "netplay" / "account.key");
    if (do_logout) {
        std::string err;
        const bool ok = acct.sign_out(&err);
        std::cout << (ok ? "signed out; device key revoked and removed\n"
                         : "device key removed locally; server revoke failed: " + err + "\n");
        return ok ? 0 : 1;
    }
    AccountInfo info;
    std::string session;
    if (!guest) {
        std::string err;
        bool rejected = false;
        if (acct.has_stored_key()) {
            if (acct.restore(&info, &err, &rejected)) {
                session = info.session;
                std::cout << "signed in as " << info.handle << " (@" << info.discord_username
                          << ") via stored device key\n";
            } else if (rejected) {
                std::cout << "stored device key was rejected by the server and removed; "
                             "sign in again with --login\n";
            } else {
                std::cout << "could not restore the session (" << err << "); key kept\n";
            }
        }
        if (session.empty() && do_login) {
            std::cout << "starting Discord sign-in (timeout " << login_timeout << "s)…\n";
            const bool ok = acct.login(
                &info, &err, login_timeout, !no_browser,
                [](const std::string& u) { std::cout << "authorise at: " << u << "\n"; });
            if (ok) {
                session = info.session;
                std::cout << "signed in as " << info.handle << " (@" << info.discord_username
                          << "); device key stored\n";
            } else {
                std::cout << "sign-in failed: " << err << "\n";
                if (acct.unavailable()) return 1;
            }
        }
        if (session.empty()) std::cout << "not signed in; connecting as a guest (use --login)\n";
    }

    LobbyConfig lc;
    lc.url = url;
    lc.session_token = session;
    lc.display_name = !name.empty() ? name
                      : !cfg.netplay.display_name.empty() ? cfg.netplay.display_name
                                                          : std::string("retcomm-probe");
    lc.game_name = game;
    lc.game_version = version;
    std::cout << "connecting " << url;
    if (!game.empty()) std::cout << "  scope \"" << game << "\" v" << version;
    std::cout << "\n";

    LobbyClient client;
    client.start(lc);
    auto wait_for = [&](const std::function<bool(const Snapshot&)>& pred, int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            if (pred(client.snapshot())) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return pred(client.snapshot());
    };
    auto print_rooms = [&](const Snapshot& s) {
        std::cout << "rooms: " << s.rooms.size() << " shown";
        if (s.rooms_total != s.rooms.size()) std::cout << " of " << s.rooms_total << " on the server";
        std::cout << "\n";
        for (const LobbyRow& r : s.rooms) {
            std::cout << "  " << r.lobby_id << "  \"" << r.name << "\"  " << r.game_name << " v"
                      << r.game_version << "  " << r.player_count << "/" << r.max_slots
                      << (r.has_password ? "  [locked]" : "")
                      << (r.allow_spectators ? "  spectators " + std::to_string(r.spectator_count) +
                                                   "/" + std::to_string(r.max_spectators)
                                             : std::string())
                      << (r.host_country.empty() ? "" : "  " + r.host_country) << "\n";
        }
        std::cout << "players online: " << s.players.size() << "\n";
        for (const OnlinePlayer& p : s.players) {
            std::cout << "  " << p.display_name << (p.is_local ? " (you)" : "")
                      << (p.country.empty() ? "" : " [" + p.country + "]") << "  "
                      << (p.hosting ? "Hosting" : !p.lobby_id.empty() ? "In lobby" : "Browsing")
                      << (p.lobby_name.empty() ? "" : " \"" + p.lobby_name + "\"")
                      << (p.game_name.empty() ? "" : "  " + p.game_name) << "\n";
        }
    };
    auto print_room = [&](const Snapshot& s) {
        std::cout << "room \"" << s.room_name << "\" " << s.lobby_id << "  session " << s.session_id
                  << "  " << s.player_count << "/" << s.max_slots
                  << (s.is_host ? "  (you host)" : "") << (s.all_ready ? "  all ready" : "") << "\n";
        for (const Member& m : s.members) {
            std::cout << "  slot " << m.slot << "  " << m.display_name
                      << (m.is_local ? " (you)" : "") << (m.is_host ? "  Host" : "")
                      << (m.is_spectator ? "  Watching" : m.ready ? "  Ready" : "  Waiting")
                      << "\n";
        }
    };

    if (!wait_for([](const Snapshot& s) { return s.state != ConnState::Connecting; }, 20000)) {
        std::cerr << "connect timed out\n";
        client.stop();
        return 1;
    }
    Snapshot s = client.snapshot();
    if (s.state != ConnState::Connected) {
        std::cerr << "connect failed: " << s.transport_error << "\n";
        client.stop();
        return 1;
    }
    wait_for([](const Snapshot& s) { return s.list_seq > 0 && !s.display_name.empty(); }, 8000);
    s = client.snapshot();
    std::cout << "connected: player_id " << s.player_id << "  name \"" << s.display_name << "\""
              << (s.signed_in ? "  [signed in]" : "  [guest]") << "\n";
    if (!s.last_error_code.empty())
        std::cout << "server error: " << s.last_error_code << " " << s.last_error_detail << "\n";
    print_rooms(s);

    std::uint64_t seen_err = s.error_seq;
    if (!create_room.empty()) {
        client.create(create_room, "", 2, json());
        wait_for([&](const Snapshot& x) { return x.in_room || x.error_seq != seen_err; }, 8000);
        s = client.snapshot();
        if (s.in_room) {
            std::cout << "created ";
            print_room(s);
        } else {
            std::cout << "create failed: " << s.last_error_code << " " << s.last_error_detail << "\n";
        }
        seen_err = s.error_seq;
    }
    if (!join_id.empty()) {
        client.join(join_id, "");
        wait_for([&](const Snapshot& x) { return x.in_room || x.error_seq != seen_err; }, 8000);
        s = client.snapshot();
        if (s.in_room) {
            std::cout << "joined ";
            print_room(s);
        } else {
            std::cout << "join failed: " << s.last_error_code << " " << s.last_error_detail << "\n";
        }
        seen_err = s.error_seq;
    }
    if (!chat_text.empty()) {
        if (s.in_room) client.chat(chat_text);
        else client.server_chat(chat_text);
    }

    if (watch > 0) {
        std::cout << "watching for " << watch << "s…\n";
        std::uint64_t last_list = s.list_seq, last_chat = 0, last_err = seen_err;
        std::string last_members;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(watch);
        while (std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            s = client.snapshot();
            if (s.state != ConnState::Connected) {
                std::cout << "disconnected: " << s.transport_error << "\n";
                break;
            }
            if (s.list_seq != last_list) {
                last_list = s.list_seq;
                std::cout << "[list] " << s.rooms.size() << " room(s), " << s.players.size()
                          << " player(s)";
                for (const LobbyRow& r : s.rooms)
                    std::cout << "  \"" << r.name << "\" " << r.player_count << "/" << r.max_slots;
                std::cout << "\n";
            }
            for (const auto* ring : {&s.server_chat, &s.room_chat}) {
                for (const ChatLine& l : *ring) {
                    if (l.seq <= last_chat) continue;
                    last_chat = l.seq;
                    std::cout << (ring == &s.room_chat ? "[room] " : "[server] ")
                              << (l.is_system ? "* " : l.from + ": ") << l.text << "\n";
                }
            }
            if (s.error_seq != last_err) {
                last_err = s.error_seq;
                std::cout << "[error] " << s.last_error_code << " " << s.last_error_detail << "\n";
            }
            std::string sig;
            for (const Member& m : s.members)
                sig += std::to_string(m.slot) + ":" + m.display_name + (m.ready ? "+" : "-") + ";";
            if (s.in_room && sig != last_members) {
                last_members = sig;
                print_room(s);
            }
            if (!s.in_room && !last_members.empty()) {
                last_members.clear();
                std::cout << "[room] " << s.room_status << "\n";
            }
            if (s.launch_pending) {
                std::cout << "[launch] " << s.launch.dump() << "\n";
                break;
            }
        }
    }
    if (verbose) {
        s = client.snapshot();
        std::cout << "--- trace (" << s.trace.size() << " op(s))\n";
        for (const std::string& t : s.trace) std::cout << t.substr(0, 400) << "\n";
    }
    s = client.snapshot();
    if (s.in_room) {
        if (s.is_host) client.close_room();
        else client.leave();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    client.stop();
    return 0;
}

int cmd_orphans(const retcomm::Paths& paths, const retcomm::Catalog& cat, bool do_remove,
                const retcomm::OrphanCleanupOptions& opts) {
    try {
        retcomm::ensure_dirs(paths);
    } catch (const std::exception& e) {
        std::cerr << "data dir error: " << e.what() << "\n";
        return 1;
    }
    const auto orphans = retcomm::list_orphan_installs(paths, cat);
    if (!do_remove) {
        if (orphans.empty()) {
            std::cout << "No installs outside the catalog.\n";
            return 0;
        }
        std::cout << orphans.size() << " install(s) not in catalog:\n";
        for (const auto& o : orphans) {
            std::cout << "  " << o.title_id;
            if (o.title_id != o.dir_name) std::cout << " (" << o.dir_name << ")";
            if (!o.tag.empty()) std::cout << " @" << o.tag;
            if (o.has_preserved_only) std::cout << " [preserved]";
            std::cout << "\n    " << o.install_root.string() << "\n";
        }
        return 0;
    }
    if (orphans.empty() && !opts.prune_indexes) {
        std::cout << "Nothing to remove.\n";
        return 0;
    }
    auto result = retcomm::cleanup_removed_catalog_titles(paths, cat, opts);
    for (const auto& m : result.messages) std::cout << m;
    std::cout << result.message;
    return result.ok ? 0 : 1;
}

int cmd_romm(const retcomm::Paths& paths, const retcomm::AppConfig& cfg) {
    std::cout << "config: " << paths.config_path.string() << "\n";
    if (!cfg.romm.enabled()) {
        std::cout << "RomM: not configured\n\n"
                  << "See: retcomm config\n";
        return 0;
    }
    std::cout << "RomM base_url: " << cfg.romm.base_url << "\n"
              << "api_token:     " << (cfg.romm.api_token.empty() ? "(empty)" : "(set)")
              << "\n";
    retcomm::RommClient client(cfg.romm);
    if (!client.ping()) std::cout << client.last_error() << "\n";
    return 0;
}


std::string human_bytes(std::uintmax_t n) {
    const char* unit[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(n);
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        ++i;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), (i == 0 ? "%.0f %s" : "%.1f %s"), v, unit[i]);
    return buf;
}

void print_root_status(const retcomm::Paths& paths) {
    std::cout << "root:   "
              << (retcomm::using_custom_root(paths) ? paths.root.string()
                                                    : std::string("(OS default)"))
              << "\n"
              << "source: " << retcomm::data_root_source_label(paths.root_source) << "\n"
              << "config: " << paths.config_dir.string() << "\n"
              << "data:   " << paths.data_dir.string() << "\n";
}

// A root change relocates the whole data tree, so make the destructive form ask
// first when a human is driving. --yes (or a pipe) skips the prompt.
bool confirm_root_change(const retcomm::RootMigrationPlan& plan, retcomm::RootMigrationMode mode,
                         bool assume_yes) {
    if (assume_yes) return true;
    if (!RETCOMM_ISATTY_STDIN()) {
        std::cerr << "refusing to change the Retro folder non-interactively; pass --yes\n";
        return false;
    }
    if (mode == retcomm::RootMigrationMode::Move && plan.existing_bytes > 0)
        std::cout << "This MOVES " << human_bytes(plan.existing_bytes) << " from "
                  << plan.from_data.string() << "\n";
    std::cout << "Continue? [y/N] " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    return line == "y" || line == "Y" || line == "yes";
}

int cmd_root(const retcomm::Paths& paths, const std::vector<std::string>& args,
             const fs::path& exe_dir) {
    bool assume_yes = false;
    for (const auto& a : args) {
        if (a == "--yes" || a == "-y") assume_yes = true;
    }
    if (args.size() < 2 || args[1] == "show") {
        print_root_status(paths);
        return 0;
    }

    const bool reset = args[1] == "reset";
    if (!reset && args[1] != "set") {
        std::cerr << "usage: retcomm root [show | set <dir> [--move|--use-existing|--fresh] "
                     "| reset]\n";
        return 2;
    }

    if (reset) {
        if (!retcomm::using_custom_root(paths)) {
            std::cout << "Already using the OS default location.\n";
            return 0;
        }
        auto reset_mode = retcomm::RootMigrationMode::Move;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--move") reset_mode = retcomm::RootMigrationMode::Move;
            else if (args[i] == "--use-existing") reset_mode = retcomm::RootMigrationMode::UseExisting;
            else if (args[i] == "--fresh") reset_mode = retcomm::RootMigrationMode::StartFresh;
            else if (args[i] == "--yes" || args[i] == "-y") continue;
            else {
                std::cerr << "unexpected root reset arg: " << args[i] << "\n";
                return 2;
            }
        }
        const retcomm::RootMigrationPlan rplan = retcomm::plan_root_migration(paths, {});
        if (!rplan.blocker.empty()) {
            std::cerr << "cannot reset: " << rplan.blocker << "\n";
            return 1;
        }
        std::cout << "from: " << rplan.from_data.string() << " ("
                  << human_bytes(rplan.existing_bytes) << ")\n"
                  << "to:   " << rplan.to_data.string() << "  (OS default)\n";
        if (!confirm_root_change(rplan, reset_mode, assume_yes)) {
            std::cout << "Cancelled.\n";
            return 1;
        }
        const retcomm::RootMigrationResult res = retcomm::migrate_data_root(
            paths, {}, reset_mode, exe_dir, /*prefer_exe_marker=*/false,
            [](const std::string& m) { std::cout << "  " << m << "\n"; });
        for (const auto& n : res.notes) std::cout << "note: " << n << "\n";
        if (!res.ok) {
            std::cerr << res.message << "\n";
            return 1;
        }
        std::cout << res.message << "\n";
        return 0;
    }

    if (args.size() < 3) {
        std::cerr << "usage: retcomm root set <dir> [--move|--use-existing|--fresh]\n";
        return 2;
    }
    const fs::path target = fs::absolute(args[2]).lexically_normal();
    auto mode = retcomm::RootMigrationMode::Move;
    bool mode_given = false;
    for (size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--move") { mode = retcomm::RootMigrationMode::Move; mode_given = true; }
        else if (args[i] == "--use-existing") { mode = retcomm::RootMigrationMode::UseExisting; mode_given = true; }
        else if (args[i] == "--fresh") { mode = retcomm::RootMigrationMode::StartFresh; mode_given = true; }
        else if (args[i] == "--yes" || args[i] == "-y") { continue; }
        else {
            std::cerr << "unexpected root set arg: " << args[i] << "\n";
            return 2;
        }
    }

    const retcomm::RootMigrationPlan plan = retcomm::plan_root_migration(paths, target);
    if (!plan.blocker.empty()) {
        std::cerr << "cannot use " << target.string() << ": " << plan.blocker << "\n";
        return 1;
    }
    std::cout << "from: " << plan.from_data.string() << " (" << human_bytes(plan.existing_bytes)
              << ")\n"
              << "to:   " << plan.to_root.string() << "\n";
    if (!plan.warning.empty()) std::cout << "note: " << plan.warning << "\n";
    if (!mode_given && plan.existing_bytes > 0)
        std::cout << "mode: --move (default; pass --use-existing or --fresh to skip moving)\n";
    if (!confirm_root_change(plan, mode, assume_yes)) {
        std::cout << "Cancelled.\n";
        return 1;
    }

    const retcomm::RootMigrationResult res = retcomm::migrate_data_root(
        paths, target, mode, exe_dir, /*prefer_exe_marker=*/false,
        [](const std::string& m) { std::cout << "  " << m << "\n"; });
    for (const auto& n : res.notes) std::cout << "note: " << n << "\n";
    if (!res.ok) {
        std::cerr << res.message << "\n";
        return 1;
    }
    std::cout << res.message << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    fs::path catalog_override;
    fs::path root_override;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--catalog" && i + 1 < argc) {
            catalog_override = argv[++i];
        } else if (a == "--root" && i + 1 < argc) {
            root_override = argv[++i];
        } else if (a == "-h" || a == "--help") {
            print_help(argv[0]);
            return 0;
        } else {
            args.push_back(a);
        }
    }

    if (args.empty() || args[0] == "help") {
        print_help(argv[0]);
        return 0;
    }

    // --root wins over the marker/env chain so a one-off command can target a
    // portable folder without touching the environment.
    retcomm::Paths paths =
        root_override.empty()
            ? retcomm::default_paths(exe_dir_from(argv[0]))
            : retcomm::paths_for_root(fs::absolute(root_override).lexically_normal(),
                                      retcomm::DataRootSource::Explicit);
    retcomm::AppConfig cfg = retcomm::load_app_config(paths.config_path);
    retcomm::set_github_token(cfg.github_token);

    const std::string& cmd = args[0];
    if (cmd == "catalog") {
        if (args.size() < 2 || args[1] != "update") {
            std::cerr << "usage: retcomm catalog update [--force]\n";
            return 2;
        }
        bool force = false;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--force")
                force = true;
            else {
                std::cerr << "unexpected catalog arg: " << args[i] << "\n";
                return 2;
            }
        }
        return cmd_catalog_update(paths, cfg, force);
    }

    if (cmd == "root") return cmd_root(paths, args, exe_dir_from(argv[0]));

    if (catalog_override.empty()) {
        try {
            retcomm::ensure_dirs(paths);
            const auto sync = retcomm::maybe_auto_update_catalog(paths, cfg);
            if (!sync.ok && !sync.skipped)
                std::cerr << "catalog auto-update: " << sync.message << "\n";
        } catch (const std::exception& e) {
            std::cerr << "catalog auto-update: " << e.what() << "\n";
        }
    }

    retcomm::Catalog catalog;
    try {
        const fs::path cat_dir =
            retcomm::resolve_catalog_dir(exe_dir_from(argv[0]), catalog_override, &paths);
        catalog = retcomm::load_catalog(cat_dir);
        std::vector<std::string> core_problems;
        retcomm::merge_core_titles(catalog, cfg, &core_problems);
        for (const auto& p : core_problems) std::cerr << p << "\n";
    } catch (const std::exception& e) {
        std::cerr << "catalog error: " << e.what() << "\n";
        return 1;
    }

    try {
        if (cmd == "list") return cmd_list(catalog);
        if (cmd == "status") return cmd_status(paths, cfg, catalog);
        if (cmd == "config") return cmd_config(paths, cfg);
        if (cmd == "library") {
            bool check_updates = false;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--check-updates")
                    check_updates = true;
                else if (args[i] == "list")
                    continue;
                else {
                    std::cerr << "unexpected library arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_library(paths, cfg, catalog, check_updates);
        }
        if (cmd == "romm") return cmd_romm(paths, cfg);
        if (cmd == "core") {
            // Titles that run through an rcore core (core_titles.hpp).
            if (args.size() >= 3 && args[1] == "add") {
                retcomm::CoreTitle ct;
                std::string err;
                if (!retcomm::read_core_title(args[2], ct, &err)) {
                    std::cerr << err << "\n";
                    return 1;
                }
                std::string name;
                for (size_t i = 3; i + 1 < args.size(); ++i) {
                    if (args[i] == "--name") name = args[++i];
                }
                auto c = retcomm::load_app_config(paths.config_path);
                bool known = false;
                for (auto& r : c.core_titles) {
                    std::error_code ec;
                    if (fs::equivalent(r.manifest, ct.manifest, ec)) {
                        known = true;
                        if (!name.empty()) r.name = name;
                    }
                }
                if (!known) c.core_titles.push_back({ct.manifest, name});
                retcomm::save_app_config(paths.config_path, c);
                std::cout << (known ? "updated " : "added ") << ct.id << " (" << ct.name << ", "
                          << ct.platform << ", " << ct.core_id << " " << ct.core_version
                          << (ct.engine_dirty ? ", dev build" : "") << ")\n"
                          << "  run `retcomm scan` to find its ROM\n";
                return 0;
            }
            if (args.size() < 2 || args[1] == "list") {
                for (const auto& r : cfg.core_titles) {
                    retcomm::CoreTitle ct;
                    std::string err;
                    if (!retcomm::read_core_title(r.manifest, ct, &err)) {
                        std::cout << "  (unreadable) " << err << "\n";
                        continue;
                    }
                    const auto idx = retcomm::load_library_index(paths.library_index_path);
                    const auto* t = catalog.find(ct.id);
                    const fs::path rom = t ? retcomm::boot_disc_rom(idx, *t) : fs::path();
                    std::cout << ct.id << "  " << (r.name.empty() ? ct.name : r.name) << "  ["
                              << ct.platform << ", " << ct.core_id << " " << ct.core_version
                              << (ct.engine_dirty ? ", dev" : "") << "]\n"
                              << "  core " << ct.library.string() << "\n"
                              << "  rom  " << (rom.empty() ? "(none matched; retcomm scan)" : rom.string())
                              << "\n";
                }
                if (cfg.core_titles.empty()) std::cout << "no core titles registered\n";
                return 0;
            }
            std::cerr << "usage: retcomm core add <sidecar.rcore.toml> [--name NAME] | core list\n";
            return 2;
        }
        if (cmd == "bios") {
            if (args.size() < 2 || args[1] == "list") return cmd_bios_list(paths, catalog);
            if (args[1] == "scan") {
                std::vector<fs::path> roots;
                bool full = false;
                for (size_t i = 2; i < args.size(); ++i) {
                    if (args[i] == "--full") {
                        full = true;
                    } else if (args[i] == "--bios-dir" && i + 1 < args.size()) {
                        roots.emplace_back(args[++i]);
                    } else {
                        std::cerr << "unexpected bios scan arg: " << args[i] << "\n";
                        return 2;
                    }
                }
                return cmd_bios_scan(paths, catalog, cfg, roots, full);
            }
            std::cerr << "usage: retcomm bios scan [--full] [--bios-dir DIR] | "
                         "retcomm bios list\n";
            return 2;
        }
        if (cmd == "scan") {
            std::vector<fs::path> roots;
            bool full = false;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--full") {
                    full = true;
                } else if (args[i] == "--rom-dir" && i + 1 < args.size()) {
                    roots.emplace_back(args[++i]);
                } else {
                    std::cerr << "unexpected scan arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_scan(paths, catalog, cfg, roots, full);
        }
        if (cmd == "install") {
            if (args.size() < 2) {
                std::cerr << "usage: retcomm install <title-id> [--force] [--prebuilt] "
                             "[--wine] [--dry-run]\n";
                return 2;
            }
            bool force = false;
            bool dry_run = false;
            bool use_wine = false;
            bool prefer_prebuilt = false;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--force")
                    force = true;
                else if (args[i] == "--dry-run")
                    dry_run = true;
                else if (args[i] == "--wine")
                    use_wine = true;
                else if (args[i] == "--prebuilt")
                    prefer_prebuilt = true;
                else {
                    std::cerr << "unexpected install arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_install(paths, catalog, args[1], force, dry_run, use_wine, prefer_prebuilt);
        }
        if (cmd == "build") {
            if (args.size() < 2) {
                std::cerr << "usage: retcomm build <title-id> [--force] [--force-generate] "
                             "[--rom PATH]\n";
                return 2;
            }
            bool force = false;
            bool force_generate = false;
            fs::path rom;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--force")
                    force = true;
                else if (args[i] == "--force-generate")
                    force_generate = true;
                else if (args[i] == "--rom" && i + 1 < args.size())
                    rom = args[++i];
                else {
                    std::cerr << "unexpected build arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_build(paths, catalog, args[1], force, force_generate, rom);
        }
        if (cmd == "pack") {
            if (args.size() < 3 || args[1] != "ensure") {
                std::cerr << "usage: retcomm pack ensure toolchain|sdk [--title ID] [--force]\n";
                return 2;
            }
            std::string title_id;
            bool force = false;
            for (size_t i = 3; i < args.size(); ++i) {
                if (args[i] == "--title" && i + 1 < args.size())
                    title_id = args[++i];
                else if (args[i] == "--force")
                    force = true;
                else {
                    std::cerr << "unexpected pack arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_pack_ensure(paths, catalog, args[2], title_id, force);
        }
        if (cmd == "update") {
            if (args.size() < 2) {
                std::cerr << "usage: retcomm update <title-id>|--all [--force] "
                             "[--force-generate]\n";
                return 2;
            }
            bool force = false;
            bool force_generate = false;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--force")
                    force = true;
                else if (args[i] == "--force-generate")
                    force_generate = true;
                else {
                    std::cerr << "unexpected update arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_update(paths, cfg, catalog, args[1], force, force_generate);
        }
        if (cmd == "uninstall" || cmd == "remove") {
            if (args.size() < 2) {
                std::cerr << "usage: retcomm uninstall <title-id> "
                             "[--keep-saves|--delete-saves] [--dry-run]\n";
                return 2;
            }
            retcomm::UninstallOptions opts;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--keep-saves")
                    opts.keep_saves = true;
                else if (args[i] == "--delete-saves" || args[i] == "--purge")
                    opts.keep_saves = false;
                else if (args[i] == "--dry-run")
                    opts.dry_run = true;
                else {
                    std::cerr << "unexpected uninstall arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_uninstall(paths, catalog, args[1], opts);
        }
        if (cmd == "orphans") {
            bool do_remove = false;
            retcomm::OrphanCleanupOptions opts;
            size_t i = 1;
            if (i < args.size() && (args[i] == "list" || args[i] == "remove")) {
                do_remove = (args[i] == "remove");
                ++i;
            } else if (i < args.size() && args[i].rfind("-", 0) != 0) {
                std::cerr << "usage: retcomm orphans [list|remove] "
                             "[--keep-saves|--delete-saves] [--dry-run] [--no-prune]\n";
                return 2;
            }
            for (; i < args.size(); ++i) {
                if (args[i] == "--keep-saves")
                    opts.keep_saves = true;
                else if (args[i] == "--delete-saves" || args[i] == "--purge")
                    opts.keep_saves = false;
                else if (args[i] == "--dry-run")
                    opts.dry_run = true;
                else if (args[i] == "--no-prune")
                    opts.prune_indexes = false;
                else {
                    std::cerr << "unexpected orphans arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_orphans(paths, catalog, do_remove, opts);
        }
        if (cmd == "cache") {
            if (args.size() < 2 || args[1] != "gc") {
                std::cerr << "usage: retcomm cache gc\n";
                return 2;
            }
            retcomm::AppConfig gc_cfg = cfg;
            gc_cfg.auto_gc_caches = true;
            const auto cr = retcomm::run_cache_gc(paths, gc_cfg);
            for (const auto& m : cr.messages) std::cout << m << "\n";
            std::cout << cr.message << "\n";
            return cr.ok ? 0 : 1;
        }
        if (cmd == "netplay") {
            return cmd_netplay(paths, cfg, catalog, args);
        }
        if (cmd == "launch") {
            if (args.size() < 2) {
                std::cerr << "usage: retcomm launch <title-id> [--rom PATH] [--bios PATH] "
                             "[--save PATH] [--mode default|direct|netplay] [--detach] "
                             "[--dry-run]\n";
                return 2;
            }
            retcomm::LaunchOptions opts;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--rom" && i + 1 < args.size()) {
                    opts.rom_path = args[++i];
                } else if (args[i] == "--bios" && i + 1 < args.size()) {
                    opts.bios_path = args[++i];
                } else if ((args[i] == "--save" || args[i] == "--save-path") &&
                           i + 1 < args.size()) {
                    opts.save_path = args[++i];
                } else if (args[i] == "--mode" && i + 1 < args.size()) {
                    std::string err;
                    opts.mode = retcomm::parse_launch_mode(args[++i], &err);
                    if (!err.empty()) {
                        std::cerr << err << "\n";
                        return 2;
                    }
                } else if (args[i] == "--detach") {
                    opts.detach = true;
                } else if (args[i] == "--dry-run") {
                    opts.dry_run = true;
                } else {
                    std::cerr << "unexpected launch arg: " << args[i] << "\n";
                    return 2;
                }
            }
            return cmd_launch(paths, catalog, args[1], opts);
        }
        std::cerr << "unknown command: " << cmd << "\n";
        print_help(argv[0]);
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
