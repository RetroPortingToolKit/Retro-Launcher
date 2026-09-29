#include "hub/hub_local_recomp.hpp"

#include "retcomm/hash.hpp"
#include "retcomm/zip_extract.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace retcomm::hub::local_recomp {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// The largest cartridge is 64 MiB; anything claiming more is not one.
constexpr std::uint64_t kMaxRomBytes = 128ull << 20;

std::string generic_label(const fs::path& p) { return p.generic_u8string(); }

} // namespace

// ---- the dumps ---------------------------------------------------------------

bool is_n64_rom_name(const std::string& name) {
    const std::string l = lower(name);
    return ends_with(l, ".z64") || ends_with(l, ".n64") || ends_with(l, ".v64");
}

std::vector<RomCandidate> find_n64_roms(const std::vector<fs::path>& roots,
                                        const std::atomic<bool>* cancel,
                                        std::vector<std::string>* problems) {
    std::vector<RomCandidate> out;
    auto problem = [&](const std::string& m) {
        if (problems) problems->push_back(m);
    };
    for (const fs::path& root : roots) {
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                            ec);
        if (ec) {
            problem("cannot read " + root.string() + ": " + ec.message());
            continue;
        }
        for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                problem("cannot read under " + root.string() + ": " + ec.message());
                break;
            }
            if (cancel && cancel->load()) return out;
            const fs::path& p = it->path();
            const std::string name = p.filename().u8string();
            std::error_code sec;
            if (it->is_directory(sec)) {
                if (!name.empty() && name[0] == '.') it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(sec)) continue;
            const fs::path rel = p.lexically_relative(root);
            if (is_n64_rom_name(name)) {
                out.push_back({p, {}, generic_label(rel), it->file_size(sec)});
            } else if (ends_with(lower(name), ".zip")) {
                std::vector<std::string> entries;
                std::string err;
                if (!zip::list_file(p, &entries, &err)) {
                    problem(p.string() + ": " + err);
                    continue;
                }
                for (const std::string& e : entries) {
                    if (e.empty() || e.back() == '/' || !is_n64_rom_name(e)) continue;
                    out.push_back({p, e, generic_label(rel) + "/" + e, 0});
                }
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const RomCandidate& a, const RomCandidate& b) {
        const std::string la = lower(a.label), lb = lower(b.label);
        return la != lb ? la < lb : a.label < b.label;
    });
    return out;
}

std::string n64_byte_order(const std::string& image) {
    if (image.size() < 4) return {};
    const auto b = [&](size_t i) { return static_cast<unsigned char>(image[i]); };
    const std::uint32_t magic = (std::uint32_t(b(0)) << 24) | (std::uint32_t(b(1)) << 16) |
                                (std::uint32_t(b(2)) << 8) | std::uint32_t(b(3));
    switch (magic) {
    case 0x80371240u: return "z64";
    case 0x37804012u: return "v64";
    case 0x40123780u: return "n64";
    default: return {};
    }
}

bool n64_to_big_endian(std::string& image, std::string* order) {
    const std::string o = n64_byte_order(image);
    if (order) *order = o;
    if (o.empty()) return false;
    if (o == "v64") {
        if (image.size() % 2) return false;
        for (size_t i = 0; i + 1 < image.size(); i += 2) std::swap(image[i], image[i + 1]);
    } else if (o == "n64") {
        if (image.size() % 4) return false;
        for (size_t i = 0; i + 3 < image.size(); i += 4) {
            std::swap(image[i], image[i + 3]);
            std::swap(image[i + 1], image[i + 2]);
        }
    }
    return true;
}

