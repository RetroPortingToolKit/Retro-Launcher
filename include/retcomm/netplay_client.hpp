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
    std::string player_id, display_name, country;
    bool ready = false, is_host = false, is_local = false, is_spectator = false;
};

struct ChatLine {
    std::uint64_t seq = 0;  // monotonic across both rings
    std::string from, from_player_id, text;
    bool is_system = false, is_local = false;
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
    std::vector<Member> members;
    nlohmann::json match_caps;
    std::deque<ChatLine> room_chat;
    std::string room_status;  // one sentence, th.warn style; cleared on success

    // Launch
    bool launch_pending = false;
    nlohmann::json launch;

    // Last server error{code}
    std::string last_error_code, last_error_detail;
    std::uint64_t error_seq = 0;

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
    void request_list();
    void create(const std::string& name, const std::string& password, int max_slots,
                const nlohmann::json& match_caps);
    void join(const std::string& lobby_id, const std::string& password);
    void leave();
    void close_room();
    void set_ready(bool ready);
    void chat(const std::string& text);
    void server_chat(const std::string& text);
    void start_match();
    void kick(int slot);
    void move_slot(int from_slot, int to_slot);
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
    bool session_rejected_ = false;
    std::uint64_t chat_seq_ = 0;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

}  // namespace retcomm::netplay
