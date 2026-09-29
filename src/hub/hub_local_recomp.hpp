#pragma once

// "Generate Local Recomp" (docs/LOCAL_RECOMP.md): the drawer's way to turn one
// of the player's own N64 dumps into a title app, on this machine, through
// n64lle's new-project scaffold -- then register it as a core title
// (core_titles.hpp), so it is a row in the library like any other.
//
// Development builds only (the CI release build has no such button, and no
// Nintendo 64 platform at all): the scaffold runs from an n64lle SOURCE
// checkout, and the core, runner and hub it bundles are this machine's dev
// builds -- n64lle's `--core generate`, and the flat hub prefix this hub
// came from or that Retro-Launcher's scripts/build-local.sh makes.
//
// Everything here needs no window, so the unit test runs it headless:
//   - finding N64 dumps under the platform's library folders, zips included;
//   - staging the chosen one as a big-endian .z64 (its sha256 is the title's
//     identity);
//   - where the n64lle checkout, the launcher source and the dev hub are;
//   - the scaffold's command line, and reading its output;
//   - running it in its own process group, so Cancel stops every child.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace retcomm::hub::local_recomp {

namespace fs = std::filesystem;

// ---- the dumps ---------------------------------------------------------------

// One dump the picker offers: a file, or one entry inside a zip.
struct RomCandidate {
    fs::path file;      // on disk; the .zip itself for an entry
    std::string entry;  // the zip entry's stored name; empty for a plain file
    std::string label;  // the path under its library folder, "<zip>/<entry>" inside a zip
    std::uint64_t size = 0;
};

// .z64 .n64 .v64, any case.
bool is_n64_rom_name(const std::string& name);

// Every N64 dump under `roots`, recursively, sorted by label. Zips are opened
// and each N64 entry offered. Folders starting with '.' are skipped, and a
// folder that cannot be read is a line in *problems, not a failure.
std::vector<RomCandidate> find_n64_roms(const std::vector<fs::path>& roots,
                                        const std::atomic<bool>* cancel,
                                        std::vector<std::string>* problems);

// "z64" (big-endian), "v64" (byte-swapped), "n64" (word-swapped), or empty
// when the first four bytes are not an N64 image's.
std::string n64_byte_order(const std::string& image);
// In place, to big-endian. False (and nothing changed) when it is not an N64
// image, or a swapped image's length does not divide into its unit.
bool n64_to_big_endian(std::string& image, std::string* order);

// The candidate's image, big-endian, written to <dir>/<its stem>.z64 (the
// scaffold names the project from that file name). *sha256 is the digest of
// what was written, lowercase: the title's ROM identity.
bool stage_rom(const RomCandidate& c, const fs::path& dir, fs::path* out, std::string* sha256,
               std::string* error);

// ---- the tools ---------------------------------------------------------------

// n64lle: tools/new_project/setup_project.sh.
bool is_n64lle_checkout(const fs::path& dir);
// Retro-Launcher: scripts/build-local.sh beside a CMakeLists.txt.
bool is_launcher_checkout(const fs::path& dir);
// A flat hub prefix that can package a title app: retro-hub, retro-core-runner
// and packaging/title/build-title-app.sh (scripts/build-local.sh's output).
bool is_title_app_hub_prefix(const fs::path& dir);

// The Retro-Launcher checkout this hub was built from: the nearest folder
// above `exe_dir` (at most four up) that is one. Empty when there is none --
// an installed hub.
fs::path launcher_checkout_above(const fs::path& exe_dir);
// n64lle beside that checkout (<parent>/n64lle), when it is one.
fs::path n64lle_beside(const fs::path& launcher_checkout);

struct Tools {
    fs::path n64lle;    // the checkout the scaffold runs from
    fs::path hub_prefix; // a title-app hub prefix, when this hub is in one
    fs::path launcher;  // else: the checkout scripts/build-local.sh makes one from
};

