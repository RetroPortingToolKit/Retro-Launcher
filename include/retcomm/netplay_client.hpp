#pragma once

// Lobby client for recomp-net-server's JSON-over-WebSocket protocol
// (recomp-net-server/docs/WS_LOBBY.md). One connection, one worker thread.
//
// Shape follows recomp-ui's netplay backend: the UI never blocks and never
// gets callbacks. It queues commands, and once per frame copies a Snapshot
// that the worker keeps current. Chat rings are the only source of chat
// lines; the client never appends its own (the server echoes the sender's
// line in room order).

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

namespace retcomm::netplay {

enum class ConnState { Disconnected, Connecting, Connected, Failed };

struct LobbyConfig {
    std::string url;            // ws:// or wss://
    std::string session_token;  // Discord session JWT; empty = guest hello
    std::string display_name;   // used only when session_token is empty
    std::string game_name;      // wire scope; empty = unfiltered
    std::string game_version;   // wire pin; empty = unfiltered
    std::string disc_fp;        // optional SHA-256 of the disc TOC
};

struct LobbyRow {
    std::string lobby_id, name, game_name, game_version, host_country;
    int player_count = 0, max_slots = 0;
    bool has_password = false;
    bool allow_spectators = false;
    int max_spectators = 0, spectator_count = 0;
    int latency_ms = -1;
};

struct OnlinePlayer {
    std::string display_name, country, lobby_id, lobby_name, tag, game_name, account;
    bool hosting = false;
    bool is_local = false;
};

struct Member {
    int slot = -1;
    std::string player_id, display_name, country, account;
    bool ready = false, is_host = false, is_local = false, is_spectator = false;
};

struct ChatLine {
    std::uint64_t seq = 0;  // monotonic across both rings
    std::string from, from_player_id, text;
    std::string mid;           // the server's message id: what a report names
    std::string from_account;  // opaque account id; empty for a guest
    std::string country;
    bool is_system = false, is_local = false;
};

// A `signal` another member sent (recomp-net-server WS_LOBBY.md): opaque
// text the server relays within the room.
struct SignalMsg {
    std::uint64_t seq = 0;
    std::string from_player_id;
    int type = 0;
    std::string text;
};

struct Snapshot {
    ConnState state = ConnState::Disconnected;
    std::string transport_error;  // why state is Failed / Disconnected
    std::string player_id;        // server-minted per connection
    std::string display_name;     // as accepted by hello_ok
    bool signed_in = false;       // hello carried a session the server accepted

    // Browser
    std::vector<LobbyRow> rooms;      // filtered to game_name/game_version when set
    std::vector<OnlinePlayer> players;
    std::size_t rooms_total = 0;      // before filtering
    std::uint64_t list_seq = 0;       // bumps on every lobby_list
    std::deque<ChatLine> server_chat;

    // Room
    bool in_room = false, is_host = false, all_ready = false;
    std::string lobby_id, room_name;
    int local_slot = -1, player_count = 0, max_slots = 0;
    long long session_id = 0;
    std::string host_endpoint;  // the host's advertised UDP address (lobby_update)
    std::vector<Member> members;
    nlohmann::json match_caps;
    std::deque<ChatLine> room_chat;
    std::string room_status;  // one sentence, th.warn style; cleared on success

    // Launch
    bool launch_pending = false;
    nlohmann::json launch;
    // The server messages that seated us, verbatim: the created/joined reply
    // and the latest lobby_update. A game started into the match replays them
    // with the launch (docs/NETPLAY_HANDOFF.md), so it settles from exactly
    // what the server said rather than from this client's reading of it.
    nlohmann::json seat_msg, room_msg;
    std::string lobby_url;  // the server they came from

    // A seated player asked to swap seats with us (seat_swap_ask); answer
    // with seat_swap_answer. seq bumps per ask.
    struct SwapAsk {
        std::string asker_player_id, asker_name;
        int from_slot = -1, target_slot = -1;
        std::uint64_t seq = 0;
    } swap_ask;
    // The answer to our own seat_swap_request.
    struct SwapResult {
        bool accepted = false;
        std::uint64_t seq = 0;
    } swap_result;
    std::uint64_t report_ok_seq = 0;  // chat_report_ok count
    int rtt_ms = -1;                  // lobby server round trip, from ping/pong