bool stage_rom(const RomCandidate& c, const fs::path& dir, fs::path* out, std::string* sha256,
               std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    std::string image;
    std::string name;
    if (c.entry.empty()) {
        std::error_code ec;
        const auto size = fs::file_size(c.file, ec);
        if (ec) return fail("cannot read " + c.file.string() + ": " + ec.message());
        if (size > kMaxRomBytes)
            return fail(c.file.string() + " is larger than any N64 cartridge");
        std::ifstream in(c.file, std::ios::binary);
        if (!in) return fail("cannot open " + c.file.string());
        image.resize(static_cast<size_t>(size));
        in.read(image.data(), static_cast<std::streamsize>(size));
        if (static_cast<std::uint64_t>(in.gcount()) != size)
            return fail("short read from " + c.file.string());
        name = c.file.filename().u8string();
    } else {
        std::string err;
        if (!zip::read_entry(c.file, c.entry, &image, kMaxRomBytes, &err)) return fail(err);
        name = fs::u8path(c.entry).filename().u8string();
    }
    std::string order;
    if (!n64_to_big_endian(image, &order))
        return fail(c.label + " is not an N64 cartridge image (its first bytes are no N64 "
                              "header in any byte order)");
    const fs::path stem = fs::u8path(name).stem();
    if (stem.empty()) return fail("no file name to stage " + c.label + " under");
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return fail("cannot create " + dir.string() + ": " + ec.message());
    const fs::path dest = dir / (stem.u8string() + ".z64");
    {
        std::ofstream o(dest, std::ios::binary | std::ios::trunc);
        o.write(image.data(), static_cast<std::streamsize>(image.size()));
        if (!o) return fail("cannot write " + dest.string() + " (disk full?)");
    }
    if (out) *out = dest;
    if (sha256) *sha256 = sha256_hex(image);
    return true;
}

// ---- the tools ---------------------------------------------------------------

bool is_n64lle_checkout(const fs::path& dir) {
    std::error_code ec;
    return !dir.empty() &&
           fs::is_regular_file(dir / "tools" / "new_project" / "setup_project.sh", ec);
}

bool is_launcher_checkout(const fs::path& dir) {
    std::error_code ec;
    return !dir.empty() && fs::is_regular_file(dir / "scripts" / "build-local.sh", ec) &&
           fs::is_regular_file(dir / "CMakeLists.txt", ec);
}

bool is_title_app_hub_prefix(const fs::path& dir) {
    std::error_code ec;
    return !dir.empty() && fs::is_regular_file(dir / "retro-hub", ec) &&
           fs::is_regular_file(dir / "retro-core-runner", ec) &&
           fs::is_regular_file(dir / "packaging" / "title" / "build-title-app.sh", ec);
}

fs::path launcher_checkout_above(const fs::path& exe_dir) {
    fs::path d = exe_dir;
    for (int i = 0; i < 5 && !d.empty(); ++i) {
        if (is_launcher_checkout(d)) return d;
        if (d == d.parent_path()) break;
        d = d.parent_path();
    }
    return {};
}

fs::path n64lle_beside(const fs::path& launcher_checkout) {
    if (launcher_checkout.empty()) return {};
    const fs::path p = launcher_checkout.parent_path() / "n64lle";
    return is_n64lle_checkout(p) ? p : fs::path();
}

std::string tools_problem(const Tools& t) {
    if (!is_n64lle_checkout(t.n64lle))
        return t.n64lle.empty()
                   ? "No n64lle source checkout is set. Choose the folder you cloned n64lle into."
                   : t.n64lle.string() + " is not an n64lle source checkout (it has no "
                                         "tools/new_project/setup_project.sh).";
    if (!is_title_app_hub_prefix(t.hub_prefix) && !is_launcher_checkout(t.launcher))
        return "This hub is not in a title-app hub prefix and no Retro-Launcher checkout is "
               "above it, so there is no development hub to build the app with. Run the hub "
               "that scripts/build-local.sh builds.";
    return {};
}

std::vector<std::string> build_local_argv(const fs::path& launcher) {
    return {"bash", (launcher / "scripts" / "build-local.sh").string()};
}

std::string retro_hub_line(const std::string& line) {
    static const std::string kKey = "RETRO_HUB=";
    return line.rfind(kKey, 0) == 0 ? line.substr(kKey.size()) : std::string();
}