// What is missing, one sentence, or empty when the tools can run.
std::string tools_problem(const Tools& t);

// scripts/build-local.sh, run from the launcher checkout.
std::vector<std::string> build_local_argv(const fs::path& launcher);
// Its last `RETRO_HUB=<path>` line, from a line of its output; empty otherwise.
std::string retro_hub_line(const std::string& line);

// The scaffold, non-interactive: the dev core built from the checkout
// (--core generate), the runner and hub of `hub_prefix`, every controller port
// plugged in with the Transfer Pak supported (the other paks follow the
// cartridge's own default), the game package, its gates and the title app
// built, a local git repository and no GitHub.
std::vector<std::string> scaffold_argv(const fs::path& n64lle, const fs::path& rom,
                                       const fs::path& hub_prefix, const fs::path& dest_dir);

// ---- its output --------------------------------------------------------------

// Without terminal colour codes.
std::string strip_ansi(const std::string& line);
// "==> Probing x" -> "Probing x"; empty for any other line.
std::string step_title(const std::string& line);
// The project a scaffold made, from its "==> Done: <root>" line; empty otherwise.
fs::path done_root(const std::string& line);

// What a finished scaffold left: the title app at the root of build-release/
// (the newest, when there are several), its payload's title.json, and the
// project's copy of the dump (roms/<rom_file_name>, else the .z64 in roms/:
// the scaffold names its copy after the slug).
struct GeneratedApp {
    fs::path app, title_json, rom;
};
bool find_generated_app(const fs::path& project, const std::string& rom_file_name,
                        GeneratedApp* out, std::string* error);

// ---- running it --------------------------------------------------------------

// argv[0] is looked up on PATH. stdin is /dev/null; stdout and stderr arrive
// together, a line at a time, without their newline. The child leads its own
// process group, and *cancel (checked while it runs) signals that whole group
// -- SIGTERM, then SIGKILL after a grace period -- so nothing it started is
// left running. Returns the exit status; 128+signal when it was killed, -1
// when it could not start (*error says why).
int run_process(const std::vector<std::string>& argv, const fs::path& cwd,
                const std::vector<std::pair<std::string, std::string>>& env,
                const std::function<void(const std::string&)>& on_line,
                const std::atomic<bool>* cancel, std::string* error);

// ---- one generation, on its own thread ----------------------------------------

struct Request {
    RomCandidate rom;
    Tools tools;
    fs::path dest_dir;    // where the project folder is made (an install root)
    fs::path staging_dir; // the staged .z64 lives here until the scaffold copies it
    fs::path log_path;    // every line, for after the fact
    // Titles already registered, by ROM sha256: generating one again is refused
    // before anything is built.
    std::vector<std::pair<std::string, std::string>> existing; // sha256, name
};

class Generator {
public:
    enum class State { Idle, Running, Succeeded, Failed, Cancelled };
    struct Snapshot {
        State state = State::Idle;
        std::string phase;              // the step running now
        std::vector<std::string> tail;  // the last lines of output
        std::string error;              // Failed: why
        fs::path project;               // Succeeded: the project folder
        GeneratedApp app;               // Succeeded: its app, payload and ROM copy
        std::string sha256;             // the dump's
        double elapsed_s = 0;
        fs::path log_path;
    };

    Generator() = default;
    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;
    ~Generator(); // cancels a running generation and waits for it

    // False when one is already running.
    bool start(Request r);
    void cancel();
    bool running() const;
    Snapshot snapshot() const;
    // Back to Idle, after a result has been taken. Not while running.
    void reset();

private:
    void run(Request r);
    void line(const std::string& s);
    void set_phase(const std::string& p);
    void finish(State s, const std::string& error);

    mutable std::mutex mu_;
    Snapshot snap_;
    std::chrono::steady_clock::time_point started_{};
    std::atomic<bool> cancel_{false};
    std::thread thread_;
    std::ofstream log_;
};

} // namespace retcomm::hub::local_recomp
