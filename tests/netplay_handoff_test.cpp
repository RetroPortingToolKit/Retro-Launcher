// docs/NETPLAY_HANDOFF.md, the hub's windowless half: the launch record and
// settle request carry the server's messages verbatim under the handoff_ keys,
// the game's status file reads back, and a build is recognised as one that
// takes hub matches from its executable alone.

#include "retcomm/launch.hpp"
#include "retcomm/netplay_handoff.hpp"

#include <cstdio>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace np = retcomm::netplay;
using json = nlohmann::json;

static int failures = 0;
static void require(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

int main(int argc, char** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "np_handoff";
    fs::remove_all(dir);
    fs::create_directories(dir);

    np::Snapshot s;
    s.player_id = "p-guest";
    s.lobby_url = "ws://lobby.example:8765";
    s.seat_msg = json::parse(R"({"op":"joined","ok":true,"lobby_id":"L1","local_slot":1,
                                 "session_id":7,"host_endpoint":"","guest_endpoint":""})");
    s.room_msg = json::parse(R"({"op":"lobby_update","lobby_id":"L1","name":"a {brace} room",
                                 "host_player_id":"p-host","slots":[]})");
    s.launch = json::parse(R"({"op":"launch","session_id":4242,"relay_endpoint":"203.0.113.9:7000"})");

    // The settle request: who we are, and the seating messages, as received.
    const json req = np::handoff_settle_request(s);
    require(req.value("handoff_query", "") == "settle", "settle request query");
    require(req.value("handoff_player_id", "") == "p-guest", "settle request player");
    require(req.value("handoff_lobby_url", "") == "ws://lobby.example:8765", "settle request url");
    require(req["handoff_seat"] == s.seat_msg, "settle request seat verbatim");
    require(req["handoff_room"] == s.room_msg, "settle request room verbatim");
    require(!req.contains("handoff_launch"), "settle request has no launch");

    // The record: the same, plus the launch, and no op.
    const json rec = np::handoff_record(s);
    require(!rec.contains("handoff_query"), "record is not a query");
    require(rec["handoff_launch"] == s.launch, "record launch verbatim");
    require(rec["handoff_room"]["name"] == "a {brace} room", "record keeps names as they are");
    // No key a server message uses may be mistaken for a record key: every
    // top-level key is the version or handoff_-prefixed.
    for (auto it = rec.begin(); it != rec.end(); ++it)
        require(it.key() == "v" || it.key().rfind("handoff_", 0) == 0, "record keys prefixed");

    // A room not yet updated: no handoff_room at all (the games then use the seat).
    np::Snapshot fresh = s;
    fresh.room_msg = json();
    require(!np::handoff_settle_request(fresh).contains("handoff_room"), "no room before an update");

    // The game's status file.
    const fs::path record = dir / "launch-4242.json";
    std::string why;
    require(np::read_handoff_status(record, &why) == np::HandoffState::Pending, "no status: pending");
    write(fs::path(record.string() + ".status"), "{\"ok\":true}\n");
    require(np::read_handoff_status(record, &why) == np::HandoffState::Started, "ok status: started");
    write(fs::path(record.string() + ".status"),
          "{\"ok\":false,\"why\":\"disc \\\"A\\\" is not this game's\"}\n");
    require(np::read_handoff_status(record, &why) == np::HandoffState::Refused, "refused status");
    require(why == "disc \"A\" is not this game's", "refusal reason read back");
    write(fs::path(record.string() + ".status"), "{\"ok\":tr");  // half-written
    require(np::read_handoff_status(record, &why) == np::HandoffState::Pending, "partial status: pending");

    // The capability probe reads the executable; it never runs it.
    retcomm::LaunchPlan plan;
    plan.binary = dir / "old_game";
    write(plan.binary, std::string(3 << 20, 'x') + "--launcher");
    require(!retcomm::launch_supports_netplay_handoff(plan), "old build: no handoff");
    plan.binary = dir / "new_game";
    // The flag straddles the probe's 1 MiB read boundary.
    write(plan.binary, std::string((1 << 20) - 5, 'x') + "--netplay-query" + std::string(100, 'y'));
    require(retcomm::launch_supports_netplay_handoff(plan), "new build: handoff (across a read boundary)");
    // An AppImage: AppRun is a script, the payload's usr/bin is read.
    plan.binary = dir / "appimage" / "AppRun";
    write(plan.binary, "#!/bin/sh\nexec \"$APPDIR/usr/bin/Game\" \"$@\"\n");
    require(!retcomm::launch_supports_netplay_handoff(plan), "AppImage without the payload flag");
    write(dir / "appimage" / "usr" / "bin" / "Game", "elf...--netplay-query...");
    require(retcomm::launch_supports_netplay_handoff(plan), "AppImage payload with the flag");

    if (failures) return 1;
    std::printf("netplay_handoff_test: ok\n");
    return 0;
}