std::vector<std::string> scaffold_argv(const fs::path& n64lle, const fs::path& rom,
                                       const fs::path& hub_prefix, const fs::path& dest_dir) {
    return {"bash",
            (n64lle / "tools" / "new_project" / "setup_project.sh").string(),
            rom.string(),
            "--yes",
            "--core", "generate",
            "--runner", (hub_prefix / "retro-core-runner").string(),
            "--hub", (hub_prefix / "retro-hub").string(),
            "--dir", dest_dir.string(),
            "--players", "4",
            "--transfer-pak",
            "--copy-rom",
            "--generate",
            "--app",
            "--git"};
}

// ---- its output --------------------------------------------------------------

std::string strip_ansi(const std::string& line) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
            i += 2;
            while (i < line.size() && !(line[i] >= '@' && line[i] <= '~')) ++i;
            continue;
        }
        if (line[i] == '\r') continue;
        out += line[i];
    }
    return out;
}

std::string step_title(const std::string& line) {
    const std::string s = strip_ansi(line);
    return s.rfind("==> ", 0) == 0 ? s.substr(4) : std::string();
}

fs::path done_root(const std::string& line) {
    const std::string t = step_title(line);
    return t.rfind("Done: ", 0) == 0 ? fs::u8path(t.substr(6)) : fs::path();
}

bool find_generated_app(const fs::path& project, const std::string& rom_file_name,
                        GeneratedApp* out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    const fs::path build = project / "build-release";
    std::error_code ec;
    fs::path app;
    fs::file_time_type newest{};
    for (const auto& e : fs::directory_iterator(build, ec)) {
        std::error_code fec;
        if (!e.is_regular_file(fec)) continue;
        const std::string ext = lower(e.path().extension().string());
        if (ext != ".appimage" && ext != ".exe" && ext != ".dmg") continue;
        const auto t = e.last_write_time(fec);
        if (app.empty() || t > newest) {
            app = e.path();
            newest = t;
        }
    }
    if (ec) return fail("cannot read " + build.string() + ": " + ec.message());
    if (app.empty()) return fail("the scaffold finished but " + build.string() + " holds no app");
    const fs::path title_json = build / "app" / "title" / "title.json";
    if (!fs::is_regular_file(title_json, ec))
        return fail("no staged payload at " + title_json.string());
    GeneratedApp g;
    g.app = app;
    g.title_json = title_json;
    // The scaffold's --copy-rom names its copy after the slug
    // (roms/<slug>.z64), not the dump: the one image in roms/.
    if (!rom_file_name.empty() && fs::is_regular_file(project / "roms" / rom_file_name, ec)) {
        g.rom = project / "roms" / rom_file_name;
    } else {
        for (const auto& e : fs::directory_iterator(project / "roms", ec)) {
            std::error_code fec;
            if (!e.is_regular_file(fec) || lower(e.path().extension().string()) != ".z64")
                continue;
            if (g.rom.empty() || e.path() < g.rom) g.rom = e.path();
        }
    }
    if (out) *out = g;
    return true;
}

// ---- running it --------------------------------------------------------------

