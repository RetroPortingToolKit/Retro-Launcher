// retcomm-core-runner -- the child process that runs an rcore core for the
// Retro frontend (docs/HOST_LIFECYCLE.md, docs/CORE_ABI.md).
//
// This first cut is the HEADLESS mode: load a core, check its sidecar
// manifest against the library's own info, run N frames, and write what the
// n64lle parity gate compares. Its command line and outputs match n64lle's
// rcore_probe, so tools/rust_parity/core_parity.sh grades this runner with
// RCORE_PROBE=<this binary> and no other change. A runner that matches the
// goldens the SDL harness matches is the same machine. The hub link (shared
// memory, docs/HOST_LIFECYCLE.md §4) is the next mode; HostSession is shared.
//
// Outputs in --out:
//   state_hash.tsv  written by the core itself into cache_dir
//                   (option developer.state_hash_every)
//   shot.ppm        the last submitted frame (P6, RGB)
//   summary.txt     the RUN_DONE / RDRAM_HASH / DISPATCH_STATS lines the core
//                   logged
//   events.tsv      every report(): BRIDGE / DISPATCH_MISS / FAULT
//   core.log        every log() line
//
// Exit codes: 0 ok; 1 a frame failed or the core reported a FAULT; 2 a
// refusal before the core ran (bad arguments, load, ABI, manifest); 3 the
// core broke the contract (an undeclared option key).

#include "core_library.hpp"
#include "core_manifest.hpp"
#include "host_session.hpp"
#include "runner_link.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(RETCOMM_RUNNER_HAVE_SDL3)
#  include <SDL3/SDL.h>
#endif

using namespace retcomm::runner;

namespace {

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "retcomm-core-runner: %s\n", msg.c_str());
    std::exit(2);
}

std::uint32_t script_buttons(const std::string& s) {
    std::uint32_t b = 0;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t plus = s.find('+', start);
        const std::string tok = s.substr(start, plus == std::string::npos ? std::string::npos
                                                                          : plus - start);
        if (tok.empty() || tok == "-" || tok == "none") {
        } else if (tok == "a") b |= RCORE_PAD_SOUTH;
        else if (tok == "b") b |= RCORE_PAD_WEST;
        else if (tok == "z") b |= RCORE_PAD_L2;
        else if (tok == "l") b |= RCORE_PAD_L1;
        else if (tok == "r") b |= RCORE_PAD_R1;
        else if (tok == "start") b |= RCORE_PAD_START;
        else if (tok == "dup") b |= RCORE_PAD_DPAD_UP;
        else if (tok == "ddown") b |= RCORE_PAD_DPAD_DOWN;
        else if (tok == "dleft") b |= RCORE_PAD_DPAD_LEFT;
        else if (tok == "dright") b |= RCORE_PAD_DPAD_RIGHT;
        else die("--input-script: unknown button '" + tok + "'");
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return b;
}

// Headless: everything to files, input from a script on seat 0.
class HeadlessSink final : public Sink {
public:
    HeadlessSink(const fs::path& out, bool seat0, std::vector<std::pair<std::uint64_t, std::uint32_t>> script,
                 const std::uint64_t* frame)
        : seat0_(seat0), script_(std::move(script)), frame_(frame),
          log_(out / "core.log"), events_(out / "events.tsv") {}

    void log(std::uint32_t level, const char* msg) override {
        log_ << level << '\t' << msg << '\n';
        if (!std::strncmp(msg, "RUN_DONE ", 9) || !std::strncmp(msg, "RDRAM_HASH ", 11) ||
            !std::strncmp(msg, "DISPATCH_STATS ", 15)) {
            summary.emplace_back(msg);
        }
        if (level <= RCORE_LOG_WARN) std::fprintf(stderr, "core[%u]: %s\n", level, msg);
    }

    void event(const rcore_event& e) override {
        const char* kind = e.kind == RCORE_EVENT_DISPATCH_MISS ? "DISPATCH_MISS"
                           : e.kind == RCORE_EVENT_BRIDGE      ? "BRIDGE"
                           : e.kind == RCORE_EVENT_FAULT       ? "FAULT"
                                                               : "?";
        const char* d = e.detail ? e.detail : "";
        if (e.kind == RCORE_EVENT_FAULT) {
            ++faults;
            std::fprintf(stderr, "core FAULT: %s\n", d);
        }
        if (e.kind == RCORE_EVENT_BRIDGE) ++bridges;
        char addr[24]; // "0x" + 16 hex digits: guest addresses arrive sign-extended
        std::snprintf(addr, sizeof addr, "0x%08llx",
                      static_cast<unsigned long long>(e.guest_address));
        events_ << kind << '\t' << e.frame_number << '\t' << addr << '\t' << d << '\n';
    }

