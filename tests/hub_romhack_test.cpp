// hub_romhack: the hub's windowless half of romhack BASES (romhacks.hpp; n64lle
// docs/ROMHACK-PLAN.md R5) -- what the Base picker lists, where a base's saves
// live, and where a mod stands on a base. Run as `retro-hub-romhack-test <scratch dir>`.
//
// The base.toml below is what n64lle's tools/n64romhack.py install writes.

#include "retcomm/mods.hpp"
#include "retcomm/romhacks.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace retcomm;

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

std::string base_toml(const std::string& id, const std::string& title, const std::string& saves) {
    return "# base.toml -- an INSTALLED romhack base\n[base]\nid = \"" + id +
           "\"\nversion = \"1.2.0\"\ntitle = \"" + title + "\"\n" +
           (saves.empty() ? "" : "saves = \"" + saves + "\"\n") +
           "[stock]\nsha256 = \"" + std::string(64, 'a') +
           "\"\n[patch]\nfile = \"patch.bps\"\nformat = \"bps\"\n[result]\nsha256 = \"" +
           std::string(64, 'b') + "\"\n[delta]\nfile = \"delta.so\"\n";
}

void install(const fs::path& title, const std::string& dir, const std::string& toml,
             bool with_delta = true) {
    write(title / "romhacks" / dir / "base.toml", toml);
    if (with_delta) write(title / "romhacks" / dir / "delta.so", "x");
}

void test_scan(const fs::path& dir) {
    const fs::path title = dir / "title";
    check(scan_romhacks(title).empty(), "no romhacks/ directory: nothing listed");
    install(title, "zz.hack", base_toml("zz.hack", "A Hack", ""));
    install(title, "shared.hack", base_toml("shared.hack", "B Hack", "shared-with-stock"));
    install(title, "named.wrong", base_toml("other.id", "C Hack", ""));
    install(title, "no.delta", base_toml("no.delta", "D Hack", ""), /*with_delta=*/false);
    fs::create_directories(title / "romhacks" / "not.a.base");  // no base.toml: not listed
    const auto list = scan_romhacks(title);
    check(list.size() == 4, "four bases listed (a directory without base.toml is not one)");
    check(list.size() == 4 && list[0].id == "zz.hack" && list[0].title == "A Hack",
          "sorted by title");
    const InstalledRomhack* a = find_romhack(list, "zz.hack");
    check(a && a->version == "1.2.0" && a->error.empty() && !a->saves_shared,
          "a good base: its version, no error, separate saves by default");
    const InstalledRomhack* s = find_romhack(list, "shared.hack");
    check(s && s->saves_shared, "saves = \"shared-with-stock\" is read");
    const InstalledRomhack* w = find_romhack(list, "named.wrong");
    check(w && w->error.find("says it is 'other.id'") != std::string::npos,
          "an id that is not its directory's name is listed, unusable, with why");
    const InstalledRomhack* d = find_romhack(list, "no.delta");
    check(d && d->error.find("delta package is missing") != std::string::npos,
          "a base without its delta is listed, unusable, with why");
    check(!find_romhack(list, "nope"), "an id not installed is not found");
}

void test_saves() {
    const fs::path data = "/data";
    check(base_save_dir(data, "pokemonstadium", "", false) == data / "saves" / "pokemonstadium",
          "the stock game: saves/<title>");
    check(base_save_dir(data, "pokemonstadium", "someone.hack", false) ==
              data / "saves" / "pokemonstadium@someone.hack",
          "a romhack: saves/<title>@<id>, apart from the game's");
    check(base_save_dir(data, "pokemonstadium", "someone.hack", true) ==
              data / "saves" / "pokemonstadium",
          "a romhack whose author shares the layout: the game's saves");
}

void test_standing(const fs::path& dir) {
    /* through the Mods page's own manifest reader */
    const fs::path game = dir / "game";
    const std::string sha(64, 'c');
    auto manifest = [&](const std::string& id, const std::string& bases) {
        return "format_version = 1\nid = \"" + id + "\"\nversion = \"1.0.0\"\n\n[target]\n"
               "game_id = \"g\"\nrom_sha256 = \"" + sha + "\"\n" + bases +
               "\n[feature.main]\nname = \"Main\"\n";
    };
    write(game / "mods/bundled/t.declared/manifest.toml",
          manifest("t.declared", "bases = [\"zz.hack\", \"other@3.0\"]"));
    write(game / "mods/bundled/t.pinned/manifest.toml", manifest("t.pinned", "bases = [\"zz.hack@9.9\"]"));
    write(game / "mods/bundled/t.plain/manifest.toml", manifest("t.plain", ""));
    write(game / "mods.toml", "");
    const ModScanResult scan = scan_game_mods(game, {}, {});
    auto pkg = [&](const std::string& id) -> const ModPackageInfo* {
        for (const auto& p : scan.packages)
            if (p.id == id) return &p;
        return nullptr;
    };
    const ModPackageInfo* dec = pkg("t.declared");
    check(dec && dec->bases.size() == 2 && dec->bases[1] == "other@3.0", "bases are read");
    InstalledRomhack b;
    b.id = "zz.hack";
    b.version = "1.2.0";
    check(dec && mod_base_standing(*dec, &b) == ModBaseStanding::Declared, "listed by id: declared");
    check(dec && mod_base_standing(*dec, nullptr) == ModBaseStanding::Stock, "no base: stock");
    const ModPackageInfo* pin = pkg("t.pinned");
    check(pin && mod_base_standing(*pin, &b) == ModBaseStanding::Unverified,
          "listed for another version only: unverified");
    b.version = "9.9";
    check(pin && mod_base_standing(*pin, &b) == ModBaseStanding::Declared,
          "listed for exactly this version: declared");
    const ModPackageInfo* plain = pkg("t.plain");
    check(plain && plain->bases.empty() && mod_base_standing(*plain, &b) == ModBaseStanding::Unverified,
          "lists no bases: unverified on any");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <scratch dir>\n", argv[0]);
        return 2;
    }
    const fs::path dir = argv[1];
    std::error_code ec;
    fs::remove_all(dir, ec);
    test_scan(dir);
    test_saves();
    test_standing(dir);
    if (g_failures) {
        std::fprintf(stderr, "hub_romhack: %d FAILED\n", g_failures);
        return 1;
    }
    std::printf("hub_romhack: PASS\n");
    return 0;
}
