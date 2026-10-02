// Two seats, two games, one match: docs/NETPLAY_HANDOFF.md end to end.
//
// Does what two hubs do, with the hub's own code -- LobbyClient for the seats,
// handoff_settle_request / handoff_record for what the games are given -- and
// calls each game's executable the way launch_title does for a library game
// (--launcher --netplay-query, then --launcher with RECOMP_NETPLAY_LAUNCH).
// Each game runs from its own directory, as two installs on two machines
// would, offscreen and with no audio device. Run it inside a loopback-only network namespace with a local lobby
// server: nothing here should reach a real router or the public server.
//
//   retcomm-netplay-handoff-drive --url ws://127.0.0.1:18765 \
//       --host-dir A --guest-dir B --exe Game --disc game.cue \
//       --game GAME_NAME --version VER --seconds 30 --out OUT
//
// Exit 0 when both games took the match (their .status files say ok) and were
// still running after --seconds; their logs are in OUT.

#include "retcomm/netplay_client.hpp"
#include "retcomm/netplay_handoff.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace np = retcomm::netplay;
using json = nlohmann::json;

namespace {

struct Args {
    std::string url, host_dir, guest_dir, exe, disc, game, version, out;
    int seconds = 30;
};

void say(const std::string& s) {
    std::fprintf(stderr, "drive: %s\n", s.c_str());
}

bool wait_for(const std::function<bool()>& ok, double seconds, const std::string& what) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (ok()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    say("timed out waiting for " + what);
    return false;
}

// fork/exec in `dir`, stdout+stderr to `log`, with `env` added.
pid_t spawn(const std::string& dir, const std::vector<std::string>& argv,
            const std::vector<std::string>& env, const std::string& log) {
    const pid_t pid = fork();
    if (pid != 0) return pid;
    if (chdir(dir.c_str()) != 0) _exit(126);
    const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
    }
    for (const std::string& e : env) putenv(const_cast<char*>(e.c_str()));
    std::vector<char*> a;
    for (const std::string& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    execv(a[0], a.data());
    _exit(127);
}

json read_json(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        return json::parse(ss.str());
    } catch (...) {
        return json();
    }
}

// The game's --netplay-query, as run_handoff_query runs it.
json query(const Args& a, const std::string& dir, const json& req, const std::string& tag) {
    const fs::path rq = fs::path(a.out) / (tag + ".json");
    std::ofstream(rq) << req.dump();
    const std::string exe = (fs::path(dir) / a.exe).string();
    const pid_t pid = spawn(dir, {exe, "--launcher", "--netplay-query", rq.string()},
                            {"SDL_VIDEO_DRIVER=offscreen", "SDL_AUDIO_DRIVER=dummy"},
                            (fs::path(a.out) / (tag + ".log")).string());
    int st = 0;
    waitpid(pid, &st, 0);
    const json ans = read_json(fs::path(rq.string() + ".answer"));
    say(tag + ": " + (ans.is_object() ? ans.dump().substr(0, 300) : std::string("no answer")));
    return ans;
}