    void frame(const rcore_frame& f) override {
        if (f.pixel_format != RCORE_PIXEL_RGBA8) die("the core submitted a pixel format this runner does not know");
        const auto* rows = static_cast<const std::uint8_t*>(f.pixels);
        last.clear();
        for (std::uint32_t y = 0; y < f.height; ++y) {
            last.insert(last.end(), rows + size_t(y) * f.stride,
                        rows + size_t(y) * f.stride + size_t(f.width) * 4);
        }
        last_w = f.width;
        last_h = f.height;
        ++frames_seen;
    }

    void audio(const std::int16_t*, std::uint32_t n) override { audio_frames += n; }
    void audio_rate(std::uint32_t hz) override { audio_hz = hz; }
    void frame_rate(std::uint32_t num, std::uint32_t den) override {
        if (num == rate_num && den == rate_den) return;
        rate_num = num;
        rate_den = den;
        if (num) {
            std::printf("frame rate: %u/%u (%.4f Hz)\n", num, den, double(num) / den);
        } else {
            std::printf("frame rate: withdrawn\n");
        }
    }

    void input(std::uint32_t seat, rcore_pad& pad) override {
        if (seat != 0 || !seat0_) return; // other seats: no controller
        // A controller in port 1 that nobody touches unless the script says.
        pad.connected = 1;
        std::uint32_t held = 0;
        for (const auto& [at, buttons] : script_) {
            if (at <= *frame_) held = buttons;
        }
        pad.buttons = held;
    }

    std::vector<std::string> summary;
    std::vector<std::uint8_t> last;
    std::uint32_t last_w = 0, last_h = 0;
    std::uint64_t frames_seen = 0, audio_frames = 0;
    std::uint32_t audio_hz = 0, faults = 0, bridges = 0;
    std::uint32_t rate_num = 0, rate_den = 0;

private:
    bool seat0_;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> script_;
    const std::uint64_t* frame_;
    std::ofstream log_, events_;
};

void write_ppm(const fs::path& path, const std::vector<std::uint8_t>& rgba, std::uint32_t w,
               std::uint32_t h) {
    std::ofstream f(path, std::ios::binary);
    if (!f) die(path.string() + ": cannot write");
    f << "P6\n" << w << ' ' << h << "\n255\n";
    for (size_t i = 0; i + 4 <= rgba.size(); i += 4) {
        f.write(reinterpret_cast<const char*>(&rgba[i]), 3);
    }
}

#if defined(RETCOMM_RUNNER_HAVE_SDL3)
void* sdl_gl_proc(const char* name) {
    return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
}

// A hidden window's GL context, current on this thread for every core call.
void lend_gl_context(HostSession& session) {
    if (!SDL_Init(SDL_INIT_VIDEO)) die(std::string("SDL_Init: ") + SDL_GetError());
    SDL_Window* win =
        SDL_CreateWindow("retcomm-core-runner", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) die(std::string("SDL_CreateWindow: ") + SDL_GetError());
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx || !SDL_GL_MakeCurrent(win, ctx)) die(std::string("GL context: ") + SDL_GetError());
    session.lend_gl(sdl_gl_proc);
    std::printf("gl: lent a context (hidden SDL window, driver %s)\n", SDL_GetCurrentVideoDriver());
}
#endif

} // namespace

