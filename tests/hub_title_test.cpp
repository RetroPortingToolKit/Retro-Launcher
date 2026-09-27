// hub_title: title.json, the app and its data dir, the ROM checks, and the
// n64lle mods.toml in a state dir. Run as `retro-hub-title-test <scratch dir>`.
//
// The ROMs here are a few bytes of generated data, never a real image.

#include "hub/hub_title.hpp"
#include "retcomm/hash.hpp"
#include "retcomm/mods.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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

void unset_env(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    ::unsetenv(k);
#endif
}

} // namespace

int main(int argc, char** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "hub_title_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    for (const char* v : {"RETRO_HUB_APP", "APPIMAGE", "RETCOMM_PORTABLE_EXE"}) unset_env(v);

    const std::string rom_bytes = "not a rom, just test bytes 0123456789";
    const fs::path good_rom = dir / "elsewhere" / "game.z64";
    write(good_rom, rom_bytes);
    const std::string sha = retcomm::file_sha256_hex(good_rom);
    check(sha.size() == 64, "sha256 of the fixture");

    // ---- title.json ----------------------------------------------------------
    const fs::path payload = dir / "app" / "title";
    write(payload / "core" / "fake_core.so", "core");
    write(payload / "package" / "fake_game.so", "pkg");
    write(payload / "title.json", R"({
      "schema": 1, "id": "fake_title", "name": "Fake Title", "version": "0.0.1",
      "platform": "test", "core": "core/fake_core.so", "package": "package/fake_game.so",
      "opts": ["video.renderer=software"], "future_key": {"ignored": true},
      "rom": {"file_names": ["fake.z64"], "size": )" + std::to_string(rom_bytes.size()) +
                                        R"(, "sha256": ")" + sha + R"(", "label": "Fake - USA"}
    })");
    TitleInfo t;
    std::string err;
    check(load_title(payload, t, &err), "load_title from the directory");
    check(t.id == "fake_title" && t.name == "Fake Title", "id and name");
    check(t.core == (payload / "core" / "fake_core.so").lexically_normal(), "core is absolute");
    check(t.title_dir == (payload / "package").lexically_normal(), "title_dir defaults to the package's");
    check(t.opts.size() == 1 && t.opts[0].first == "video.renderer", "opts");
    check(t.has_rom && t.rom.has_size && t.rom.sha256 == sha, "rom identity");
    check(find_bundled_title(dir / "app") == fs::weakly_canonical(payload / "title.json"),
          "found beside the exe");
    check(find_bundled_title(dir).empty(), "none where there is none");

    write(dir / "t2" / "title.json", R"({"schema": 2, "id": "x", "core": "c.so"})");
    check(!load_title(dir / "t2", t, &err) && err.find("schema 2") != std::string::npos,
          "a newer schema is refused");
    write(dir / "t3" / "title.json", R"({"schema": 1, "id": "Bad Id", "core": "c.so"})");
    check(!load_title(dir / "t3", t, &err), "an id outside [a-z0-9_-] is refused");
    write(dir / "t4" / "title.json", R"({"schema": 1, "id": "x", "core": "../c.so"})");
    check(!load_title(dir / "t4", t, &err), "a path out of the payload is refused");
    check(load_title(payload / "title.json", t, &err), "load_title from the file");

    // ---- the app and its data dir -------------------------------------------
    const fs::path exe = dir / "app" / "retro-hub";
    write(exe, "");
    AppAnchor a = locate_app(exe);
    check(a.source == "exe" && a.dir == (dir / "app").lexically_normal(), "anchor: the hub itself");
    write(dir / "dl" / "Fake-0.0.1.AppImage", "");
    set_env("APPIMAGE", (dir / "dl" / "Fake-0.0.1.AppImage").string());
    a = locate_app(exe);
    check(a.source == "APPIMAGE" && a.dir == (dir / "dl").lexically_normal(),
          "anchor: $APPIMAGE, not the mount");
    set_env("RETRO_HUB_APP", (dir / "app" / "retro-hub").string());
    check(locate_app(exe).source == "RETRO_HUB_APP", "anchor: a --hub re-exec keeps the app");
    unset_env("RETRO_HUB_APP");
    const fs::path bundle_exe = dir / "mac" / "Fake.app" / "Contents" / "MacOS" / "retro-hub";
    write(bundle_exe, "");
    unset_env("APPIMAGE");
    a = locate_app(bundle_exe);
    check(a.source == "bundle" && a.dir == (dir / "mac").lexically_normal(), "anchor: the .app's parent");

    set_env("APPIMAGE", (dir / "dl" / "Fake-0.0.1.AppImage").string());
    a = locate_app(exe);
    TitleDataDir d = resolve_title_data_dir(a, "fake_title", dir / "userdata");
    check(d.beside_app && d.dir == dir / "dl" / "fake_title-data", "data dir beside the AppImage");
    check(!fs::exists(d.dir), "resolving creates nothing");
    check(ensure_title_data_dir(d, "fake_title", &err, dir / "userdata") && fs::is_directory(d.dir),
          "created on launch");
