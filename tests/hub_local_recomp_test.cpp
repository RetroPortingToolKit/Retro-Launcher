// hub_local_recomp: finding N64 dumps (zips included), staging one big-endian,
// where the tools are, the scaffold's command line and output, and running a
// process group that Cancel stops. Run as `retro-hub-local-recomp-test
// <scratch dir>`.
//
// The "ROMs" here are a header magic and a few bytes of generated data, never
// a real image.

#include "hub/hub_local_recomp.hpp"
#include "retcomm/hash.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace retcomm::hub::local_recomp;

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

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// A big-endian image: the header magic, then a pattern.
std::string z64_image(size_t n) {
    std::string s(n, '\0');
    const unsigned char magic[4] = {0x80, 0x37, 0x12, 0x40};
    for (size_t i = 0; i < n; ++i)
        s[i] = static_cast<char>(i < 4 ? magic[i] : static_cast<unsigned char>(i * 7 + 3));
    return s;
}

std::string swap16(std::string s) {
    for (size_t i = 0; i + 1 < s.size(); i += 2) std::swap(s[i], s[i + 1]);
    return s;
}

std::string swap32(std::string s) {
    for (size_t i = 0; i + 3 < s.size(); i += 4) {
        std::swap(s[i], s[i + 3]);
        std::swap(s[i + 1], s[i + 2]);
    }
    return s;
}

std::uint32_t crc32(const std::string& s) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void le(std::string& o, std::uint32_t v, int n) {
    for (int i = 0; i < n; ++i) o += static_cast<char>((v >> (8 * i)) & 0xFF);
}

// A stored (uncompressed) zip, written by hand: this build's miniz reads zips
// but has no writer.
bool write_zip(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, central;
    for (const auto& [name, body] : files) {
        const std::uint32_t offset = static_cast<std::uint32_t>(out.size());
        const std::uint32_t crc = crc32(body), size = static_cast<std::uint32_t>(body.size());
        le(out, 0x04034b50u, 4); le(out, 20, 2); le(out, 0, 2); le(out, 0, 2);
        le(out, 0, 2); le(out, 0, 2); le(out, crc, 4); le(out, size, 4); le(out, size, 4);
        le(out, static_cast<std::uint32_t>(name.size()), 2); le(out, 0, 2);
        out += name;
        out += body;
        le(central, 0x02014b50u, 4); le(central, 20, 2); le(central, 20, 2); le(central, 0, 2);
        le(central, 0, 2); le(central, 0, 2); le(central, 0, 2); le(central, crc, 4);
        le(central, size, 4); le(central, size, 4);
        le(central, static_cast<std::uint32_t>(name.size()), 2); le(central, 0, 2);
        le(central, 0, 2); le(central, 0, 2); le(central, 0, 2); le(central, 0, 4);
        le(central, offset, 4);
        central += name;
    }
    const std::uint32_t cd_off = static_cast<std::uint32_t>(out.size());
    out += central;
    le(out, 0x06054b50u, 4); le(out, 0, 2); le(out, 0, 2);
    le(out, static_cast<std::uint32_t>(files.size()), 2);
    le(out, static_cast<std::uint32_t>(files.size()), 2);
    le(out, static_cast<std::uint32_t>(central.size()), 4); le(out, cd_off, 4); le(out, 0, 2);
    write(p, out);
    return true;
}

bool has_arg(const std::vector<std::string>& v, const std::string& a) {
    return std::find(v.begin(), v.end(), a) != v.end();
}

