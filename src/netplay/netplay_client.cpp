#include "retcomm/netplay_client.hpp"

#include "retcomm/netplay_ws.hpp"

#include <chrono>

namespace retcomm::netplay {

using json = nlohmann::json;

namespace {

constexpr std::size_t kChatRing = 400;
constexpr std::size_t kTraceRing = 60;
constexpr int kRecvTickMs = 200;
constexpr long long kPingEveryMs = 20000;

long long steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string str_or(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

int int_or(const json& j, const char* key, int def = 0) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<int>();
}

bool bool_or(const json& j, const char* key, bool def = false) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

}  // namespace

LobbyClient::~LobbyClient() { stop(); }

void LobbyClient::start(const LobbyConfig& cfg) {
    stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        cfg_ = cfg;
        snap_ = Snapshot{};
        snap_.state = ConnState::Connecting;
        outbound_.clear();
        session_rejected_ = false;
        pending_room_name_.clear();
        pending_max_slots_ = 0;
    }
    stop_ = false;
    worker_ = std::thread([this] { run(); });
}

void LobbyClient::stop() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}

Snapshot LobbyClient::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_;
}

void LobbyClient::queue(const json& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    outbound_.push_back(msg.dump());
}

json LobbyClient::hello_msg() const {
    json m = {{"op", "hello"}};
    if (!cfg_.display_name.empty()) m["display_name"] = cfg_.display_name;
    if (!cfg_.game_name.empty()) m["game_name"] = cfg_.game_name;
    if (!cfg_.session_token.empty()) m["session"] = cfg_.session_token;
    return m;
}

json LobbyClient::list_msg() const {
    json m = {{"op", "list"}};
    if (!cfg_.game_name.empty()) m["game_name"] = cfg_.game_name;
    if (!cfg_.game_version.empty()) m["game_version"] = cfg_.game_version;
    return m;
}

void LobbyClient::set_game(const std::string& game_name, const std::string& game_version) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_.game_name = game_name;
    cfg_.game_version = game_version;
    snap_.rooms.clear();
    snap_.players.clear();
    snap_.server_chat.clear();
    outbound_.push_back(hello_msg().dump());
    outbound_.push_back(list_msg().dump());
}

void LobbyClient::request_list() {
    std::lock_guard<std::mutex> lk(mu_);
    outbound_.push_back(list_msg().dump());
}

void LobbyClient::create(const std::string& name, const std::string& password, int max_slots,
                         const json& match_caps) {
    std::lock_guard<std::mutex> lk(mu_);
    json m = {{"op", "create"},
              {"name", name},
              {"game_name", cfg_.game_name},
              {"game_version", cfg_.game_version},
              {"max_slots", max_slots < 2 ? 2 : max_slots},
              {"host_bind", "0.0.0.0:7777"}};
    if (!cfg_.display_name.empty()) m["display_name"] = cfg_.display_name;
    if (!password.empty()) m["password"] = password;
    if (!cfg_.disc_fp.empty()) m["disc_fp"] = cfg_.disc_fp;
    if (match_caps.is_object() && !match_caps.empty()) m["match_caps"] = match_caps;
    pending_room_name_ = name;
    pending_max_slots_ = max_slots < 2 ? 2 : max_slots;
    outbound_.push_back(m.dump());
}

void LobbyClient::join(const std::string& lobby_id, const std::string& password) {
    std::lock_guard<std::mutex> lk(mu_);
    json m = {{"op", "join"},
              {"lobby_id", lobby_id},
              {"guest_bind", "0.0.0.0:7778"},
              {"game_name", cfg_.game_name},
              {"game_version", cfg_.game_version}};
    if (!cfg_.display_name.empty()) m["display_name"] = cfg_.display_name;
    if (!password.empty()) m["password"] = password;
    if (!cfg_.disc_fp.empty()) m["disc_fp"] = cfg_.disc_fp;
    pending_room_name_.clear();
    pending_max_slots_ = 0;
    for (const LobbyRow& r : snap_.rooms) {
        if (r.lobby_id == lobby_id) {
            pending_room_name_ = r.name;
            pending_max_slots_ = r.max_slots;
        }
    }
    outbound_.push_back(m.dump());
}

void LobbyClient::leave() { queue({{"op", "leave"}}); }
void LobbyClient::close_room() { queue({{"op", "close"}}); }
void LobbyClient::set_ready(bool ready) { queue({{"op", "set_ready"}, {"ready", ready}}); }
void LobbyClient::chat(const std::string& text) { queue({{"op", "chat"}, {"text", text}}); }

void LobbyClient::server_chat(const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    json m = {{"op", "server_chat"}, {"text", text}};
    if (!cfg_.game_name.empty()) m["game_name"] = cfg_.game_name;
    outbound_.push_back(m.dump());
}

void LobbyClient::start_match() { queue({{"op", "start"}}); }
void LobbyClient::kick(int slot) { queue({{"op", "kick"}, {"slot", slot}}); }

void LobbyClient::move_slot(int from_slot, int to_slot) {
    queue({{"op", "move"}, {"from_slot", from_slot}, {"to_slot", to_slot}});
}