    // Last server error{code}
    std::string last_error_code, last_error_detail;
    std::uint64_t error_seq = 0;

    // Signals received in the room, newest last (a ring; seq is monotonic).
    std::deque<SignalMsg> signals;

    // Raw op trace (newest last), for `retcomm netplay probe --verbose`
    std::deque<std::string> trace;
};

class LobbyClient {
public:
    LobbyClient() = default;
    ~LobbyClient();
    LobbyClient(const LobbyClient&) = delete;
    LobbyClient& operator=(const LobbyClient&) = delete;

    void start(const LobbyConfig& cfg);
    void stop();
    bool running() const { return worker_.joinable(); }

    Snapshot snapshot() const;

    // Commands: queued to the worker, applied in order.
    void set_game(const std::string& game_name, const std::string& game_version);
    // A guest's new name, announced with a fresh hello (a signed-in player's
    // name is the server's and is not changed here).
    void set_display_name(const std::string& name);
    void request_list();
    void create(const std::string& name, const std::string& password, int max_slots,
                const nlohmann::json& match_caps);
    // A room for a game other than the connection's scope (the hub's
    // cross-game browser): the room is created for, or joined as, exactly
    // this game_name / game_version, which the server checks.
    // `extra`: fields the game itself would add to its create / join
    // (NETPLAY_HANDOFF.md join_fields: disc_fp, mod_offer), merged as given.
    void create_for(const std::string& game_name, const std::string& game_version,
                    const std::string& name, const std::string& password, int max_slots,
                    bool allow_spectators, const nlohmann::json& match_caps,
                    const nlohmann::json& extra = nlohmann::json::object());
    void join(const std::string& lobby_id, const std::string& password);
    void join_as(const std::string& lobby_id, const std::string& password,
                 const std::string& game_name, const std::string& game_version,
                 const nlohmann::json& extra = nlohmann::json::object());
    void leave();
    void close_room();
    void set_ready(bool ready);
    void chat(const std::string& text);
    void server_chat(const std::string& text);
    void start_match();
    void kick(int slot);
    void move_slot(int from_slot, int to_slot);       // host
    void seat_move(int to_slot);                      // self, to an empty seat
    void seat_swap_request(int target_slot);          // self, onto a player
    void seat_swap_answer(bool accept, const std::string& asker_player_id);
    void set_blocks(const std::vector<std::string>& accounts);
    // reason: harassment | hate_speech | threats | sexual_content | spam |
    // cheating_claim | other (recomp-ui's list).
    void chat_report(const std::vector<std::string>& mids, const std::string& reason,
                     const std::string& note);
    // Host relay (recomp-net-server WS_LOBBY.md "Host relay"): the host's
    // reachable address, and a guest's verdict on reaching it.
    void set_host_endpoint(const std::string& endpoint);
    // The launch was taken: clear launch_pending, so a rematch's launch is new.
    void ack_launch();
    void path_report(const std::string& path); // "direct" | "fail"
    // Opaque text to every other member of the room (to_player_id empty) or one.
    void signal(int type, const std::string& text, const std::string& to_player_id = {});
    void send_raw(const nlohmann::json& msg);

private:
    void run();
    void handle(const nlohmann::json& m);
    void queue(const nlohmann::json& msg);
    nlohmann::json hello_msg() const;  // caller holds mu_
    nlohmann::json list_msg() const;   // caller holds mu_
    void leave_room_state(const std::string& why);  // caller holds mu_
    void push_chat(std::deque<ChatLine>& ring, ChatLine line);  // caller holds mu_

    mutable std::mutex mu_;
    LobbyConfig cfg_;
    Snapshot snap_;
    std::deque<std::string> outbound_;
    std::string pending_room_name_;
    int pending_max_slots_ = 0;
    long long ping_sent_ms_ = 0;
    bool session_rejected_ = false;
    std::uint64_t chat_seq_ = 0;
    std::uint64_t signal_seq_ = 0;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

}  // namespace retcomm::netplay