std::string after(const std::vector<std::string>& v, const std::string& a) {
    auto it = std::find(v.begin(), v.end(), a);
    return it != v.end() && it + 1 != v.end() ? *(it + 1) : std::string();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <scratch dir>\n", argv[0]);
        return 2;
    }
    const fs::path scratch = fs::absolute(argv[1]);
    fs::remove_all(scratch);
    fs::create_directories(scratch);

    // ---- names and byte orders
    check(is_n64_rom_name("a.z64") && is_n64_rom_name("B.V64") && is_n64_rom_name("c.N64"),
          "the three N64 extensions, any case");
    check(!is_n64_rom_name("a.zip") && !is_n64_rom_name("z64") && !is_n64_rom_name("a.gb"),
          "nothing else is a dump name");

    const std::string img = z64_image(4096);
    const std::string img_sha = retcomm::sha256_hex(img);
    check(n64_byte_order(img) == "z64", "big-endian magic");
    check(n64_byte_order(swap16(img)) == "v64", "byte-swapped magic");
    check(n64_byte_order(swap32(img)) == "n64", "word-swapped magic");
    check(n64_byte_order("PK\x03\x04") .empty(), "a zip is no N64 image");
    {
        std::string v = swap16(img), n = swap32(img), order;
        check(n64_to_big_endian(v, &order) && order == "v64" && v == img, "v64 -> z64");
        check(n64_to_big_endian(n, &order) && order == "n64" && n == img, "n64 -> z64");
        std::string odd = swap16(img) + "x";
        check(!n64_to_big_endian(odd, &order), "an odd-length v64 is refused");
        std::string junk = "not a rom";
        check(!n64_to_big_endian(junk, &order) && junk == "not a rom",
              "not an image: refused, unchanged");
    }

    // ---- finding them
    const fs::path lib = scratch / "roms" / "n64";
    write(lib / "Plain Game (USA).z64", img);
    write(lib / "sub" / "Swapped.V64", swap16(img));
    write(lib / ".hidden" / "Hidden.z64", img);
    write(lib / "screenshots" / "Plain Game (USA).png", "png");
    write(lib / "notes.txt", "hello");
    check(write_zip(lib / "Zipped Game (Europe).zip",
                    {{"Zipped Game (Europe).n64", swap32(img)}, {"readme.txt", "hi"}}),
          "test zip written");
    write(lib / "broken.zip", "PK but not really");

    std::vector<std::string> problems;
    const auto roms = find_n64_roms({lib}, nullptr, &problems);
    check(roms.size() == 3, "a plain dump, a swapped one in a subfolder, one zip entry");
    if (roms.size() == 3) {
        check(roms[0].label == "Plain Game (USA).z64" && roms[0].entry.empty() &&
                  roms[0].size == img.size(),
              "plain file: label, no entry, size");
        check(roms[1].label == "sub/Swapped.V64", "subfolder dump labelled with its folder");
        check(roms[2].label == "Zipped Game (Europe).zip/Zipped Game (Europe).n64" &&
                  roms[2].entry == "Zipped Game (Europe).n64" &&
                  roms[2].file == lib / "Zipped Game (Europe).zip",
              "zip entry: the zip, its entry, labelled zip/entry");
    }
    check(problems.size() == 1 && problems[0].find("broken.zip") != std::string::npos,
          "an unreadable zip is a problem line, not a failure");
    {
        std::atomic<bool> stop{true};
        check(find_n64_roms({lib}, &stop, nullptr).empty(), "cancelled before the walk");
    }
    check(find_n64_roms({scratch / "nope"}, nullptr, &problems).empty(),
          "a missing root gives nothing");

    // ---- staging
    if (roms.size() == 3) {
        const fs::path stage = scratch / "stage";
        for (const auto& c : roms) {
            fs::path out;
            std::string sha, err;
            const bool ok = stage_rom(c, stage, &out, &sha, &err);
            check(ok, ("staged " + c.label + (err.empty() ? "" : ": " + err)).c_str());
            if (!ok) continue;
            check(out.extension() == ".z64", "staged as .z64");
            check(read(out) == img, "staged image is big-endian");
            check(sha == img_sha, "sha256 of the big-endian image");
        }
        check(fs::exists(stage / "Plain Game (USA).z64") && fs::exists(stage / "Swapped.z64") &&
                  fs::exists(stage / "Zipped Game (Europe).z64"),
              "staged names keep the dump's own stem");
        RomCandidate bad{lib / "notes.txt", {}, "notes.txt", 5};
        std::string err;
        check(!stage_rom(bad, stage, nullptr, nullptr, &err) &&
                  err.find("not an N64") != std::string::npos,
              "a non-image is refused by content, not by name");
        RomCandidate missing{lib / "Zipped Game (Europe).zip", "gone.z64", "x", 0};
        check(!stage_rom(missing, stage, nullptr, nullptr, &err), "a missing entry is refused");
    }

    // ---- the tools
    const fs::path launcher = scratch / "src" / "Retro-Launcher";
    const fs::path n64lle = scratch / "src" / "n64lle";
    write(launcher / "CMakeLists.txt", "");
    write(launcher / "scripts" / "build-local.sh", "");
    write(n64lle / "tools" / "new_project" / "setup_project.sh", "");
    fs::create_directories(launcher / "out" / "local" / "linux-x86_64");
    check(launcher_checkout_above(launcher / "out" / "local" / "linux-x86_64") == launcher,
          "the checkout three folders above a build-local prefix");
    check(launcher_checkout_above(launcher / "build") == launcher, "and above build/");
    // (The scratch dir itself may sit inside a real checkout's build tree.)
    check(launcher_checkout_above(scratch / "roms") != launcher,
          "not the checkout beside an unrelated folder");
    check(n64lle_beside(launcher) == n64lle, "n64lle beside the launcher checkout");
    check(n64lle_beside(scratch / "roms").empty(), "no n64lle beside an unrelated folder");

    const fs::path prefix = scratch / "prefix";
    check(!is_title_app_hub_prefix(prefix), "an empty folder is no hub prefix");
    write(prefix / "retro-hub", "");
    write(prefix / "retro-core-runner", "");
    check(!is_title_app_hub_prefix(prefix), "a hub prefix needs the packaging kit");
    write(prefix / "packaging" / "title" / "build-title-app.sh", "");
    check(is_title_app_hub_prefix(prefix), "hub + runner + kit is a title-app prefix");

    check(!tools_problem(Tools{}).empty(), "no checkout: a problem");
    check(!tools_problem(Tools{n64lle, {}, {}}).empty(), "no hub and no launcher: a problem");
    check(tools_problem(Tools{n64lle, prefix, {}}).empty(), "checkout + prefix: ready");
    check(tools_problem(Tools{n64lle, {}, launcher}).empty(), "checkout + launcher: ready");

    // ---- the command line
    {
        const auto a = scaffold_argv(n64lle, scratch / "stage" / "G.z64", prefix,
                                     scratch / "apps");
        check(a.size() > 2 && a[0] == "bash" &&
                  a[1] == (n64lle / "tools" / "new_project" / "setup_project.sh").string() &&
                  a[2] == (scratch / "stage" / "G.z64").string(),
              "bash setup_project.sh <rom>");
        check(has_arg(a, "--yes"), "never asks");
        check(after(a, "--core") == "generate", "the dev core, built from the checkout");
        check(after(a, "--runner") == (prefix / "retro-core-runner").string() &&
                  after(a, "--hub") == (prefix / "retro-hub").string(),
              "the prefix's runner and hub");
        check(after(a, "--dir") == (scratch / "apps").string(), "made in the install root");
        check(after(a, "--players") == "4" && has_arg(a, "--transfer-pak"),
              "every port plugged in, the Transfer Pak supported");
        check(has_arg(a, "--copy-rom"), "the project keeps its own copy of the staged dump");
        check(has_arg(a, "--generate") && has_arg(a, "--app"), "built through to the app");
        check(!has_arg(a, "--gh"), "no GitHub repository");
    }
    check(build_local_argv(launcher).back() == (launcher / "scripts" / "build-local.sh").string(),
          "build-local.sh from the checkout");

    // ---- output
    check(strip_ansi("\x1b[1m==> Probing x.z64\x1b[0m\r") == "==> Probing x.z64", "ANSI removed");
    check(step_title("\x1b[1m==> Gates\x1b[0m") == "Gates", "a step line");
    check(step_title("   ==> not at the start").empty() && step_title("plain").empty(),
          "anything else is no step");
    check(done_root("\x1b[1m==> Done: /a/b/Game Recomp\x1b[0m") == fs::path("/a/b/Game Recomp"),
          "the Done line names the project");
    check(done_root("==> Gates").empty(), "other steps name no project");
    check(retro_hub_line("RETRO_HUB=/x/retro-hub") == "/x/retro-hub" &&
              retro_hub_line("build-local: RETRO_HUB=/x").empty(),
          "RETRO_HUB= only at the start of a line");

    // ---- what the scaffold leaves
    {
        const fs::path proj = scratch / "apps" / "Game Recomp";
        GeneratedApp g;
        std::string err;
        check(!find_generated_app(proj, "G.z64", &g, &err), "no build-release: refused");
        write(proj / "build-release" / "Game-0.0.1-linux-x86_64.AppImage", "app");
        check(!find_generated_app(proj, "G.z64", &g, &err) &&
                  err.find("payload") != std::string::npos,
              "an app without its payload: refused");
        write(proj / "build-release" / "app" / "title" / "title.json", "{}");
        write(proj / "roms" / "G.z64", img);
        check(find_generated_app(proj, "G.z64", &g, &err), "app + payload found");
        check(g.app == proj / "build-release" / "Game-0.0.1-linux-x86_64.AppImage" &&
                  g.title_json == proj / "build-release" / "app" / "title" / "title.json" &&
                  g.rom == proj / "roms" / "G.z64",
              "the app at build-release's root, the payload in app/title, the ROM copy");
        fs::remove(proj / "roms" / "G.z64");
        write(proj / "roms" / "game.z64", img);
        write(proj / "roms" / "README.md", "x");
        check(find_generated_app(proj, "G (USA).z64", &g, &err) &&
                  g.rom == proj / "roms" / "game.z64",
              "the scaffold's slug-named copy when the dump's own name is not there");
    }

