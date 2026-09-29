// hub_core_settings: --describe parsing, the bindings and option files, and
// the labels a page shows. Run as `retro-hub-settings-test <scratch dir>`.
//
// The describe records below are real: retro-core-runner --describe against
// n64lle's generic core 0.374.0 (2026-09-26), trimmed.

#include "hub/hub_core_settings.hpp"
#include "retcomm/mods.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
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

const char* kN64 =
    "describe\t1\n"
    "core\tn64lle\t0.374.0\tn64\n"
    "option\tvideo.renderer\tenum\trestart,netplay\t1\ttitle\t0\t0\tRenderer\tThe RDP rasterizer.\n"
    "value\tvideo.renderer\ttitle\n"
    "value\tvideo.renderer\tsoftware\n"
    "value\tvideo.renderer\topengl\n"
    "value\tvideo.renderer\tlle-software\n"
    "option\tdeveloper.state_hash_every\tint\trestart,developer\t1\t0\t0\t1000000\tState-hash "
    "trace interval\tEvery N fields.\n"
    "option\tengine.cache\tstring\trestart,developer\t0\t\t0\t0\tN64LLE_CACHE\tA knob.\n"
    "input\t1\t0\t0\tA\n"
    "input\t4\t0\t0\tB\n"
    "input\t64\t0\t0\tZ\n"
    "input\t0\t1\t0\tControl Stick (left/right)\n"
    "input\t0\t2\t0\tControl Stick (up/down)\n"
    "input\t0\t4\t1\tC-Up\n"
    "input\t0\t4\t-1\tC-Down\n";

void test_parse_n64() {
    CoreDescription d;
    std::string err;
    check(parse_core_description(kN64, d, &err), "n64 description parses");
    check(d.ok && d.core_id == "n64lle" && d.core_version == "0.374.0" && d.platforms == "n64",
          "core line");
    check(d.options.size() == 3, "three options");
    const CoreOptionDecl& r = d.options[0];
    check(r.key == "video.renderer" && r.type == "enum" && r.restart && r.netplay &&
              !r.developer && r.has_default && r.default_value == "title",
          "renderer option fields");
    check(r.values.size() == 4 && r.values[3] == "lle-software", "renderer values in order");
    check(d.options[1].int_max == 1000000 && d.options[1].developer, "int range and flags");
    check(!d.options[2].has_default && d.options[2].default_value.empty(), "no default");
    check(d.inputs.size() == 7, "seven inputs");

    check(pad_target_label(PadTarget::South, d) == "A", "South is A");
    check(pad_target_label(PadTarget::West, d) == "B", "West is B");
    check(pad_target_label(PadTarget::L2, d) == "Z", "L2 is Z");
    check(pad_target_label(PadTarget::LyPlus, d) == "Control Stick (up/down): up",
          "a whole-axis descriptor names each half");
    check(pad_target_label(PadTarget::RyPlus, d) == "C-Up", "C-Up on the right stick's up");
    check(pad_target_label(PadTarget::RyMinus, d) == "C-Down", "C-Down on its down");
    check(!pad_target_declared(PadTarget::East, d), "East is not an N64 input");
    check(pad_target_label(PadTarget::East, d) == pad_target_generic_name(PadTarget::East),
          "an undeclared target keeps its generic name");
}

void test_parse_edges() {
    CoreDescription d;
    std::string err;
    // Escapes, and stderr noise mixed in (the spawn log holds both streams).
    check(parse_core_description("core: loading\n"
                                 "describe\t1\n"
                                 "option\tk\tstring\t-\t0\t\t0\t0\ttab\\there\tline\\nnext \\\\ end\n",
                                 d, &err),
          "escaped description parses");
    check(d.options.size() == 1 && d.options[0].label == "tab\there" &&
              d.options[0].description == "line\nnext \\ end",
          "escapes decode");
    check(!parse_core_description("describe\t2\n", d, &err), "unknown revision refused");
    check(!parse_core_description("retro-core-runner: unknown flag --describe\n", d, &err),
          "no header refused");
    check(!parse_core_description("describe\t1\nvalue\tnope\tx\n", d, &err),
          "an orphan value refused");
}