int main(int argc, char** argv) {
    std::string core_path, rom, title_dir = ".";
    fs::path out = ".";
    std::uint64_t frames = 60;
    std::optional<fs::path> load_state, tpak_save, tpak_rtc;
    std::string tpak_rom;
    bool gl = false, strict = false, seat0 = true, list_options = false, link = false;
    std::optional<std::uint64_t> replay_at;
    std::map<std::string, std::string> overrides;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> script;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) die(a + " needs a value");
            return argv[++i];
        };
        auto num = [&](const std::string& v) -> std::uint64_t {
            char* end = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &end, 10);
            if (v.empty() || *end) die(a + ": not a number: " + v);
            return n;
        };
        if (a == "--core") core_path = val();
        else if (a == "--rom") rom = val();
        else if (a == "--title-dir") title_dir = val();
        else if (a == "--out") out = val();
        else if (a == "--frames") frames = num(val());
        else if (a == "--load-state") load_state = fs::path(val());
        else if (a == "--tpak1-rom") tpak_rom = val();
        else if (a == "--tpak1-save") tpak_save = fs::path(val());
        else if (a == "--tpak1-rtc") tpak_rtc = fs::path(val());
        else if (a == "--gl") gl = true;
        else if (a == "--link") link = true;
        else if (a == "--strict") strict = true;
        else if (a == "--no-seats") seat0 = false;
        else if (a == "--list-options") list_options = true;
        else if (a == "--replay-at") replay_at = num(val());
        else if (a == "--opt") {
            const std::string kv = val();
            const auto eq = kv.find('=');
            if (eq == std::string::npos) die("--opt key=value");
            overrides[kv.substr(0, eq)] = kv.substr(eq + 1);
        } else if (a == "--input-script") {
            const std::string s = val();
            size_t start = 0;
            while (start <= s.size()) {
                const size_t comma = s.find(',', start);
                const std::string ev = s.substr(start, comma == std::string::npos
                                                           ? std::string::npos
                                                           : comma - start);
                const auto colon = ev.find(':');
                if (colon == std::string::npos) die("--input-script F:buttons,...");
                script.emplace_back(num(ev.substr(0, colon)), script_buttons(ev.substr(colon + 1)));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else {
            die("unknown argument " + a);
        }
    }
    if (core_path.empty()) die("--core <library> is required");
    if (rom.empty()) die("--rom <image> is required");
    std::error_code ec;
    fs::create_directories(out, ec);

    // ---- load: hash the file that is loaded, then the one symbol ---------
    LoadedCore core;
    std::string err;
    if (!load_core(core_path, core, &err)) die(err);
    const rcore_core_info& info = *core.info;
    std::printf("core: %s %s platforms=%s capabilities=0x%llx state_compat_id=%s\n",
                info.core_id, info.core_version, info.platforms,
                static_cast<unsigned long long>(info.capabilities),
                info.state_compat_id ? info.state_compat_id : "NULL");
    std::printf("identity: sha256 %s\n", core.sha256.c_str());

    // ---- the sidecar must agree with the library, field for field --------
    CoreManifest manifest;
    if (!read_manifest(manifest_path_for(core.path), manifest, &err)) die(err);
    const auto diffs = verify_manifest(manifest, core);
    if (!diffs.empty()) {
        std::fprintf(stderr, "retcomm-core-runner: %s disagrees with the library it describes:\n",
                     manifest.path.string().c_str());
        for (const auto& d : diffs) std::fprintf(stderr, "  %s\n", d.c_str());
        return 2;
    }
    std::printf("manifest: %s agrees (draft revision %ld; runner %u)%s\n",
                manifest.path.filename().string().c_str(), manifest.draft_revision,
                RCORE_DRAFT_REVISION, manifest.engine_dirty ? "; engine DIRTY" : "");
    if (!(info.capabilities & RCORE_CAP_RUN_FRAME) || !core.api->run_frame) {
        die("this runner drives RUN_FRAME cores only; the core declares none");
    }

    // ---- link mode: the hub drives the session -----------------------------
    if (link) {
#if !defined(RETCOMM_RUNNER_HAVE_SDL3)
        if (gl) die("--gl: this runner was built without SDL3, so it has no GL context to lend");
        void (*lend)(HostSession&) = nullptr;
#else
        void (*lend)(HostSession&) = lend_gl_context;
#endif
        LinkArgs la;
        la.rom = rom;
        la.title_dir = title_dir;
        la.out = out;
        la.gl = gl;
        la.strict = strict;
        la.overrides = overrides;
        la.load_state = load_state;
        la.tpak_rom = tpak_rom;
        std::fflush(stdout);
        return run_link_mode(core, manifest, la, lend);
    }

    // ---- the session ------------------------------------------------------
    std::uint64_t frame = 0; // the frame being run, 1-based; the input script reads it
    HeadlessSink sink(out, seat0, std::move(script), &frame);
    HostSession session(core, sink);
    if (!session.set_options(overrides, &err)) die(err);
    for (const auto& [k, v] : session.options()) {
        if (v) std::printf("option %s = %s\n", k.c_str(), v->c_str());
        else if (list_options) std::printf("option %s unset\n", k.c_str());
    }
    if (gl) {
#if defined(RETCOMM_RUNNER_HAVE_SDL3)
        lend_gl_context(session);
#else
        die("--gl: this runner was built without SDL3, so it has no GL context to lend");
#endif
    }

    const std::string cache = out.string();
    rcore_init_params ip{};
    ip.struct_size = sizeof ip;
    ip.flags = strict ? RCORE_INIT_STRICT : 0u;
    ip.system_dir = nullptr;
    ip.cache_dir = cache.c_str();
    if (const rcore_result rc = core.api->init(session.host_api(), &ip); rc != RCORE_OK) {
        die("init -> " + std::to_string(rc));
    }

    // ---- load: content, the Transfer Pak binding, host-owned saves -------
    std::vector<rcore_accessory_binding> bindings;
    if (!tpak_rom.empty()) {
        rcore_accessory_binding b{};
        b.struct_size = sizeof b;
        b.seat = 0;
        b.slot = 0;
        b.type_id = "n64.transfer_pak";
        b.content_path = tpak_rom.c_str();
        b.content_sha256 = nullptr;
        bindings.push_back(b);
    }
    rcore_load_params lp{};
    lp.struct_size = sizeof lp;
    lp.content_path = rom.c_str();
    lp.content_sha256 = nullptr;
    lp.package_path = nullptr;
    lp.title_dir = title_dir.c_str();
    lp.accessories = bindings.empty() ? nullptr : bindings.data();
    lp.accessory_count = static_cast<std::uint32_t>(bindings.size());
    const rcore_save_region* regs = nullptr;
    std::uint32_t nregs = 0;
    if (const rcore_result rc = core.api->load(&lp, &regs, &nregs); rc != RCORE_OK) {
        die("load -> " + std::to_string(rc));
    }
    std::map<std::string, fs::path> save_files;
    if (tpak_save) save_files["tpak1"] = *tpak_save;
    if (tpak_rtc) save_files["tpak1.rtc"] = *tpak_rtc;
    session.adopt_save_regions(regs, nregs, save_files);
    for (const SaveRegion& r : session.save_regions()) {
        std::printf("save region %s: kind=%u seat=%u size=%zu from %s\n", r.id.c_str(), r.kind,
                    r.seat, r.size, r.file ? r.file->string().c_str() : "(none)");
    }

    if (load_state) {
        std::ifstream in(*load_state, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
        if (!in && bytes.empty()) die(load_state->string() + ": unreadable");
        if (!core.api->unserialize ||
            core.api->unserialize(bytes.data(), bytes.size()) != RCORE_OK) {
            die("unserialize " + load_state->string() + " failed");
        }
    }

    // ---- run --------------------------------------------------------------
    auto run = [&](std::uint64_t from, std::uint64_t to) {
        for (std::uint64_t k = from; k <= to; ++k) {
            frame = k;
            if (const rcore_result rc = core.api->run_frame(); rc != RCORE_OK) {
                std::fprintf(stderr, "retcomm-core-runner: run_frame %llu -> %d\n",
                             static_cast<unsigned long long>(k), rc);
                return false;
            }
        }
        return true;
    };
    bool ok;
    std::string replay_verdict;
    if (!replay_at) {
        ok = run(1, frames);
    } else {
        // DETERMINISTIC + SAVESTATE: serialize after frame K, run to N,
        // restore, run K+1..N again; the second pass must be the first.
        const std::uint64_t k = *replay_at;
        ok = run(1, k);
        const std::uint64_t size = core.api->serialize_size ? core.api->serialize_size() : 0;
        std::vector<std::uint8_t> state(size);
        if (!size || core.api->serialize(state.data(), size) != RCORE_OK) die("serialize failed");
        ok = ok && run(k + 1, frames);
        const std::vector<std::uint8_t> first = sink.last;
        if (core.api->unserialize(state.data(), size) != RCORE_OK) die("unserialize (replay) failed");
        ok = ok && run(k + 1, frames);
        replay_verdict = "REPLAY serialized at frame " + std::to_string(k) + " (" +
                         std::to_string(size) + " bytes), ran to " + std::to_string(frames) +
                         " twice; final frame " + (first == sink.last ? "IDENTICAL" : "DIFFERS");
        std::printf("%s\n", replay_verdict.c_str());
    }

    core.api->unload();
    write_ppm(out / "shot.ppm", sink.last, sink.last_w, sink.last_h);
    session.persist_save_regions();
    {
        std::ofstream sm(out / "summary.txt");
        for (const auto& l : sink.summary) sm << l << '\n';
    }
    std::printf("runner: %llu frame(s) submitted, %llu audio frame(s) at %u Hz, frame rate %s, "
                "%u bridge event(s), %u fault(s)%s%s\n",
                static_cast<unsigned long long>(sink.frames_seen),
                static_cast<unsigned long long>(sink.audio_frames), sink.audio_hz,
                sink.rate_num ? (std::to_string(sink.rate_num) + "/" + std::to_string(sink.rate_den)).c_str()
                              : "unstated",
                sink.bridges, sink.faults, replay_verdict.empty() ? "" : "; ",
                replay_verdict.c_str());
    core.api->deinit();
    return (!ok || sink.faults) ? 1 : 0;
}