#if !defined(_WIN32)
    // ---- running
    {
        std::vector<std::string> lines;
        std::string err;
        const int rc = run_process({"sh", "-c", "echo one; echo two >&2; printf three; exit 3"},
                                   scratch, {{"LR_TEST", "v"}},
                                   [&](const std::string& s) { lines.push_back(s); }, nullptr,
                                   &err);
        check(rc == 3, "the exit status");
        check(lines.size() == 3 && lines[0] == "one" && lines[1] == "two" && lines[2] == "three",
              "stdout and stderr, a line at a time, the last without a newline");
        lines.clear();
        run_process({"sh", "-c", "echo \"$LR_TEST\"; pwd; read x || echo eof"}, scratch,
                    {{"LR_TEST", "v"}}, [&](const std::string& s) { lines.push_back(s); },
                    nullptr, &err);
        check(lines.size() == 3 && lines[0] == "v" && fs::path(lines[1]) == scratch &&
                  lines[2] == "eof",
              "environment, working directory, stdin at EOF");
        check(run_process({"no-such-program-here"}, scratch, {}, nullptr, nullptr, &err) == 127,
              "a missing program exits 127");

        // A child that starts grandchildren: Cancel stops the whole group, and
        // nothing is left behind to hold the pipe open.
        std::atomic<bool> stop{false};
        const fs::path marker = scratch / "grandchild.pid";
        const auto t0 = std::chrono::steady_clock::now();
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            stop.store(true);
        });
        const int crc = run_process(
            {"sh", "-c", "sleep 30 & echo $! > '" + marker.string() + "'; sleep 30"}, scratch,
            {}, nullptr, &stop, &err);
        canceller.join();
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check(crc == 128 + 15, "cancelled: killed by SIGTERM");
        check(secs < 5.0, "cancelled promptly");
        const std::string gpid = read(marker);
        if (!gpid.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            check(::kill(std::stoi(gpid), 0) != 0, "the grandchild is gone too");
        }

        // A daemon holding the pipe does not keep us waiting after the leader exits.
        const auto t1 = std::chrono::steady_clock::now();
        const int drc = run_process({"sh", "-c", "(sleep 5 &) ; echo led; exit 0"}, scratch, {},
                                    nullptr, nullptr, &err);
        const double dsecs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        check(drc == 0 && dsecs < 3.0, "the leader's exit ends the run");
    }
#endif

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("hub_local_recomp: all checks passed\n");
    return 0;
}