void LobbyClient::send_raw(const json& msg) { queue(msg); }

void LobbyClient::leave_room_state(const std::string& why) {
    snap_.in_room = false;
    snap_.is_host = false;
    snap_.all_ready = false;
    snap_.lobby_id.clear();
    snap_.room_name.clear();
    snap_.local_slot = -1;
    snap_.player_count = 0;
    snap_.max_slots = 0;
    snap_.session_id = 0;
    snap_.members.clear();
    snap_.match_caps = json();
    snap_.room_chat.clear();
    snap_.launch_pending = false;
    snap_.launch = json();
    snap_.room_status = why;
}

void LobbyClient::push_chat(std::deque<ChatLine>& ring, ChatLine line) {
    line.seq = ++chat_seq_;
    ring.push_back(std::move(line));
    while (ring.size() > kChatRing) ring.pop_front();
}

void LobbyClient::handle(const json& m) {
    const std::string op = str_or(m, "op");
    std::lock_guard<std::mutex> lk(mu_);

    snap_.trace.push_back(m.dump());
    while (snap_.trace.size() > kTraceRing) snap_.trace.pop_front();

    if (op == "welcome") {
        snap_.player_id = str_or(m, "player_id");
        outbound_.push_back(hello_msg().dump());
        outbound_.push_back(list_msg().dump());
        return;
    }
    if (op == "hello_ok") {
        snap_.display_name = str_or(m, "display_name");
        // A hello that carried a token is signed in unless the server answered
        // error{session_invalid} first (it then continues the client as a
        // guest and still sends hello_ok).
        snap_.signed_in = !cfg_.session_token.empty() && !session_rejected_;
        return;
    }
    if (op == "lobby_list") {
        snap_.rooms.clear();
        snap_.players.clear();
        snap_.rooms_total = 0;
        const auto lobbies = m.find("lobbies");
        if (lobbies != m.end() && lobbies->is_array()) {
            snap_.rooms_total = lobbies->size();
            for (const json& l : *lobbies) {
                LobbyRow r;
                r.lobby_id = str_or(l, "lobby_id");
                r.name = str_or(l, "name");
                r.game_name = str_or(l, "game_name");
                r.game_version = str_or(l, "game_version");
                r.host_country = str_or(l, "host_country");
                r.player_count = int_or(l, "player_count");
                r.max_slots = int_or(l, "max_slots");
                r.has_password = bool_or(l, "has_password");
                r.allow_spectators = bool_or(l, "allow_spectators");
                r.max_spectators = int_or(l, "max_spectators");
                r.spectator_count = int_or(l, "spectator_count");
                if (!cfg_.game_name.empty() && r.game_name != cfg_.game_name) continue;
                if (!cfg_.game_version.empty() && r.game_version != cfg_.game_version) continue;
                snap_.rooms.push_back(std::move(r));
            }
        }
        const auto players = m.find("players");
        if (players != m.end() && players->is_array()) {
            for (const json& p : *players) {
                OnlinePlayer o;
                o.display_name = str_or(p, "display_name");
                o.country = str_or(p, "country");
                o.lobby_id = str_or(p, "lobby_id");
                o.lobby_name = str_or(p, "lobby_name");
                o.tag = str_or(p, "tag");
                o.game_name = str_or(p, "game_name");
                o.account = str_or(p, "account");
                o.hosting = bool_or(p, "hosting");
                o.is_local = !o.tag.empty() && snap_.player_id.compare(0, o.tag.size(), o.tag) == 0;
                // Unfiltered broadcasts carry everyone; keep our title only.
                if (!cfg_.game_name.empty() && !o.game_name.empty() && o.game_name != cfg_.game_name)
                    continue;
                snap_.players.push_back(std::move(o));
            }
        }
        ++snap_.list_seq;
        return;
    }
    if (op == "error") {
        snap_.last_error_code = str_or(m, "code");
        snap_.last_error_detail = str_or(m, "detail");
        if (snap_.last_error_detail.empty()) snap_.last_error_detail = str_or(m, "message");
        ++snap_.error_seq;
        if (snap_.last_error_code == "session_invalid") {
            snap_.signed_in = false;
            session_rejected_ = true;
        }
        return;
    }
    if (op == "created" || op == "joined") {
        if (!bool_or(m, "ok", true)) return;  // a refusal arrives as error{} anyway
        snap_.in_room = true;
        snap_.is_host = (op == "created");
        snap_.lobby_id = str_or(m, "lobby_id");
        snap_.room_name = pending_room_name_;
        snap_.local_slot = int_or(m, "local_slot", -1);
        snap_.session_id = m.value("session_id", 0LL);
        // created/joined carry no max_slots; lobby_update does. Until then use
        // what we asked for (create) or the row we joined from.
        snap_.max_slots = int_or(m, "max_slots", pending_max_slots_);
        snap_.room_status.clear();
        snap_.room_chat.clear();
        const auto caps = m.find("match_caps");
        if (caps != m.end() && caps->is_object()) snap_.match_caps = *caps;
        // Fall through into the slot map below.
    }
    if (op == "created" || op == "joined" || op == "lobby_update") {
        if (op == "lobby_update") {
            if (!snap_.in_room) snap_.in_room = true;
            snap_.lobby_id = str_or(m, "lobby_id");
            snap_.session_id = m.value("session_id", snap_.session_id);
            snap_.player_count = int_or(m, "player_count", snap_.player_count);
            snap_.max_slots = int_or(m, "max_slots", snap_.max_slots);
            snap_.all_ready = bool_or(m, "all_ready");
            const std::string host = str_or(m, "host_player_id");
            if (!host.empty()) snap_.is_host = (host == snap_.player_id);
            const auto caps = m.find("match_caps");
            if (caps != m.end() && caps->is_object()) snap_.match_caps = *caps;
        }
        const auto slots = m.find("slots");
        if (slots != m.end() && slots->is_array()) {
            snap_.members.clear();
            const std::string host = str_or(m, "host_player_id");
            for (const json& s : *slots) {
                Member mem;
                mem.slot = int_or(s, "slot", -1);
                mem.player_id = str_or(s, "player_id");
                mem.display_name = str_or(s, "display_name");
                mem.country = str_or(s, "country");
                mem.ready = bool_or(s, "ready");
                mem.is_local = !mem.player_id.empty() && mem.player_id == snap_.player_id;
                mem.is_host = host.empty() ? mem.slot == 0 : mem.player_id == host;
                if (mem.is_local) snap_.local_slot = mem.slot;
                snap_.members.push_back(std::move(mem));
            }
            if (op != "lobby_update") snap_.player_count = static_cast<int>(snap_.members.size());
        }
        const auto specs = m.find("spectators");
        if (specs != m.end() && specs->is_array()) {
            for (const json& s : *specs) {
                Member mem;
                mem.slot = int_or(s, "slot", -1);
                mem.player_id = str_or(s, "player_id");
                mem.display_name = str_or(s, "display_name");
                mem.country = str_or(s, "country");
                mem.is_spectator = true;
                mem.is_local = !mem.player_id.empty() && mem.player_id == snap_.player_id;
                snap_.members.push_back(std::move(mem));
            }
        }
        return;
    }
    if (op == "chat" || op == "server_chat") {
        ChatLine line;
        line.from = str_or(m, "from");
        line.from_player_id = str_or(m, "from_player_id");
        line.text = str_or(m, "text");
        line.is_system = line.from.empty();
        line.is_local = !line.from_player_id.empty() && line.from_player_id == snap_.player_id;
        push_chat(op == "chat" ? snap_.room_chat : snap_.server_chat, std::move(line));
        return;
    }
    if (op == "kicked") {
        leave_room_state("You were removed from the room by the host.");
        return;
    }
    if (op == "lobby_closed") {
        leave_room_state("The host closed the room.");
        return;
    }
    if (op == "launch") {
        snap_.launch_pending = true;
        snap_.launch = m;
        snap_.session_id = m.value("session_id", snap_.session_id);
        return;
    }
    if (op == "need_mods") {
        snap_.last_error_code = "need_mods";
        snap_.last_error_detail = "the host's match requires mods this build does not have";
        ++snap_.error_seq;
        return;
    }
    // pong, host_endpoint_ok, path_report_ok, signal, turn_credentials, …:
    // recorded in the trace, otherwise ignored for now.
}