#if !defined(_WIN32)
    // A read-only app directory (a mounted dmg, a share): the user data dir.
    fs::create_directories(dir / "ro");
    write(dir / "ro" / "Fake.AppImage", "");
    fs::permissions(dir / "ro", fs::perms::owner_read | fs::perms::owner_exec);
    set_env("APPIMAGE", (dir / "ro" / "Fake.AppImage").string());
    d = resolve_title_data_dir(locate_app(exe), "fake_title", dir / "userdata");
    if (fs::path probe = dir / "ro" / "p"; std::ofstream(probe)) {
        fs::remove(probe); // running as root: nothing is read-only
    } else {
        check(!d.beside_app && d.dir == dir / "userdata" / "fake_title", "read-only: user data fallback");
    }
    fs::permissions(dir / "ro", fs::perms::owner_all);
#endif
    unset_env("APPIMAGE");
    const fs::path data = dir / "dl" / "fake_title-data";

    // ---- the ROM -------------------------------------------------------------
    RomCheck c = check_rom(good_rom, t.rom);
    check(c.ok, "the right image passes");
    const fs::path short_rom = dir / "short.z64";
    write(short_rom, "short");
    c = check_rom(short_rom, t.rom);
    check(!c.ok && c.error.find("expected " + std::to_string(rom_bytes.size()) + " bytes") !=
                       std::string::npos &&
              c.error.find("got 5 bytes") != std::string::npos,
          "a wrong size says expected vs got");
    std::string same_size = rom_bytes;
    same_size[0] = 'N';
    const fs::path wrong_rom = dir / "wrong.z64";
    write(wrong_rom, same_size);
    c = check_rom(wrong_rom, t.rom);
    check(!c.ok && c.error.find("expected sha256 " + sha) != std::string::npos &&
              c.error.find("got ") != std::string::npos,
          "a wrong sha256 says expected vs got");

    RomChoice r = resolve_rom(t, wrong_rom, dir / "dl", data);
    check(r.refused && r.path.empty(), "--rom that does not match is refused, not skipped");
    r = resolve_rom(t, good_rom, dir / "dl", data);
    check(!r.refused && r.source == "--rom", "--rom that matches");
    r = resolve_rom(t, {}, dir / "dl", data);
    check(r.path.empty() && !r.refused, "nothing resolves: the page asks");
    write(data / "roms" / "fake.z64", rom_bytes);
    r = resolve_rom(t, {}, dir / "dl", data);
    check(r.source == "data dir", "<data>/roms/<file_name>");
    write(dir / "dl" / "fake.z64", rom_bytes);
    r = resolve_rom(t, {}, dir / "dl", data);
    check(r.source == "beside the app", "beside the app before <data>/roms");
    check(remember_rom(data, good_rom, &err), "remember");
    check(remembered_rom(data) == fs::absolute(good_rom).lexically_normal(), "remembered path");
    r = resolve_rom(t, {}, dir / "dl", data);
    check(r.source == "remembered", "the remembered ROM first");
    fs::remove(good_rom);
    r = resolve_rom(t, {}, dir / "dl", data);
    check(r.source == "beside the app", "a remembered ROM that is gone is passed over");
    write(dir / "dl" / "fake.z64", same_size);
    fs::remove(data / "roms" / "fake.z64");
    r = resolve_rom(t, {}, dir / "dl", data);
    check(r.path.empty() && !r.notes.empty(), "a mismatching file beside the app is skipped, noted");

    // ---- mods.toml in the state dir -----------------------------------------
    const fs::path game = payload / "package";
    write(game / "mods" / "bundled" / "demo" / "manifest.toml",
          "format_version = 1\nid = \"demo\"\nversion = \"1.0.0\"\n\n[target]\n"
          "game_id = \"fake\"\nrom_sha256 = \"" + sha +
              "\"\n\n[feature.one]\n\n[patch.a]\nfeature = \"one\"\naddress = \"0x10\"\n"
              "expected = \"00\"\nreplace = \"01\"\n");
    retcomm::ModScanResult m = retcomm::scan_game_mods(game, {}, data);
    check(m.layout == retcomm::ModLayout::N64lle && m.selection == data / "mods.toml",
          "the selection lives in the state dir");
    if (!m.packages.empty()) {
        check(retcomm::set_scanned_mod_all(m, game, m.packages[0], true, &err), "enable all");
        check(fs::is_regular_file(data / "mods.toml"), "mods.toml written to the state dir");
        check(!fs::exists(game / "mods.toml"), "nothing written into the payload");
        m = retcomm::scan_game_mods(game, {}, data);
        check(!m.packages.empty() && !m.packages[0].features.empty() &&
                  m.packages[0].features[0].enabled,
              "read back from the state dir");
    } else {
        check(false, "the demo n64lle package scans");
    }

    // ---- paths ---------------------------------------------------------------
    const retcomm::Paths p = title_paths(data);
    check(p.config_path == data / "config.json" && p.data_dir == data, "title paths are flat");
    check(absolute_from("a/b.z64", dir) == (dir / "a" / "b.z64").lexically_normal(), "absolutize");

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("hub_title: ok\n");
    return 0;
}
