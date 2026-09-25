// retcomm-core-link-test -- drives a core through the hub's link, the way the
// hub does, and writes what headless mode writes. If the link is correct, its
// artifacts are byte-identical to `retcomm-core-runner` headless and to
// n64lle's rcore_probe on the same core and scenario (docs/CORE_LINK.md).
//
// Same flags as headless mode (a subset: no --replay-at, which needs the
// savestate envelope), plus --runner <path to retcomm-core-runner>.
// Outputs in --out: core.log, events.tsv and state_hash.tsv come from the
// runner's session dir, which is --out; shot.ppm is the last picture taken
// from shared memory; summary.txt is grepped from core.log.

#include "core_link.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace retcomm::corelink;

namespace {

[[noreturn]] void die(const std::string& m) {
    std::fprintf(stderr, "retcomm-core-link-test: %s\n", m.c_str());
    std::exit(2);
}

std::uint32_t script_buttons(const std::string& s) {
    std::uint32_t b = 0;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t plus = s.find('+', start);
        const std::string t =
            s.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (t.empty() || t == "-" || t == "none") {
        } else if (t == "a") b |= RCORE_PAD_SOUTH;
        else if (t == "b") b |= RCORE_PAD_WEST;
        else if (t == "z") b |= RCORE_PAD_L2;
        else if (t == "l") b |= RCORE_PAD_L1;
        else if (t == "r") b |= RCORE_PAD_R1;
        else if (t == "start") b |= RCORE_PAD_START;
        else if (t == "dup") b |= RCORE_PAD_DPAD_UP;
        else if (t == "ddown") b |= RCORE_PAD_DPAD_DOWN;
        else if (t == "dleft") b |= RCORE_PAD_DPAD_LEFT;
        else if (t == "dright") b |= RCORE_PAD_DPAD_RIGHT;
        else die("--input-script: unknown button '" + t + "'");
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return b;
}

} // namespace