int run_process(const std::vector<std::string>& argv, const fs::path& cwd,
                const std::vector<std::pair<std::string, std::string>>& env,
                const std::function<void(const std::string&)>& on_line,
                const std::atomic<bool>* cancel, std::string* error) {
#if defined(_WIN32)
    (void)argv, (void)cwd, (void)env, (void)on_line, (void)cancel;
    if (error) *error = "generating a local recomp is not built for Windows";
    return -1;
#else
    if (argv.empty()) {
        if (error) *error = "nothing to run";
        return -1;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        if (error) *error = std::string("pipe: ") + std::strerror(errno);
        return -1;
    }
    std::vector<std::string> args = argv;
    std::vector<char*> cargv;
    for (auto& a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    const std::string dir = cwd.string();

    const pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = std::string("fork: ") + std::strerror(errno);
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        if (!dir.empty() && chdir(dir.c_str()) != 0) _exit(126);
        for (const auto& [k, v] : env) {
            if (v.empty()) unsetenv(k.c_str());
            else setenv(k.c_str(), v.c_str(), 1);
        }
        execvp(cargv[0], cargv.data());
        dprintf(2, "cannot run %s: %s\n", cargv[0], std::strerror(errno));
        _exit(127);
    }
    setpgid(pid, pid); // both sides, so a cancel right after fork finds the group
    close(fds[1]);

    using clock = std::chrono::steady_clock;
    bool termed = false;
    clock::time_point termed_at{};
    std::string buf;
    char chunk[4096];
    int st = 0;
    bool reaped = false;
    for (;;) {
        if (cancel && cancel->load()) {
            if (!termed) {
                kill(-pid, SIGTERM);
                termed = true;
                termed_at = clock::now();
            } else if (clock::now() - termed_at > std::chrono::seconds(10)) {
                kill(-pid, SIGKILL);
            }
        }
        pollfd p{fds[0], POLLIN, 0};
        // Once the leader is gone, only what is already written is read: a
        // daemon it started (a compiler cache server) may hold the pipe open
        // for as long as it lives.
        const int r = poll(&p, 1, reaped ? 0 : 200);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) {
            if (reaped) break;
            if (waitpid(pid, &st, WNOHANG) == pid) reaped = true;
            continue;
        }
        const ssize_t n = read(fds[0], chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        buf.append(chunk, static_cast<size_t>(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            if (on_line) on_line(buf.substr(0, nl));
            buf.erase(0, nl + 1);
        }
    }
    if (!buf.empty() && on_line) on_line(buf);
    close(fds[0]);
    if (!reaped)
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    // The group may outlive its leader (a build a trap did not wait for).
    if (termed) kill(-pid, SIGKILL);
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return -1;
#endif
}

// ---- one generation, on its own thread ----------------------------------------

namespace {
constexpr size_t kTailLines = 80;

// A hub running from an AppImage carries the mount's libraries and Python in
// its environment; the scaffold's compilers, cmake and python must not see them.
std::vector<std::pair<std::string, std::string>> child_env() {
    std::vector<std::pair<std::string, std::string>> env;
    if (std::getenv("APPIMAGE")) {
        for (const char* k : {"LD_LIBRARY_PATH", "PYTHONHOME", "PYTHONPATH"}) env.emplace_back(k, "");
    }
    return env;
}
} // namespace

Generator::~Generator() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

bool Generator::start(Request r) {
    if (running()) return false;
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_ = Snapshot{};
        snap_.state = State::Running;
        snap_.phase = "Starting";
        snap_.log_path = r.log_path;
    }
    cancel_.store(false);
    started_ = std::chrono::steady_clock::now();
    thread_ = std::thread([this, req = std::move(r)]() mutable { run(std::move(req)); });
    return true;
}

void Generator::cancel() {
    if (running()) cancel_.store(true);
}

bool Generator::running() const {
    std::lock_guard<std::mutex> lock(mu_);
    return snap_.state == State::Running;
}

Generator::Snapshot Generator::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    Snapshot s = snap_;
    if (s.state == State::Running)
        s.elapsed_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    return s;
}

void Generator::reset() {
    if (running()) return;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mu_);
    snap_ = Snapshot{};
}

void Generator::line(const std::string& raw) {
    const std::string s = strip_ansi(raw);
    if (log_) log_ << s << '\n' << std::flush;
    std::lock_guard<std::mutex> lock(mu_);
    snap_.tail.push_back(s);
    if (snap_.tail.size() > kTailLines)
        snap_.tail.erase(snap_.tail.begin(),
                         snap_.tail.begin() + static_cast<long>(snap_.tail.size() - kTailLines));
}

void Generator::set_phase(const std::string& p) {
    std::lock_guard<std::mutex> lock(mu_);
    snap_.phase = p;
}

void Generator::finish(State s, const std::string& error) {
    if (!error.empty()) line("error: " + error);
    log_.close();
    std::lock_guard<std::mutex> lock(mu_);
    snap_.elapsed_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    snap_.state = s;
    snap_.error = error;
}