void test_bindings(const fs::path& dir) {
    const InputBindings def = default_input_bindings();
    std::vector<std::string> warn;
    check(load_platform_input(dir, "n64", &warn) == PlatformInput{}, "no file is the defaults");
    check(PlatformInput{}.maps[3] == def && PlatformInput{}.deadzone_pct == 10,
          "every seat starts on the default maps, a 10% deadzone");
    check(def.pad[static_cast<size_t>(PadTarget::Function)] ==
              PadSource{PadSource::Button, SDL_GAMEPAD_BUTTON_BACK},
          "Function defaults to Back / Select");

    // The defaults are the table the hub used before bindings existed.
    auto pad = [&](PadTarget t) { return def.pad[static_cast<int>(t)]; };
    check(pad(PadTarget::South) == PadSource{PadSource::Button, SDL_GAMEPAD_BUTTON_SOUTH},
          "South from SDL South");
    check(pad(PadTarget::L2) == PadSource{PadSource::AxisPlus, SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
          "L2 from the left trigger");
    check(pad(PadTarget::LyPlus) == PadSource{PadSource::AxisMinus, SDL_GAMEPAD_AXIS_LEFTY},
          "stick up is SDL's negative Y");
    check(def.key[static_cast<int>(PadTarget::South)] == SDL_SCANCODE_X, "X key is South");
    check(def.key[static_cast<int>(PadTarget::RyPlus)] == SDL_SCANCODE_T &&
              def.key[static_cast<int>(PadTarget::RxPlus)] == SDL_SCANCODE_H,
          "the keyboard reaches the right stick (n64lle's C buttons)");

    PlatformInput in;
    in.deadzone_pct = 12;
    in.seats[0] = {SeatAssign::Keyboard, "", ""};
    in.seats[1] = {SeatAssign::Gamepad, "03000000de280000ff11000001000000", "Steam Pad"};
    in.seats[3] = {SeatAssign::None, "", ""};
    in.maps[1].pad[static_cast<int>(PadTarget::South)] = {PadSource::Button, SDL_GAMEPAD_BUTTON_EAST};
    in.maps[1].pad[static_cast<int>(PadTarget::R3)] = {};
    in.maps[0].key[static_cast<int>(PadTarget::Start)] = SDL_SCANCODE_SPACE;
    in.maps[0].key[static_cast<int>(PadTarget::L2)] = SDL_SCANCODE_UNKNOWN;
    std::string err;
    check(save_platform_input(dir, "n64", in, &err), "seats save");
    warn.clear();
    check(load_platform_input(dir, "n64", &warn) == in, "seats round-trip");
    check(warn.empty(), "a saved file loads without warnings");
    check(load_platform_input(dir, "psx") == PlatformInput{}, "another platform is untouched");

    PadSource s;
    check(pad_source_from_string("lefttrigger+", s) && s.kind == PadSource::AxisPlus,
          "axis source parses");
    check(!pad_source_from_string("nonsense", s), "unknown source refused");
}

void test_seat_plan() {
    PlatformInput in; // all Auto
    auto p = plan_seats(in, {});
    check(p[0].keyboard && !p[1].connected(), "no pads: the keyboard is port 1");
    p = plan_seats(in, {"g1", "g2"});
    check(p[0].pad == 0 && p[1].pad == 1 && !p[0].keyboard && !p[2].connected(),
          "Auto seats take pads in order, no keyboard");

    in.seats[1] = {SeatAssign::Gamepad, "g1", "first"};
    p = plan_seats(in, {"g1", "g2"});
    check(p[1].pad == 0 && p[0].pad == 1, "a Gamepad seat claims its pad before Auto seats");
    p = plan_seats(in, {"g2"});
    check(!p[1].connected() && p[0].pad == 0, "a Gamepad seat whose pad is absent is unplugged");

    in.seats[2] = {SeatAssign::Keyboard, "", ""};
    p = plan_seats(in, {});
    check(p[2].keyboard && !p[0].keyboard, "a Keyboard seat stops port 1's keyboard fallback");
    in.seats[0] = {SeatAssign::None, "", ""};
    p = plan_seats(in, {"g2"});
    check(!p[0].connected() && p[3].pad == 0, "None is unplugged; the pad goes to the next Auto");
}

void test_options(const fs::path& dir) {
    check(load_core_options(dir, "n64", "pokemonstadiumtest_game").empty(), "no options file");
    std::string perr;
    check(save_core_options(dir, "n64", "", {{"video.renderer", "software"}, {"a", "1"}}, &perr),
          "platform options save");
    check(load_core_options(dir, "n64", "").at("video.renderer") == "software",
          "platform options round-trip");
    const auto layered = layer_core_options({{"video.renderer", "software"}, {"a", "1"}},
                                            {{"video.renderer", "opengl"}}, {{"a", "2"}});
    check(layered.at("video.renderer") == "opengl" && layered.at("a") == "2",
          "platform < title < command line");
    std::map<std::string, std::string> v{{"video.renderer", "opengl"},
                                         {"developer.state_hash_every", "60"}};
    std::string err;
    check(save_core_options(dir, "n64", "pokemonstadiumtest_game", v, &err), "options save");
    check(load_core_options(dir, "n64", "pokemonstadiumtest_game") == v, "options round-trip");
    check(!save_core_options(dir, "n64", "t", {{"k", "a\nb"}}, &err),
          "a value with a newline refused");
}

void write(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// An n64lle guarded-write package (n64lle docs/MODDING.md 5.1) in a title dir.
void test_n64lle_mods(const fs::path& dir) {
    const fs::path game = dir / "title";
    write(game / "mods/bundled/yourname.first-mod/manifest.toml",
          "format_version = 1\n"
          "id = \"yourname.first-mod\"\n"
          "version = \"0.1.0\"\n\n"
          "[target]\n"
          "game_id = \"pokemon-stadium\"\n"
          "rom_sha256 = \"502f6082a6436012a8b61419435dec1388869a90ed870e87e2d7bee88f831519\"\n\n"
          "[feature.main]\n"
          "default_enabled = true\n\n"
          "[feature.other]\n\n"
          "[patch.one]\nfeature = \"main\"\naddress = \"0x10\"\nexpected = \"00\"\nreplace = \"01\"\n"
          "[patch.two]\nfeature = \"main\"\naddress = \"0x11\"\nexpected = \"00\"\nreplace = \"01\"\n");
    // Named for another id: the game refuses it, so the page must not list it.
    write(game / "mods/installed/wrong-dir/manifest.toml",
          "format_version = 1\nid = \"other\"\nversion = \"1.0.0\"\n[target]\n"
          "game_id = \"x\"\nrom_sha256 = \"00\"\n[feature.main]\n");

    retcomm::ModScanResult r = retcomm::scan_game_mods(game);
    check(r.layout == retcomm::ModLayout::N64lle, "n64lle layout detected");
    check(r.packages.size() == 1 && r.errors.size() == 1, "one package, one refusal");
    if (r.packages.size() != 1) return;
    const retcomm::ModPackageInfo& p = r.packages[0];
    check(p.id == "yourname.first-mod" && p.version == "0.1.0", "id and version");
    check(p.features.size() == 2 && p.features[0].id == "main" && p.features[0].enabled &&
              !p.features[1].enabled,
          "features with manifest defaults");
    check(p.features[0].description == "2 guarded writes.", "writes counted per feature");

    std::string err;
    // Turning `other` on makes the section explicit: main (a default) stays on.
    check(retcomm::set_scanned_mod_enabled(r, game, p, "other", true, &err), "toggle writes");
    const std::string sel = slurp(game / "mods.toml");
    check(sel.find("\n[yourname.first-mod]\n") != std::string::npos,
          "dotted id bare, as n64lle writes it");
    check(sel.find("enabled = \"main other\"") != std::string::npos, "exact enabled list");
    r = retcomm::scan_game_mods(game);
    check(r.packages.size() == 1 && r.packages[0].features[0].enabled &&
              r.packages[0].features[1].enabled,
          "rescan reads the selection back");

    check(retcomm::set_scanned_mod_all(r, game, r.packages[0], false, &err), "disable all");
    const std::string off = slurp(game / "mods.toml");
    check(off.find("enabled = \"\"") != std::string::npos, "all off is an empty list");
    check(off.find("version = \"0.1.0\"") != std::string::npos, "version line kept");
    r = retcomm::scan_game_mods(game);
    check(!r.packages[0].features[0].enabled, "an empty list overrides a default");

    // The same id in both roots refuses both.
    write(game / "mods/installed/yourname.first-mod/manifest.toml",
          slurp(game / "mods/bundled/yourname.first-mod/manifest.toml"));
    r = retcomm::scan_game_mods(game);
    check(r.packages.empty(), "a duplicate id shows neither copy");
}

} // namespace

// A fake Game Boy header: `type` at 0x147, `ram` at 0x149, a valid checksum.
void write_gb_rom(const fs::path& p, unsigned char type, unsigned char ram) {
    std::string rom(0x8000, '\0');
    rom[0x147] = static_cast<char>(type);
    rom[0x149] = static_cast<char>(ram);
    unsigned char sum = 0;
    for (int i = 0x134; i <= 0x14C; ++i) sum = static_cast<unsigned char>(sum - static_cast<unsigned char>(rom[static_cast<size_t>(i)]) - 1);
    rom[0x14D] = static_cast<char>(sum);
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << rom;
}

// Transfer Pak Support: the three accepted dumps, their settings file, and the
// refusals. The real dumps are checked too when RETRO_TEST_GB_DIR names a
// folder holding them (they are not in the repo).
void test_tpak_library(const fs::path& dir) {
    check(supported_gb_rom_by_sha256(supported_gb_rom(1).sha256) == 1 &&
              supported_gb_rom_by_sha256("00") == -1,
          "supported dumps are found by sha256");
    const fs::path gb = dir / "tpaklib";
    write_gb_rom(gb / "fake_red.gb", 0x13, 0x03);
    std::string sha, err;
    check(!verify_supported_gb_rom(0, gb / "fake_red.gb", &sha, &err) &&
              err.find("not the supported Pokemon Red dump") != std::string::npos &&
              sha.size() == 64,
          "a Game Boy ROM that is not the dump is refused, naming the dump");
    fs::create_directories(gb);
    std::ofstream(gb / "junk.gb", std::ios::binary) << std::string(0x200, 'x');
    err.clear();
    check(!verify_supported_gb_rom(2, gb / "junk.gb", &sha, &err) &&
              err.find("no valid Game Boy header") != std::string::npos,
          "a file that is no Game Boy ROM says so");
    err.clear();
    check(!verify_supported_gb_rom(1, gb / "missing.gb", &sha, &err) && !err.empty(),
          "a missing file is refused");

    TransferPakLibrary lib;
    check(!lib.complete(), "an empty library is incomplete");
    lib.path[0] = "/roms/red.gb";
    lib.sha256[0] = supported_gb_rom(0).sha256;
    lib.path[2] = "/roms/yellow.gb";
    lib.sha256[2] = supported_gb_rom(2).sha256;
    check(save_tpak_library(dir, "n64", lib, &err), "transfer_pak.ini saves");
    check(load_tpak_library(dir, "n64") == lib, "paths and hashes read back, the unset one empty");
    err.clear();
    check(!recheck_tpak_library(lib, &err) && err.find("Pokemon Red") != std::string::npos,
          "a recheck reports the first problem");

    if (const char* real = std::getenv("RETRO_TEST_GB_DIR")) {
        const char* names[3] = {"Pokemon - Red Version (USA, Europe) (SGB Enhanced).gb",
                                "Pokemon - Blue Version (USA, Europe) (SGB Enhanced).gb",
                                "Pokemon - Yellow Version - Special Pikachu Edition (USA, Europe) "
                                "(CGB+SGB Enhanced).gb"};
        TransferPakLibrary full;
        for (int i = 0; i < 3; ++i) {
            const fs::path f = fs::path(real) / names[i];
            err.clear();
            check(verify_supported_gb_rom(i, f, &sha, &err), ("the real dump verifies: " + err).c_str());
            full.path[static_cast<size_t>(i)] = f.string();
            full.sha256[static_cast<size_t>(i)] = sha;
        }
        err.clear();
        check(!verify_supported_gb_rom(0, fs::path(real) / names[1], &sha, &err) &&
                  err.find("is Pokemon Blue, not Pokemon Red") != std::string::npos,
              "Blue offered as Red is named as Blue");
        check(full.complete() && recheck_tpak_library(full, &err), "the real library rechecks");
    }
}

void test_transfer_pak(const fs::path& dir) {
    const fs::path gb = dir / "tpak";
    write_gb_rom(gb / "red.gb", 0x13, 0x03); // MBC3+RAM+BATTERY, 32 KiB
    write_gb_rom(gb / "mbc2.gb", 0x06, 0x00);
    write_gb_rom(gb / "noram.gb", 0x00, 0x00);
    std::string err;
    check(gb_cart_ram_bytes(gb / "red.gb", &err) == 32 * 1024, "32 KiB cart RAM from the header");
    check(gb_cart_ram_bytes(gb / "mbc2.gb", &err) == 512, "MBC2 carts hold 512 bytes");
    err.clear();
    check(gb_cart_ram_bytes(gb / "noram.gb", &err) == 0 && err.empty(), "no RAM, no error");
    {
        std::ofstream(gb / "junk.gb", std::ios::binary) << std::string(0x200, 'x');
        err.clear();
        check(gb_cart_ram_bytes(gb / "junk.gb", &err) == 0 && !err.empty(), "a bad header is refused");
    }
    check(create_gb_save(gb / "red.gb", gb / "Red.srm", &err) &&
              fs::file_size(gb / "Red.srm") == 32 * 1024,
          "a new save is the cart's size");
    {
        std::ifstream in(gb / "Red.srm", std::ios::binary);
        char c = 0;
        in.get(c);
        check(static_cast<unsigned char>(c) == 0xFF, "a new save reads as blank battery RAM");
    }
    check(!create_gb_save(gb / "red.gb", gb / "Red.srm", &err), "an existing save is not overwritten");
    check(!create_gb_save(gb / "noram.gb", gb / "none.srm", &err), "a cart without RAM gets no save");

    // Every seat's Transfer Pak round-trips through input.ini.
    PlatformInput in;
    in.paks[0] = {SeatPak::TransferPak, (gb / "red.gb").string(), (gb / "Red.srm").string()};
    in.paks[3] = {SeatPak::TransferPak, (gb / "mbc2.gb").string(), ""};
    check(save_platform_input(dir, "n64", in, &err), "input.ini with paks saves");
    const PlatformInput back = load_platform_input(dir, "n64");
    check(back.paks[0] == in.paks[0], "seat 1's Transfer Pak reads back");
    check(back.paks[3] == in.paks[3], "seat 4's Transfer Pak reads back");
    check(back.paks[1].kind == SeatPak::None, "a seat without one stays None");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <scratch dir>\n", argv[0]);
        return 2;
    }
    const fs::path dir = argv[1];
    std::error_code ec;
    fs::remove_all(dir, ec);
    test_parse_n64();
    test_parse_edges();
    {
        CoreDescription d;
        std::string err;
        check(!load_description_cache(dir, "n64").ok, "no cache yet");
        parse_core_description(kN64, d, &err);
        d.raw = kN64;
        check(save_description_cache(dir, "n64", d, &err), "cache saves");
        const CoreDescription c = load_description_cache(dir, "n64");
        check(c.ok && c.cached && c.options.size() == 3, "cache reads back, marked cached");
    }
    test_bindings(dir);
    test_seat_plan();
    test_options(dir);
    test_n64lle_mods(dir);
    test_transfer_pak(dir);
    test_tpak_library(dir);
    {
        // The host hotkeys round-trip through play.ini with the overlay's settings.
        PlayPrefs pr;
        check(pr.hotkeys == default_host_hotkeys(), "hotkeys start at the defaults");
        pr.show_fps = true;
        check(pr.turbo_sound, "sound during turbo is on by default");
        pr.turbo_sound = false;
        pr.hotkeys.key[static_cast<size_t>(HostAction::Turbo)] = SDL_SCANCODE_LSHIFT;
        pr.hotkeys.combo[static_cast<size_t>(HostAction::SaveStates)] =
            PadSource{PadSource::Button, SDL_GAMEPAD_BUTTON_NORTH};
        pr.hotkeys.combo[static_cast<size_t>(HostAction::ShowFps)] = {};
        std::string err;
        check(save_play_prefs(dir, pr, &err), "play.ini saves");
        const PlayPrefs back = load_play_prefs(dir);
        check(back == pr, "hotkeys, an unbound combo included, read back");
    }
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("hub_core_settings: all checks passed\n");
    return 0;
}