int main(int argc, char** argv) {
    LaunchSpec spec;
    spec.gl = false;
    std::uint64_t frames = 60;
    bool seat0 = true;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> script;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) die(a + " needs a value");
            return argv[++i];
        };
        if (a == "--runner") spec.runner = val();
        else if (a == "--core") spec.core = val();
        else if (a == "--rom") spec.rom = val();
        else if (a == "--title-dir") spec.title_dir = val();
        else if (a == "--out") spec.session_dir = val();
        else if (a == "--frames") frames = std::strtoull(val().c_str(), nullptr, 10);
        else if (a == "--load-state") spec.load_state = fs::path(val());
        else if (a == "--tpak1-rom") spec.tpak_rom = val();
        else if (a == "--tpak1-save") spec.save_files["tpak1"] = val();
        else if (a == "--tpak1-rtc") spec.save_files["tpak1.rtc"] = val();
        else if (a == "--gl") spec.gl = true;
        else if (a == "--strict") spec.strict = true;
        else if (a == "--no-seats") seat0 = false;
        else if (a == "--opt") {
            const std::string kv = val();
            const auto eq = kv.find('=');
            if (eq == std::string::npos) die("--opt key=value");
            spec.options[kv.substr(0, eq)] = kv.substr(eq + 1);
        } else if (a == "--input-script") {
            const std::string s = val();
            size_t start = 0;
            while (start <= s.size()) {
                const size_t comma = s.find(',', start);
                const std::string ev = s.substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start);
                const auto colon = ev.find(':');
                if (colon == std::string::npos) die("--input-script F:buttons,...");
                script.emplace_back(std::strtoull(ev.substr(0, colon).c_str(), nullptr, 10),
                                    script_buttons(ev.substr(colon + 1)));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (a == "--replay-at") {
            die("--replay-at: not over the link yet (needs the savestate envelope)");
        } else {
            die("unknown argument " + a);
        }
    }
    if (spec.runner.empty() || spec.core.empty() || spec.rom.empty()) {
        die("--runner, --core and --rom are required");
    }
    if (spec.title_dir.empty()) spec.title_dir = ".";
    if (spec.session_dir.empty()) spec.session_dir = ".";

    // Before frame 1, the seats stand as frame 1 will have them.
    if (seat0) {
        spec.initial_pads[0].connected = 1;
        for (const auto& [at, buttons] : script) {
            if (at <= 1) spec.initial_pads[0].buttons = buttons;
        }
    }

    CoreLink link;
    std::string err;
    if (!link.start(spec, &err)) die(err);
    while (link.state() == LinkState::Starting) link.pump(100);
    if (link.state() != LinkState::Ready) {
        die("the runner ended before it was ready (exit " + std::to_string(link.exit_code()) +
            (link.exit_reason().empty() ? "" : ": " + link.exit_reason()) + "); see " +
            link.runner_log().string());
    }
    const CoreIdentity& id = link.identity();
    std::printf("link: %s %s sha256 %s draft %u%s\n", id.core_id.c_str(), id.core_version.c_str(),
                id.sha256.c_str(), id.draft_revision, id.engine_dirty ? " (engine dirty)" : "");

    // ---- the same frames headless mode runs, one grant each -----------------
    bool ok = true;
    for (std::uint64_t k = 1; k <= frames && ok; ++k) {
        rcore_pad pads[RCORE_MAX_SEATS]{};
        for (auto& p : pads) p.struct_size = sizeof(rcore_pad);
        if (seat0) {
            pads[0].connected = 1;
            for (const auto& [at, buttons] : script) {
                if (at <= k) pads[0].buttons = buttons;
            }
        }
        if (!link.grant(pads)) die("grant " + std::to_string(k) + " refused");
        while (link.state() == LinkState::Ready && link.frames_done() < k) link.pump(100);
        if (link.state() != LinkState::Ready) ok = false;
        link.take_frame();
        std::int16_t sink[4096];
        while (link.drain_audio(sink, 2048)) {
        }
    }
    for (const LinkLog& l : link.logs) {
        if (l.level <= RCORE_LOG_WARN) std::fprintf(stderr, "core[%u]: %s\n", l.level, l.text.c_str());
    }
    std::uint32_t faults = 0;
    for (const LinkEvent& e : link.events) {
        if (e.kind == RCORE_EVENT_FAULT) {
            ++faults;
            std::fprintf(stderr, "core FAULT: %s\n", e.detail.c_str());
        }
    }
    link.stop();
    if (link.exit_code() != 0) {
        ok = false;
        std::fprintf(stderr, "retcomm-core-link-test: runner exit %d%s%s\n", link.exit_code(),
                     link.exit_reason().empty() ? "" : ": ", link.exit_reason().c_str());
    }

    // ---- the artifacts headless mode writes ----------------------------------
    if (const FrameInfo* f = link.frame_info()) {
        std::ofstream shot(spec.session_dir / "shot.ppm", std::ios::binary);
        shot << "P6\n" << f->width << ' ' << f->height << "\n255\n";
        const std::uint8_t* px = link.frame_pixels();
        for (std::size_t i = 0; i < std::size_t(f->width) * f->height; ++i) {
            shot.write(reinterpret_cast<const char*>(px + i * 4), 3);
        }
    }
    {
        std::ifstream log(spec.session_dir / "core.log");
        std::ofstream sm(spec.session_dir / "summary.txt");
        std::string line;
        while (std::getline(log, line)) {
            const auto tab = line.find('\t');
            const std::string msg = tab == std::string::npos ? line : line.substr(tab + 1);
            if (!msg.compare(0, 9, "RUN_DONE ") || !msg.compare(0, 11, "RDRAM_HASH ") ||
                !msg.compare(0, 15, "DISPATCH_STATS ")) {
                sm << msg << '\n';
            }
        }
    }
    std::printf("link-test: %llu frame(s) granted and done, runner exit %d, %u fault(s)\n",
                static_cast<unsigned long long>(link.frames_done()), link.exit_code(), faults);
    return (ok && !faults) ? 0 : 1;
}