void LobbyClient::run() {
    LobbyConfig cfg;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cfg = cfg_;
    }
    WsConnection ws;
    std::string err;
    if (!ws.connect(cfg.url, &err)) {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.state = ConnState::Failed;
        snap_.transport_error = err;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.state = ConnState::Connected;
        snap_.transport_error.clear();
    }

    long long last_ping = steady_ms();
    std::string msg;
    bool alive = true;
    while (alive && !stop_) {
        std::deque<std::string> out;
        {
            std::lock_guard<std::mutex> lk(mu_);
            out.swap(outbound_);
        }
        for (const std::string& text : out) {
            if (!ws.send_text(text, &err)) {
                alive = false;
                break;
            }
        }
        if (!alive) break;

        const int r = ws.recv_text(msg, kRecvTickMs, &err);
        if (r < 0) {
            alive = false;
            break;
        }
        if (r == 1) {
            json parsed = json::parse(msg, nullptr, false);
            if (parsed.is_object()) handle(parsed);
        }
        if (steady_ms() - last_ping > kPingEveryMs) {
            last_ping = steady_ms();
            if (!ws.send_text(R"({"op":"ping"})", &err)) {
                alive = false;
                break;
            }
        }
    }

    if (alive) {
        // Orderly exit: tell the room, then close.
        bool in_room = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            in_room = snap_.in_room;
        }
        if (in_room) ws.send_text(R"({"op":"leave"})", &err, 1000);
        ws.close();
        err.clear();
    }
    std::lock_guard<std::mutex> lk(mu_);
    snap_.state = ConnState::Disconnected;
    snap_.transport_error = err;
    if (snap_.in_room) leave_room_state(err.empty() ? "Disconnected." : "Disconnected: " + err);
}

}  // namespace retcomm::netplay