void Generator::run(Request r) {
    std::error_code ec;
    // The scaffold runs from the n64lle checkout: nothing it is handed may be
    // relative to where the hub started.
    for (fs::path* p : {&r.dest_dir, &r.staging_dir, &r.log_path, &r.rom.file})
        if (!p->empty()) *p = fs::absolute(*p, ec);
    if (!r.log_path.empty()) {
        fs::create_directories(r.log_path.parent_path(), ec);
        log_.open(r.log_path, std::ios::trunc);
    }
    auto cancelled = [&] { return cancel_.load(); };
    auto on_line = [&](const std::string& s) { line(s); };

    // 1. The dump, big-endian, under the name the project is made from.
    set_phase("Reading " + r.rom.label);
    line("dump: " + r.rom.label + " (" + r.rom.file.string() + ")");
    fs::path staged;
    std::string sha, err;
    if (!stage_rom(r.rom, r.staging_dir, &staged, &sha, &err)) return finish(State::Failed, err);
    struct Unstage {
        fs::path p;
        ~Unstage() {
            std::error_code e;
            if (!p.empty()) fs::remove(p, e);
        }
    } unstage{staged};
    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_.sha256 = sha;
    }
    line("sha256 " + sha + ", staged as " + staged.string());
    for (const auto& [have, name] : r.existing) {
        if (have == sha)
            return finish(State::Failed, "this dump is already a local recomp: " + name +
                                             ". Uninstall that one first to generate it again.");
    }
    if (cancelled()) return finish(State::Cancelled, {});

    // 2. A development hub and runner to bundle: this hub's own prefix, else
    // one built from the launcher checkout.
    fs::path prefix = r.tools.hub_prefix;
    if (!is_title_app_hub_prefix(prefix)) {
        set_phase("Building the development hub and runner");
        line("$ scripts/build-local.sh (in " + r.tools.launcher.string() + ")");
        std::string hub_path;
        const int rc = run_process(build_local_argv(r.tools.launcher), r.tools.launcher,
                                   child_env(),
                                   [&](const std::string& s) {
                                       line(s);
                                       const std::string h = retro_hub_line(strip_ansi(s));
                                       if (!h.empty()) hub_path = h;
                                   },
                                   &cancel_, &err);
        if (cancelled()) return finish(State::Cancelled, {});
        if (rc != 0)
            return finish(State::Failed, rc < 0 ? err
                                                : "scripts/build-local.sh failed (exit " +
                                                      std::to_string(rc) + ")");
        prefix = fs::path(hub_path).parent_path();
        if (!is_title_app_hub_prefix(prefix))
            return finish(State::Failed, "scripts/build-local.sh named " + hub_path +
                                             ", which is not a title-app hub prefix");
    }
    line("hub prefix: " + prefix.string());

    // 3. The scaffold: probe, lay out, build the core, harvest, emit, build
    // the game package, gate it, and package the title app.
    set_phase("Starting the n64lle scaffold");
    const std::vector<std::string> argv = scaffold_argv(r.tools.n64lle, staged, prefix, r.dest_dir);
    {
        std::string cmd = "$";
        for (const auto& a : argv) cmd += " " + a;
        line(cmd);
    }
    fs::path root;
    const int rc = run_process(argv, r.tools.n64lle, child_env(),
                               [&](const std::string& s) {
                                   on_line(s);
                                   const std::string t = step_title(s);
                                   if (t.empty()) return;
                                   if (const fs::path d = done_root(s); !d.empty()) root = d;
                                   set_phase(t);
                               },
                               &cancel_, &err);
    if (cancelled())
        return finish(State::Cancelled, {});
    if (rc != 0)
        return finish(State::Failed, rc < 0 ? err
                                            : "the n64lle scaffold failed (exit " +
                                                  std::to_string(rc) +
                                                  "); its reason is in the lines above");
    if (root.empty())
        return finish(State::Failed, "the scaffold exited 0 without naming the project it made");

    GeneratedApp app;
    if (!find_generated_app(root, staged.filename().u8string(), &app, &err))
        return finish(State::Failed, err);
    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_.project = root;
        snap_.app = app;
    }
    line("app: " + app.app.string());
    finish(State::Succeeded, {});
}

} // namespace retcomm::hub::local_recomp