const np::Member* local_member(const np::Snapshot& s) {
    for (const np::Member& m : s.members)
        if (m.is_local) return &m;
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--url") a.url = v;
        else if (k == "--host-dir") a.host_dir = v;
        else if (k == "--guest-dir") a.guest_dir = v;
        else if (k == "--exe") a.exe = v;
        else if (k == "--disc") a.disc = v;
        else if (k == "--game") a.game = v;
        else if (k == "--version") a.version = v;
        else if (k == "--seconds") a.seconds = std::atoi(v.c_str());
        else if (k == "--out") a.out = v;
    }
    fs::create_directories(a.out);
    std::signal(SIGPIPE, SIG_IGN);

    // 1. Each game says what its own lobby would send.
    json idreq = {{"v", 1}, {"handoff_query", "identity"}};
    if (!a.disc.empty()) idreq["handoff_disc"] = a.disc;
    const json id_host = query(a, a.host_dir, idreq, "identity-host");
    const json id_guest = query(a, a.guest_dir, idreq, "identity-guest");
    if (!id_host.is_object() || !id_guest.is_object() || id_host.contains("error")) return 2;

    // 2. Two seats.
    np::LobbyClient host, guest;
    np::LobbyConfig hc{a.url, "", "Host", a.game, a.version, ""};
    np::LobbyConfig gc{a.url, "", "Guest", a.game, a.version, ""};
    host.start(hc);
    guest.start(gc);
    if (!wait_for([&] { return host.snapshot().state == np::ConnState::Connected &&
                               guest.snapshot().state == np::ConnState::Connected; },
                  10, "both connections"))
        return 3;
    json caps = json::object();
    if (id_host.contains("start") && id_host["start"].contains("match_caps"))
        caps = id_host["start"]["match_caps"];
    host.create_for(a.game, a.version, "handoff e2e", "", 2, false, caps,
                    id_host.value("join_fields", json::object()));
    if (!wait_for([&] { return host.snapshot().in_room; }, 10, "the host's room")) return 4;
    const std::string lobby = host.snapshot().lobby_id;
    say("room " + lobby);
    guest.join_as(lobby, "", a.game, a.version, id_guest.value("join_fields", json::object()));
    if (!wait_for([&] { return guest.snapshot().in_room; }, 10, "the guest's seat")) {
        say("guest error: " + guest.snapshot().last_error_code);
        return 5;
    }

    // 3. Ready, with each game's own offer.
    host.send_raw(id_host["ready"]);
    guest.send_raw(id_guest["ready"]);
    if (!wait_for([&] {
            const np::Snapshot s = host.snapshot();
            if (s.members.size() < 2) return false;
            for (const np::Member& m : s.members)
                if (!m.ready) return false;
            return true;
        }, 10, "both seats ready"))
        return 6;

    // 4. The host's PLAY: the host's game settles, its start is sent as is.
    const json settle = query(a, a.host_dir, np::handoff_settle_request(host.snapshot()), "settle");
    if (!settle.contains("start")) {
        say("settle refused: " + settle.value("error", std::string("no answer")));
        return 7;
    }
    host.send_raw(settle["start"]);
    if (!wait_for([&] { return host.snapshot().launch_pending && guest.snapshot().launch_pending; },
                  10, "the launch"))
        return 8;

    // 5. Each seat's game takes the match from its record.
    struct Seat {
        std::string tag, dir;
        np::Snapshot snap;
        fs::path record;
        pid_t pid = -1;
    };
    Seat seats[2] = {{"host", a.host_dir, host.snapshot(), {}, -1},
                     {"guest", a.guest_dir, guest.snapshot(), {}, -1}};
    host.ack_launch();
    guest.ack_launch();
    for (Seat& s : seats) {
        s.record = fs::absolute(fs::path(a.out) / ("launch-" + s.tag + ".json"));
        std::ofstream(s.record) << np::handoff_record(s.snap).dump();
        fs::remove(s.record.string() + ".status");
        const std::string exe = (fs::path(s.dir) / a.exe).string();
        s.pid = spawn(s.dir, {exe, "--launcher"},
                      {"SDL_VIDEO_DRIVER=offscreen", "SDL_AUDIO_DRIVER=dummy",
                       "RECOMP_NETPLAY_LAUNCH=" + s.record.string()},
                      (fs::path(a.out) / (s.tag + ".log")).string());
        say(s.tag + " game pid " + std::to_string(s.pid));
    }
    int rc = 0;
    for (Seat& s : seats) {
        if (!wait_for([&] { return fs::exists(s.record.string() + ".status"); }, 30,
                      s.tag + "'s status"))
            rc = 9;
        else
            say(s.tag + " status: " + read_json(s.record.string() + ".status").dump());
    }
    // 6. Free-run (never pause a peer), then check both are still playing.
    std::this_thread::sleep_for(std::chrono::seconds(a.seconds));
    for (Seat& s : seats) {
        int st = 0;
        if (waitpid(s.pid, &st, WNOHANG) != 0) {
            say(s.tag + " game exited early (status " + std::to_string(st) + ")");
            rc = rc ? rc : 10;
        }
    }
    for (Seat& s : seats) {
        kill(s.pid, SIGTERM);
        int st = 0;
        waitpid(s.pid, &st, 0);
    }
    host.stop();
    guest.stop();
    say(rc == 0 ? "both games took the match and ran" : "FAILED rc=" + std::to_string(rc));
    return rc;
}
