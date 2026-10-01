#pragma once

// Starting a library game into a match the hub's lobby negotiated
// (docs/NETPLAY_HANDOFF.md).
//
// The hub holds the seat; the game only plays the match. Everything a game's
// own lobby would send or settle comes from the game itself, never from a copy
// in the hub:
//
//   * `<game> --launcher --netplay-query <request>` answers into
//     "<request>.answer": the fields its create/join carry (join_fields), its
//     set_ready message (ready), and its start message (start) -- the
//     player's defaults for {"handoff_query":"identity"}, settled from the
//     room for {"handoff_query":"settle"}. The hub sends those messages
//     verbatim. Every top-level key is handoff_-prefixed: the games' readers
//     take a key's first occurrence, and the server messages carried inside
//     have keys of their own ("op").
//   * On the server's `launch`, the hub writes a launch record -- the messages
//     that seated this player and the launch itself, verbatim -- and starts
//     the game with RECOMP_NETPLAY_LAUNCH=<record> and --launcher. recomp-ui
//     hands the record to the game, which replays the messages through its own
//     lobby code, and writes "<record>.status".

#include "retcomm/catalog.hpp"
#include "retcomm/launch.hpp"
#include "retcomm/netplay_client.hpp"
#include "retcomm/paths.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace retcomm::netplay {

namespace fs = std::filesystem;

// The launch record for the match `s` holds (s.launch_pending). Top-level keys
// are prefixed "handoff_" so no key inside a server message can be mistaken
// for one by the games' small JSON readers.
nlohmann::json handoff_record(const Snapshot& s);

// A settle request for the room `s` is in: who we are and the messages that
// seated us (the host's PLAY).
nlohmann::json handoff_settle_request(const Snapshot& s);

struct QueryAnswer {
    bool ok = false;        // the game answered
    nlohmann::json answer;  // its answer; "error" in it is the game refusing
    std::string error;      // why there is no answer
};

// Runs the game's --netplay-query with `request` (written under work_dir) and
// reads the answer. `opts` is a launch the way Play would start the title (its
// disc or ROM, its BIOS); its mode is set here. Blocks until the game exits:
// run it off the UI thread.
QueryAnswer run_handoff_query(const Paths& paths, const Title& title, LaunchOptions opts,
                              const nlohmann::json& request, const fs::path& work_dir);

// Writes the record for `s` under work_dir and starts the game into the match,
// detached. *record is where the record went, for read_handoff_status.
bool start_handoff_match(const Paths& paths, const Title& title, LaunchOptions opts,
                         const Snapshot& s, const fs::path& work_dir, fs::path* record,
                         std::string* why);

enum class HandoffState {
    Pending,  // the game has not said yet
    Started,  // it took the match
    Refused,  // it did not; *why says why
};
HandoffState read_handoff_status(const fs::path& record, std::string* why);

} // namespace retcomm::netplay
