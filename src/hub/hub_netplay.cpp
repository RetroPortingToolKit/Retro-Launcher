#include "hub/hub_netplay.hpp"
#include "hub/hub_overlay.hpp"

#include "hub/hub_model.hpp"
#include "hub/hub_theme.hpp"
#include "hub/hub_widgets.hpp"
#include "retcomm/config.hpp"
#include "retcomm/netplay_account.hpp"
#include "retcomm/netplay_client.hpp"
#include "retcomm/netplay_handoff.hpp"
#include "retcomm/netplay_lan.hpp"
#include "retcomm/netplay_nat.hpp"
#include "retcomm/paths.hpp"
#include "retcomm/hash.hpp"
#include "hub/hub_core_settings.hpp"

#include "imgui.h"
#include <SDL3/SDL_clipboard.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace retcomm::hub {

namespace {

namespace np = retcomm::netplay;
using json = nlohmann::json;

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void copy_to(char* dst, size_t cap, const std::string& s) {
    if (!cap) return;
    const size_t n = std::min(cap - 1, s.size());
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// Name rules, as recomp-ui's: refused, never masked.
std::string name_problem(const std::string& n, const char* what) {
    if (n.empty()) return std::string("Enter a ") + what + ".";
    if (n.size() > 63) return std::string("That ") + what + " is too long.";
    size_t chars = 0;
    for (unsigned char c : n)
        if ((c & 0xC0) != 0x80) ++chars;
    if (chars > 32) return std::string("That ") + what + " is longer than 32 characters.";
    return {};
}

// Server error{code} -> one sentence (recomp-ui np_ingest_last_error's wording
// where it has one).
std::string error_sentence(const std::string& code, const std::string& detail) {
    static const std::map<std::string, std::string> k = {
        {"need_password", "This lobby needs a password."},
        {"bad_password", "Incorrect password."},
        {"full", "That lobby is full."},
        {"gone", "Selected lobby is no longer available."},
        {"version_mismatch", "That lobby runs a different version of the game than yours."},
        {"game_mismatch", "That lobby is for a different game."},
        {"disc_mismatch", "Your disc image does not match the host's."},
        {"name_rejected", "That name can't be used. Please pick another one."},
        {"not_host", "Only the host can do that."},
        {"not_in_lobby", "You are not in a lobby."},
        {"slot_taken", "That seat was just taken."},
        {"bad_slot", "That seat does not exist."},
        {"not_seated", "You need a seat to do that."},
        {"empty_slot", "Nobody is in that seat."},
        {"no_game", "Pick a game in the filter to use server chat."},
        {"need_mods", "The host's match needs mods this build does not have."},
        {"session_invalid", "Your sign-in expired. Sign in again."},
        {"rate_limited", "Slow down a little."},
    };
    const auto it = k.find(code);
    if (it != k.end()) return it->second;
    return "Lobby server: " + code + (detail.empty() ? "" : " (" + detail + ")");
}

// ---- installed games ---------------------------------------------------------------

struct Game {
    std::string catalog_id, title, game_name, pin;
    int max_slots = 2;
};

const NetplayScope* current_scope();

std::vector<Game> installed_games(const HubModel& hub) {
    std::vector<Game> out;
    if (const NetplayScope* sc = current_scope()) {
        out.push_back({"", sc->title, sc->game_name, sc->pin, std::max(2, sc->max_slots)});
        return out;
    }
    std::set<std::string> seen;
    for (const TitleRow& r : hub.rows) {
        if (!r.netplay_supported || !r.installed || r.netplay_game_name.empty()) continue;
        if (!seen.insert(r.netplay_game_name).second) continue;
        out.push_back({r.id, r.name, r.netplay_game_name, r.netplay_pin,
                       std::max(2, r.netplay_max_slots)});
    }
    std::sort(out.begin(), out.end(), [](const Game& a, const Game& b) { return a.title < b.title; });
    return out;
}

const Game* game_by_wire_name(const std::vector<Game>& games, const std::string& name) {
    for (const Game& g : games)
        if (g.game_name == name) return &g;
    return nullptr;
}

// The catalog's display name for a wire game_name, even when not installed.
std::string title_for(const HubModel& hub, const std::string& wire) {
    if (const NetplayScope* sc = current_scope(); sc && sc->game_name == wire) return sc->title;
    for (const TitleRow& r : hub.rows)
        if (r.netplay_supported && r.netplay_game_name == wire) return r.name;
    return wire;
}

// ---- moderation (ignore / block), kept per machine -----------------------------------

struct Moderation {
    struct Entry {
        std::string name;
        bool blocked = false; // else ignored
    };
    std::map<std::string, Entry> by_account;
    fs::path file;
    std::uint64_t rev = 1;

    void load(const fs::path& f) {
        file = f;
        std::ifstream in(f);
        if (!in) return;
        const json j = json::parse(in, nullptr, false);
        if (!j.is_object()) return;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!it->is_object()) continue;
            by_account[it.key()] = {it->value("name", ""), it->value("blocked", false)};
        }
    }
    void save() {
        json j = json::object();
        for (const auto& [acct, e] : by_account) j[acct] = {{"name", e.name}, {"blocked", e.blocked}};
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream(file) << j.dump(2) << "\n";
        ++rev;
    }
    bool hidden(const std::string& acct) const { return !acct.empty() && by_account.count(acct); }
    bool blocked(const std::string& acct) const {
        const auto it = by_account.find(acct);
        return it != by_account.end() && it->second.blocked;
    }
    std::vector<std::string> blocked_accounts() const {
        std::vector<std::string> v;
        for (const auto& [a, e] : by_account)
            if (e.blocked) v.push_back(a);
        return v;
    }
};

// ---- Discord sign-in, on its own thread -------------------------------------------------

class Account {
public:
    enum class State { Guest, Waiting, SignedIn, Failed };

    ~Account() { join(); }

    void configure(const std::string& ws_url, const fs::path& key) {
        join();
        std::lock_guard<std::mutex> lk(mu_);
        acct_ = std::make_unique<np::DiscordAccount>(np::DiscordAccount::base_from_ws_url(ws_url), key);
        state_ = State::Guest;
        info_ = {};
        error_.clear();
    }

    // Silent relogin with the stored device key, if there is one.
    void restore() {
        if (busy() || !acct_ || !acct_->has_stored_key()) return;
        start([this] {
            np::AccountInfo info;
            std::string err;
            bool rejected = false;
            const bool ok = acct_->restore(&info, &err, &rejected);
            finish(ok, info, ok ? std::string() : rejected ? "your saved sign-in was not accepted" : err);
        });
    }

    void login() {
        if (busy() || !acct_) return;
        start([this] {
            np::AccountInfo info;
            std::string err;
            const bool ok = acct_->login(&info, &err, 300, true,
                                         [this](const std::string& u) {
                                             std::lock_guard<std::mutex> lk(mu_);
                                             login_url_ = u;
                                         },
                                         &cancel_);
            finish(ok, info, err);
        });
    }

    // Retry: drop a login still waiting on the browser, then start again.
    void relogin() {
        cancel_ = true;
        join();
        login();
    }

    void sign_out() {
        cancel_ = true;
        join();
        std::string err;
        if (acct_) acct_->sign_out(&err);
        std::lock_guard<std::mutex> lk(mu_);
        state_ = State::Guest;
        info_ = {};
        error_.clear();
    }

    State state() const {
        std::lock_guard<std::mutex> lk(mu_);
        return state_;
    }
    np::AccountInfo info() const {
        std::lock_guard<std::mutex> lk(mu_);
        return info_;
    }
    std::string error() const {
        std::lock_guard<std::mutex> lk(mu_);
        return error_;
    }
    double waiting_since() const {
        std::lock_guard<std::mutex> lk(mu_);
        return waiting_since_;
    }
    // The page the sign-in opened (or tried to): shown so it can be opened by hand or copied.
    std::string login_url() const {
        std::lock_guard<std::mutex> lk(mu_);
        return login_url_;
    }
    // Discord is offered unless the server said otherwise (503 at start).
    bool offered() const { return acct_ && !acct_->unavailable(); }

    void join() {
        if (th_.joinable()) th_.join();
        cancel_ = false;
    }
    void shutdown() {
        cancel_ = true;
        join();
    }
    // Stop waiting for the browser.
    void cancel() {
        cancel_ = true;
        join();
        std::lock_guard<std::mutex> lk(mu_);
        state_ = State::Guest;
        error_.clear();
    }

private:
    bool busy() const {
        std::lock_guard<std::mutex> lk(mu_);
        return state_ == State::Waiting;
    }
    template <class F>
    void start(F f) {
        if (th_.joinable()) th_.join();
        cancel_ = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            state_ = State::Waiting;
            error_.clear();
            login_url_.clear();
            waiting_since_ = now_s();
        }
        th_ = std::thread(std::move(f));
    }
    void finish(bool ok, const np::AccountInfo& info, const std::string& err) {
        std::lock_guard<std::mutex> lk(mu_);
        if (ok) {
            state_ = State::SignedIn;
            info_ = info;
            error_.clear();
        } else {
            state_ = cancel_ ? State::Guest : State::Failed;
            error_ = err;
        }
    }

    mutable std::mutex mu_;
    std::unique_ptr<np::DiscordAccount> acct_;
    State state_ = State::Guest;
    np::AccountInfo info_;
    std::string error_;
    double waiting_since_ = 0;
    std::string login_url_;
    std::atomic<bool> cancel_{false};
    std::thread th_;
};

// ---- the page's state --------------------------------------------------------------------

enum class View { Mode, SignIn, Online, Lan, Host, Room };

struct Page {
    bool init = false;
    View view = View::Mode;
    Account account;
    np::LobbyClient client;
    bool client_started = false;
    std::string client_url;
    bool client_signed = false;     // the connection's hello carried a session
    bool scope_dirty = false;       // a room pinned the connection's game scope
    std::string filter;             // wire game_name; "" = all games
    np::LanBrowser lan;
    Moderation mod;

    np::Snapshot snap;              // this frame's
    std::uint64_t seen_error_seq = 0, seen_swap_seq = 0, seen_swap_result_seq = 0;
    std::uint64_t seen_report_seq = 0, pushed_blocks_rev = 0;
    bool was_in_room = false, saw_launch = false;
    std::string status;             // browser / LAN status line (th.warn)
    std::string room_status;
    std::string selected_lobby;
    int selected_lan = -1;
    std::uint64_t server_chat_seen = 0, room_chat_seen = 0;
    char server_chat_in[256]{}, room_chat_in[256]{};
    double decline_swaps_until = 0;
    double mode_entered_at = 0;
    bool asked_name = false;
    // The overlay (draw_netplay_overlay): the open section of a view, a person or message a tap
    // opened the actions of, and the seat likewise.
    int sub = 0;
    View sub_view = View::Mode, last_view = View::Mode;
    std::string act_name, act_account, act_mid;
    int seat_action_slot = -1;
    bool seat_action_spec = false;

    // Modals
    bool open_host = false, open_name = false, open_net = false, open_join_pw = false;
    bool open_direct = false, open_report = false, open_modlist = false, open_room_info = false;
    bool open_swap = false;
    int host_game = 0;
    char host_name[128]{}, host_pw[64]{};
    int host_max = 2;
    bool host_spectators = false;
    std::string host_error;
    char join_pw[64]{};
    std::string join_pw_lobby, join_pw_game, join_pw_version;
    char name_buf[64]{};
    std::string name_error;
    char url_buf[256]{};
    char direct_ip[64]{}, direct_port[8]{"7777"};
    std::string direct_status;
    std::string report_mid, report_name;
    int report_reason = 0;
    char report_note[200]{};
    np::Snapshot::SwapAsk swap;
    // Direct mode (NetplayScope): set by each draw; the launch handled last.
    const NetplayScope* scope = nullptr;
    long long launched_session = -1;
    bool host_tpak = false;
    // Host relay (NETPLAY_DIRECT.md): the host holds its game port while the
    // room waits; guests probe it and report.
    np::HostPort host_port;
    bool host_port_open = false;
    bool host_endpoint_sent = false;
    std::string probed_endpoint;
    std::future<bool> probe;
    std::string relay_note;
    bool relay_note_bad = false;
    // Transfer Pak lobbies: the paks the room exchanged, by player.
    struct PakGot {
        int seat = -1, cart = -1;
        std::string save_sha, path; // path: the save, stored under scope->pak_dir
    };
    std::map<std::string, PakGot> paks;
    struct PakParts {
        int seat = -1, cart = -1, n = 0;
        std::string save_sha;
        std::map<int, std::string> parts;
    };
    std::map<std::string, PakParts> pak_parts;
    std::uint64_t signals_seen = 0;
    std::string pak_members; // the room's player ids when our pak was last sent
    bool pak_ready = false;
    NetplayPak my_pak;
    std::string pak_note;

    // Library games (docs/NETPLAY_HANDOFF.md): the game this hub starts into
    // the match its lobby negotiates. Everything the game's own lobby would
    // send comes from the game (its --netplay-query answer), verbatim.
    struct LibGame {
        std::string game_name;    // the room's wire name
        Title title;              // a copy: the queries run on workers
        LaunchOptions opts;       // as Play would start it
        fs::path work_dir;        // <data>/netplay/<title id>
        bool capable = false;     // the build takes hub matches
        std::string why;          // !capable: why not, in a sentence
        std::future<np::QueryAnswer> identity_f;
        json identity;            // the game's identity answer, once in
        std::future<np::QueryAnswer> settle_f;
        fs::path record;          // the running match's launch record
        bool record_open = false; // still waiting for the game's status
    };
    std::unique_ptr<LibGame> lib;
    // A join or create waiting on the game's identity (its join fields).
    struct PendingSeat {
        bool create = false;
        std::string lobby_id, password, game_name, version, name;
        int max_slots = 2;
        bool spectators = false;
    };
    std::optional<PendingSeat> pending_seat;
    double ready_sent_at = -1;
};

constexpr int kPakSignal = 0x5450; // "TP"
constexpr std::size_t kPakChunk = 12288;

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const std::string& in) {
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i < in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool b64_decode(const std::string& in, std::string* out) {
    out->clear();
    unsigned v = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const char* at = std::strchr(kB64, c);
        if (!at || !c) return false;
        v = (v << 6) | static_cast<unsigned>(at - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<char>((v >> bits) & 0xFF));
        }
    }
    return true;
}

Page& page() {
    static Page p;
    return p;
}

const NetplayScope* current_scope() { return page().scope; }

std::string lobby_url(const HubModel& hub) { return hub.cfg.resolve_netplay_lobby_url(); }

std::string local_name(const HubModel& hub) {
    const Page& p = page();
    if (p.account.state() == Account::State::SignedIn) {
        const std::string h = p.account.info().handle;
        if (!h.empty()) return h;
    }
    return hub.cfg.netplay.display_name;
}

void ensure_init(HubModel& hub) {
    Page& p = page();
    if (p.init) return;
    p.init = true;
    p.account.configure(lobby_url(hub), hub.paths.data_dir / "netplay" / "account.key");
    p.account.restore();
    p.mod.load(hub.paths.data_dir / "netplay" / "moderation.json");
    p.mode_entered_at = now_s();
    if (p.scope) p.filter = p.scope->game_name; // one game, always
}

// (Re)connect to the lobby server with what we know now: the session when
// signed in, the name, and the filter as the connection's game scope.
void connect(HubModel& hub) {
    Page& p = page();
    np::LobbyConfig c;
    c.url = lobby_url(hub);
    c.display_name = local_name(hub).empty() ? "Player" : local_name(hub);
    if (p.account.state() == Account::State::SignedIn) c.session_token = p.account.info().session;
    c.game_name = p.filter;
    p.client.start(c);
    p.client_started = true;
    p.client_url = c.url;
    p.client_signed = !c.session_token.empty();
    p.scope_dirty = false;
    p.pushed_blocks_rev = 0;
    p.status = "Connecting to lobby server\xE2\x80\xA6";
}

void enter_online(HubModel& hub) {
    Page& p = page();
    const bool signed_in = p.account.state() == Account::State::SignedIn;
    // A connection made before signing in (or as someone else) starts over.
    if (!p.client_started || p.client_signed != signed_in || p.client_url != lobby_url(hub))
        connect(hub);
    p.view = View::Online;
}

void enter_lan(HubModel& hub) {
    Page& p = page();
    p.lan.start();
    p.lan.refresh();
    p.view = View::Lan;
    if (hub.cfg.netplay.display_name.empty() && !p.asked_name) {
        p.asked_name = true;
        p.open_name = true;
    }
}

void set_filter(HubModel& hub, const std::string& wire, const std::vector<Game>& games) {
    Page& p = page();
    if (wire == p.filter) return;
    const bool to_all = wire.empty();
    p.filter = wire;
    p.selected_lobby.clear();
    p.server_chat_seen = 0;
    // The server keeps a connection's title once it has one, so "All games"
    // needs a fresh connection; any title can be switched to in place.
    if (to_all || !p.client_started) {
        connect(hub);
    } else {
        (void)games;
        p.client.set_game(wire, "");
    }
}

// ---- chat -------------------------------------------------------------------------------

struct ChatBinding {
    const char* id;
    const char* title;
    const char* empty_text;
    const char* hint;
    const std::deque<np::ChatLine>* lines;
    std::uint64_t* seen;
    char* input;
    size_t input_cap;
    bool enabled;
    const char* disabled_text;
};

void player_menu(HubModel& hub, const Theme& th, const std::string& name, const std::string& account,
                 const std::string& mid);

void draw_chat_panel(HubModel& hub, const Theme& th, const ChatBinding& b,
                     const std::function<void(const std::string&)>& send) {
    Page& p = page();
    ImGui::TextColored(th.accent, "%s", b.title);
    const float input_h = ImGui::GetFrameHeight() + 10.f;
    const float log_h = std::max(80.f, ImGui::GetContentRegionAvail().y - input_h);
    ImGui::BeginChild(b.id, ImVec2(0, log_h), ImGuiChildFlags_Borders);
    bool any = false;
    std::uint64_t newest = 0;
    for (const np::ChatLine& l : *b.lines) {
        newest = l.seq;
        if (!l.is_local && p.mod.hidden(l.from_account)) continue;
        any = true;
        ImGui::PushID(static_cast<int>(l.seq));
        if (l.is_system) {
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped("%s", l.text.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextColored(l.is_local ? th.good : th.accent, "%s", l.from.c_str());
            if (!l.is_local && ImGui::BeginPopupContextItem("who")) {
                player_menu(hub, th, l.from, l.from_account, l.mid);
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            ImGui::TextWrapped("%s", l.text.c_str());
        }
        ImGui::PopID();
    }
    if (!any) {
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped("%s", b.enabled ? b.empty_text : b.disabled_text);
        ImGui::PopStyleColor();
    }
    // Follow new lines; a reader scrolled up stays where they are.
    if (newest != *b.seen) {
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.f) ImGui::SetScrollHereY(1.f);
        *b.seen = newest;
    }
    ImGui::EndChild();

    ImGui::BeginDisabled(!b.enabled);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80.f);
    std::string field_id = std::string("##in_") + b.id;
    const bool enter = ImGui::InputTextWithHint(field_id.c_str(), b.hint, b.input, b.input_cap,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    if (enter) ImGui::SetKeyboardFocusHere(-1); // Enter keeps the field
    ImGui::SameLine();
    std::string send_id = std::string("Send##") + b.id;
    const bool clicked = ImGui::Button(send_id.c_str(), ImVec2(72, 0));
    ImGui::EndDisabled();
    if ((enter || clicked) && b.enabled) {
        const std::string text = trim(b.input);
        if (!text.empty()) send(text);
        b.input[0] = '\0';
    }
}

void player_menu(HubModel& hub, const Theme& th, const std::string& name, const std::string& account,
                 const std::string& mid) {
    Page& p = page();
    (void)hub;
    ImGui::TextColored(th.accent, "%s", name.c_str());
    ImGui::Separator();
    if (account.empty()) {
        ImGui::TextDisabled("Signed-out players cannot be ignored:");
        ImGui::TextDisabled("there is no account to remember.");
    } else {
        const bool hidden = p.mod.hidden(account);
        const bool blocked = p.mod.blocked(account);
        if (ImGui::MenuItem(hidden && !blocked ? "Stop Ignoring" : "Ignore Player", nullptr, false,
                            !blocked)) {
            if (hidden) p.mod.by_account.erase(account);
            else p.mod.by_account[account] = {name, false};
            p.mod.save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hide their chat. They can still be in your lobby.");
        if (ImGui::MenuItem(blocked ? "Unblock Player" : "Block Player")) {
            if (blocked) p.mod.by_account.erase(account);
            else p.mod.by_account[account] = {name, true};
            p.mod.save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hide their chat, hide them from the lists, and ask the matchmaker not "
                              "to pair you.");
    }
    if (!mid.empty() && ImGui::MenuItem("Report Message...")) {
        p.report_mid = mid;
        p.report_name = name;
        p.report_reason = 0;
        p.report_note[0] = '\0';
        p.open_report = true;
    }
    if (ImGui::MenuItem("Moderation list...")) p.open_modlist = true;
}

// ---- views ----------------------------------------------------------------------------

bool mode_card(const char* id, const char* title, const char* body, bool accent, bool enabled,
               const Theme& th, float w) {
    const float h = 150.f;
    ImGui::BeginDisabled(!enabled);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool pressed = retcomm::hub::overlay::nav_button(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h),
                      ImGui::ColorConvertFloat4ToU32(hot ? th.panel_hovered : th.panel), 10.f);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h),
                ImGui::ColorConvertFloat4ToU32(hot || accent ? th.accent : th.border), 10.f, 0,
                hot ? 2.f : 1.f);
    dl->AddText(ImVec2(p0.x + 20.f, p0.y + 18.f),
                ImGui::ColorConvertFloat4ToU32(accent ? th.accent : th.text), title);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p0.x + 20.f, p0.y + 50.f),
                ImGui::ColorConvertFloat4ToU32(enabled ? th.text_muted : th.border), body, nullptr,
                w - 40.f);
    ImGui::EndDisabled();
    return pressed && enabled;
}

void draw_mode(HubModel& hub, const Theme& th) {
    Page& p = page();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 760.f);
    ImGui::TextColored(th.text_muted,
                       "How do you want to play? You can change this any time by coming back here.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 12));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w = avail > 900.f ? 420.f : std::max(200.f, (avail - 30.f) * 0.5f);

    const Account::State st = p.account.state();
    const bool offered = p.account.offered();
    const double waited = st == Account::State::Waiting ? now_s() - p.account.waiting_since() : 0;
    const bool holding = st == Account::State::Waiting && waited < 5.0;
    const char* online_body =
        !offered ? "Find players on the lobby server. This server does not offer sign-in, so you "
                   "will play as a guest."
        : holding ? "Checking your saved sign-in\xE2\x80\xA6"
                  : "Find players on the lobby server. Sign in with Discord so your name is yours "
                    "across sessions.";

    if (mode_card("##lan", "LAN / Direct IP",
                  "Play with someone on your network, or connect straight to an address they give "
                  "you. No account, no lobby server.",
                  false, true, th, w))
        enter_lan(hub);
    ImGui::SameLine(0, 24.f);
    if (mode_card("##online", "Online Netplay", online_body, true, !holding, th, w)) {
        if (!offered || st == Account::State::SignedIn || st == Account::State::Waiting)
            enter_online(hub);
        else
            p.view = View::SignIn;
    }

    ImGui::Dummy(ImVec2(0, 16));
    if (st == Account::State::SignedIn) {
        ImGui::TextColored(th.good, "Signed in as %s", p.account.info().handle.c_str());
    } else if (holding) {
        ImGui::TextColored(th.text_muted, "Checking your saved sign-in\xE2\x80\xA6");
    } else if (st == Account::State::Waiting) {
        ImGui::TextColored(th.text_muted,
                           "Still checking your saved sign-in \xE2\x80\x94 you can continue as a guest.");
    } else if (st == Account::State::Failed) {
        ImGui::TextColored(th.warn, "Not signed in \xE2\x80\x94 %s", p.account.error().c_str());
    }
}

void draw_signin(HubModel& hub, const Theme& th) {
    Page& p = page();
    const Account::State st = p.account.state();
    if (st == Account::State::SignedIn) {
        enter_online(hub);
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 640.f);
    ImGui::TextUnformatted("Signing in with Discord gives you a name that is yours across sessions "
                           "and devices, on this server and any other that uses it.");
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(th.text_muted,
                       "Online play on this server needs an account. LAN and Direct IP do not "
                       "\xE2\x80\x94 go back and pick those to play without signing in.");
    ImGui::Dummy(ImVec2(0, 12));
    if (st == Account::State::Waiting) {
        ImGui::TextColored(th.accent, "Waiting for Discord\xE2\x80\xA6");
        ImGui::TextUnformatted("Finish signing in on the page that opened in your browser. This "
                               "screen will move on by itself.");
        if (now_s() - p.account.waiting_since() > 10.0) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextColored(th.text_muted, "Still waiting. If the browser never opened, or you "
                                              "closed the page, start again:");
            if (accent_button("Retry Discord Sign In", th, ImVec2(240, 36))) p.account.relogin();
        }
    } else {
        if (accent_button("Sign in with Discord", th, ImVec2(240, 40))) p.account.login();
        if (st == Account::State::Failed)
            ImGui::TextColored(th.warn, "%s", p.account.error().c_str());
    }
    ImGui::PopTextWrapPos();
}

// The game filter: All games, then the netplay games installed here.
void draw_filter(HubModel& hub, const Theme& th, const std::vector<Game>& games) {
    Page& p = page();
    if (p.scope) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(th.accent, "%s", p.scope->title.c_str());
        ImGui::SameLine();
        ImGui::TextColored(th.text_muted, "\xC2\xB7 %s", p.scope->pin.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This build's identity: only players with exactly this game, core "
                              "and package can join.");
        return;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(th.text_muted, "Game");
    ImGui::SameLine();
    const std::string label = p.filter.empty() ? "All games" : title_for(hub, p.filter);
    ImGui::SetNextItemWidth(320.f);
    if (ImGui::BeginCombo("##np_filter", label.c_str())) {
        if (ImGui::Selectable("All games", p.filter.empty())) set_filter(hub, "", games);
        if (games.empty()) ImGui::TextDisabled("No netplay games installed");
        for (const Game& g : games) {
            const std::string item = g.title + "  \xC2\xB7  " + g.pin + "##" + g.game_name;
            if (ImGui::Selectable(item.c_str(), p.filter == g.game_name))
                set_filter(hub, g.game_name, games);
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show every game's lobbies, or one installed game's. Server chat and "
                          "players online follow the game you pick.");
}

// Whether this machine can sit in a room for `game` at `version`, and why not.
std::string join_blocker(const HubModel& hub, const std::vector<Game>& games,
                         const std::string& game, const std::string& version, const Game** out) {
    const Game* g = game_by_wire_name(games, game);
    if (out) *out = g;
    if (!g) return "Install " + title_for(hub, game) + " to join this lobby.";
    if (!netplay_versions_equal(g->pin, version))
        return "This lobby runs version " + version + "; your " + g->title + " is " + g->pin + ".";
    return {};
}

// ---- library games: the hub starts them into its matches -------------------------------

// Resolves a library game for a room and, when its build can take a hub
// match, asks it for its identity on a worker. Called on every join or create,
// so the answer reflects the player's BIOS and settings as they are now.
std::unique_ptr<Page::LibGame> make_lib_game(HubModel& hub, const Game& g) {
    auto lib = std::make_unique<Page::LibGame>();
    lib->game_name = g.game_name;
    const Title* t = hub.catalog.find(g.catalog_id);
    if (!t) t = hub.catalog.find_by_netplay_game_name(g.game_name);
    if (!t) {
        lib->why = g.title + " is not in the catalog.";
        return lib;
    }
    lib->title = *t;
    lib->opts = hub.play_launch_options(*t);
    lib->work_dir = hub.paths.data_dir / "netplay" / t->id;
    LaunchOptions probe = lib->opts;
    probe.mode = LaunchMode::Netplay;
    probe.netplay_query = lib->work_dir / "probe.json";
    const LaunchPlan plan = plan_launch(hub.paths, *t, probe);
    if (!plan.ready) {
        lib->why = "Retro cannot start " + t->name + ": " +
                   plan.message.substr(0, plan.message.find('\n'));
        return lib;
    }
    if (!launch_supports_netplay_handoff(plan)) {
        lib->why = "This build of " + t->name +
                   " cannot be started by Retro: update the game, or start the match "
                   "from its own Netplay menu.";
        return lib;
    }
    lib->capable = true;
    json req = {{"v", 1}, {"handoff_query", "identity"}};
    if (!lib->opts.rom_path.empty()) req["handoff_disc"] = lib->opts.rom_path.string();
    lib->identity_f = std::async(std::launch::async,
                                 [paths = hub.paths, title = lib->title, opts = lib->opts,
                                  dir = lib->work_dir, req]() {
                                     return np::run_handoff_query(paths, title, opts, req, dir);
                                 });
    return lib;
}

bool future_ready(const std::future<np::QueryAnswer>& f) {
    return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// The create or join itself, with the fields the game adds to its own.
void send_seat(const Page::PendingSeat& ps, const json& join_fields) {
    Page& p = page();
    const json extra = join_fields.is_object() ? join_fields : json::object();
    if (ps.create) {
        json caps = json::object();
        if (p.lib && p.lib->identity.is_object()) {
            const json& start = p.lib->identity.value("start", json::object());
            if (start.is_object() && start.contains("match_caps")) caps = start["match_caps"];
        }
        p.client.create_for(ps.game_name, ps.version, ps.name, ps.password, ps.max_slots,
                            ps.spectators, caps, extra);
        p.status = "Creating lobby\xE2\x80\xA6";
    } else {
        p.client.join_as(ps.lobby_id, ps.password, ps.game_name, ps.version, extra);
        p.status = "Joining\xE2\x80\xA6";
    }
    if (p.filter.empty()) p.scope_dirty = true;
}

// A join or create from the library: through the game's identity when its
// build takes hub matches, as before when it does not (it can sit, not play).
void begin_seat(HubModel& hub, const Game& g, Page::PendingSeat ps) {
    Page& p = page();
    if (p.scope) {
        send_seat(ps, json::object());
        return;
    }
    p.lib = make_lib_game(hub, g);
    if (!p.lib->capable) {
        send_seat(ps, json::object());
        return;
    }
    p.pending_seat = std::move(ps);
    p.status = "Asking " + p.lib->title.name + " for its netplay settings\xE2\x80\xA6";
}

void join_room(HubModel& hub, const std::vector<Game>& games, const np::LobbyRow& r) {
    Page& p = page();
    const Game* g = nullptr;
    const std::string why = join_blocker(hub, games, r.game_name, r.game_version, &g);
    if (!why.empty()) {
        p.status = why;
        return;
    }
    if (r.has_password) {
        p.join_pw_lobby = r.lobby_id;
        p.join_pw_game = g->game_name;
        p.join_pw_version = g->pin;
        p.join_pw[0] = '\0';
        p.open_join_pw = true;
        return;
    }
    Page::PendingSeat ps;
    ps.lobby_id = r.lobby_id;
    ps.game_name = g->game_name;
    ps.version = g->pin;
    begin_seat(hub, *g, std::move(ps));
}

void open_host_modal(HubModel& hub, const std::vector<Game>& games) {
    Page& p = page();
    if (games.empty()) {
        p.status = "Install a game that supports netplay to host a lobby.";
        return;
    }
    p.host_game = 0;
    for (size_t i = 0; i < games.size(); ++i)
        if (games[i].game_name == p.filter) p.host_game = static_cast<int>(i);
    std::string n = local_name(hub);
    copy_to(p.host_name, sizeof(p.host_name), (n.empty() ? std::string("My") : n + "'s") + " Lobby");
    p.host_pw[0] = '\0';
    p.host_max = 2;
    p.host_spectators = false;
    p.host_tpak = false;
    p.host_error.clear();
    p.open_host = true;
}

void draw_players_panel(HubModel& hub, const Theme& th) {
    Page& p = page();
    int shown = 0;
    for (const np::OnlinePlayer& o : p.snap.players)
        if (o.is_local || !p.mod.blocked(o.account)) ++shown;
    ImGui::TextColored(th.accent, "PLAYERS ONLINE");
    ImGui::SameLine();
    ImGui::TextColored(th.text_muted, "%d", shown);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(p.filter.empty() ? "Everyone on the lobby server."
                                           : "Players on the lobby server for this game.");
    if (shown <= 1 && p.snap.players.size() <= 1) {
        ImGui::TextColored(th.text_muted, p.filter.empty() ? "Nobody else is online."
                                                           : "Nobody else is online for this game.");
        return;
    }
    if (!ImGui::BeginTable("##np_online", 2,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_BordersInnerH))
        return;
    ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed, 84.f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    int i = 0;
    for (const np::OnlinePlayer& o : p.snap.players) {
        if (!o.is_local && p.mod.blocked(o.account)) continue;
        ImGui::PushID(i++);
        ImGui::TableNextRow(0, 26.f);
        ImGui::TableNextColumn();
        if (!o.country.empty()) {
            ImGui::TextColored(th.text_muted, "%s", o.country.c_str());
            ImGui::SameLine();
        }
        if (o.is_local) {
            ImGui::TextColored(th.accent, "%s", o.display_name.c_str());
            ImGui::SameLine();
            ImGui::TextColored(th.text_muted, "(you)");
        } else {
            ImGui::TextUnformatted(o.display_name.c_str());
            if (ImGui::BeginPopupContextItem("who")) {
                player_menu(hub, th, o.display_name, o.account, {});
                ImGui::EndPopup();
            }
            if (p.mod.hidden(o.account)) {
                ImGui::SameLine();
                ImGui::TextColored(th.text_muted, "(ignored)");
            }
        }
        ImGui::TableNextColumn();
        const std::string game = o.game_name.empty() ? std::string() : title_for(hub, o.game_name);
        if (o.hosting) ImGui::TextColored(th.good, "Hosting");
        else if (!o.lobby_id.empty()) ImGui::TextUnformatted("In lobby");
        else ImGui::TextColored(th.text_muted, "Browsing");
        if (ImGui::IsItemHovered() && (!o.lobby_name.empty() || !game.empty()))
            ImGui::SetTooltip("%s%s%s", o.lobby_name.c_str(),
                              !o.lobby_name.empty() && !game.empty() ? " \xC2\xB7 " : "",
                              game.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void draw_online(HubModel& hub, const Theme& th, const std::vector<Game>& games) {
    Page& p = page();
    const np::Snapshot& s = p.snap;
    const bool all = p.filter.empty();

    // Toolbar: filter, connection state.
    draw_filter(hub, th, games);
    ImGui::SameLine(0, 24.f);
    if (s.state == np::ConnState::Connected) {
        ImGui::TextColored(th.text_muted, "%s%s", s.signed_in ? "Signed in \xC2\xB7 " : "Guest \xC2\xB7 ",
                           s.display_name.c_str());
        if (s.rtt_ms >= 0) {
            ImGui::SameLine();
            ImGui::TextColored(th.text_muted, "\xC2\xB7 server %d ms", s.rtt_ms);
        }
    } else if (s.state == np::ConnState::Connecting) {
        ImGui::TextColored(th.text_muted, "Connecting\xE2\x80\xA6");
    } else {
        ImGui::TextColored(th.warn, "Could not reach lobby server.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Retry")) {
            p.account.restore();
            connect(hub);
        }
        if (!s.transport_error.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", s.transport_error.c_str());
    }

    const float footer_h = 46.f + ImGui::GetStyle().ItemSpacing.y * 2.f;
    const float avail_h = ImGui::GetContentRegionAvail().y - footer_h;
    const float gap = 10.f;
    const float chat_h = 230.f;
    const float top_h = avail_h > chat_h + 200.f ? avail_h - chat_h - gap : avail_h * 0.6f;
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const bool side_panel = avail_w >= 300.f + 20.f + 560.f;

    ImGui::BeginChild("##np_lobbies", ImVec2(side_panel ? avail_w - 320.f : 0, top_h));
    ImGui::TextColored(th.accent, "LOBBIES");
    if (!p.status.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(th.warn, "%s", p.status.c_str());
    }
    const int cols = all ? 6 : 5;
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
    if (ImGui::BeginTable("##np_lobby_table", cols,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Lobby", ImGuiTableColumnFlags_WidthStretch);
        if (all) ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthFixed, 190.f);
        ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 64.f);
        ImGui::TableSetupColumn("Spectators", ImGuiTableColumnFlags_WidthFixed, 82.f);
        ImGui::TableSetupColumn("Latency", ImGuiTableColumnFlags_WidthFixed, 64.f);
        ImGui::TableSetupColumn("Join", ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        if (s.rooms.empty()) {
            ImGui::TableNextRow(0, 32.f);
            ImGui::TableNextColumn();
            ImGui::TextColored(th.text_muted, "No lobbies yet - host one.");
        }
        for (const np::LobbyRow& r : s.rooms) {
            ImGui::PushID(r.lobby_id.c_str());
            ImGui::TableNextRow(0, 32.f);
            ImGui::TableNextColumn();
            const bool sel = p.selected_lobby == r.lobby_id;
            std::string label = (r.host_country.empty() ? "" : r.host_country + "  ") +
                                (r.name.empty() ? "Unnamed lobby" : r.name) +
                                (r.has_password ? "  [locked]" : "");
            if (ImGui::Selectable(label.c_str(), sel,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap |
                                      ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(0, ImGui::GetFrameHeight()))) {
                p.selected_lobby = r.lobby_id;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) join_room(hub, games, r);
            }
            if (all) {
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(title_for(hub, r.game_name).c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Version %s", r.game_version.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d/%d", r.player_count, r.max_slots);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (r.allow_spectators) ImGui::Text("%d/%d", r.spectator_count, r.max_spectators);
            else ImGui::TextColored(th.text_muted, "No");
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(th.text_muted, "\xE2\x80\x94");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Latency to the host is measured by the game, not by Retro.");
            ImGui::TableNextColumn();
            const std::string why = join_blocker(hub, games, r.game_name, r.game_version, nullptr);
            ImGui::BeginDisabled(!why.empty());
            if (ImGui::Button("Join", ImVec2(64, 0))) join_room(hub, games, r);
            ImGui::EndDisabled();
            if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", why.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    if (side_panel) {
        ImGui::SameLine(0, 20.f);
        ImGui::BeginChild("##np_online_panel", ImVec2(300.f, top_h));
        draw_players_panel(hub, th);
        ImGui::EndChild();
    }

    // Server chat: one game's players, so only with a game picked.
    ImGui::BeginChild("##np_server_chat", ImVec2(0, avail_h - top_h - gap));
    ChatBinding b{"server_chat", "SERVER CHAT",
                  "Everyone online for this game sees this \xE2\x80\x94 no history, only what is said "
                  "while you are here.",
                  "Message everyone playing this game\xE2\x80\xA6", &s.server_chat, &p.server_chat_seen,
                  p.server_chat_in, sizeof(p.server_chat_in),
                  !all && s.state == np::ConnState::Connected,
                  all ? "Server chat is per game: pick one in the Game filter to talk with its players."
                      : "Server chat is not available right now."};
    draw_chat_panel(hub, th, b, [&](const std::string& t) { p.client.server_chat(t); });
    ImGui::EndChild();

    // Footer.
    ImGui::Separator();
    if (accent_button("Host Lobby", th, ImVec2(190, 46))) open_host_modal(hub, games);
    ImGui::SameLine();
    if (ImGui::Button("Network Settings", ImVec2(170, 46))) {
        copy_to(p.url_buf, sizeof(p.url_buf), lobby_url(hub));
        p.open_net = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(120, 46))) {
        if (s.state == np::ConnState::Connected) p.client.request_list();
        else connect(hub);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reload server lobbies");
}

void draw_lan(HubModel& hub, const Theme& th, const std::vector<Game>& games) {
    Page& p = page();
    auto rooms = p.lan.rooms();
    if (p.scope)
        rooms.erase(std::remove_if(rooms.begin(), rooms.end(),
                                   [&](const np::LanRoom& r) {
                                       return r.game_name != p.scope->game_name;
                                   }),
                    rooms.end());
    const float footer_h = 46.f + ImGui::GetStyle().ItemSpacing.y * 2.f;
    ImGui::TextColored(th.accent, "LAN LOBBIES");
    ImGui::SameLine();
    ImGui::TextColored(th.text_muted, p.scope ? "found on your network" : "found on your network, every game");
    const std::string lerr = p.lan.listen_error();
    if (!lerr.empty())
        ImGui::TextColored(th.warn, "Not listening for recomp-net rooms: %s. PlayStation rooms are "
                                    "still found.", lerr.c_str());
    if (!p.status.empty()) ImGui::TextColored(th.warn, "%s", p.status.c_str());

    ImGui::BeginChild("##np_lan", ImVec2(0, ImGui::GetContentRegionAvail().y - footer_h));
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
    if (ImGui::BeginTable("##np_lan_table", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Lobby", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthFixed, 200.f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 150.f);
        ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 64.f);
        ImGui::TableSetupColumn("Join", ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        if (rooms.empty()) {
            ImGui::TableNextRow(0, 32.f);
            ImGui::TableNextColumn();
            ImGui::TextColored(th.text_muted, "No LAN lobbies found yet. Hosts on your network show "
                                              "up here while their waiting room is open.");
        }
        for (size_t i = 0; i < rooms.size(); ++i) {
            const np::LanRoom& r = rooms[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow(0, 32.f);
            ImGui::TableNextColumn();
            const std::string label = "LAN - " + (r.name.empty() ? std::string("Unnamed lobby") : r.name) +
                                      (r.has_password ? "  [locked]" : "") +
                                      (r.direct ? "  [direct]" : "");
            if (ImGui::Selectable(label.c_str(), p.selected_lan == static_cast<int>(i),
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, ImGui::GetFrameHeight())))
                p.selected_lan = static_cast<int>(i);
            ImGui::TableNextColumn();
            const Game* g = game_by_wire_name(games, r.game_name);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(g ? g->title.c_str() : title_for(hub, r.game_name).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s%s%s", r.proto == np::LanRoom::Proto::Psx ? "PlayStation LAN lobby"
                                                                              : "recomp-net LAN lobby",
                                  r.game_version.empty() ? "" : " \xC2\xB7 version ",
                                  r.game_version.c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(th.text_muted, "%s", r.endpoint.c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d/%d", r.players, r.max_slots);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(true);
            ImGui::Button("Join", ImVec2(64, 0));
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("A LAN room is held by the host's game, and a seat is taken by "
                                  "yours. Join from %s's own Netplay menu (LAN / Direct IP): "
                                  "Retro can't hand a seat to a game yet.",
                                  g ? g->title.c_str() : "the game");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::BeginDisabled(true);
    accent_button("Host Lobby", th, ImVec2(190, 46));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A LAN lobby lives in the host's game (it owns the game's UDP port). "
                          "Host from the game's own Netplay menu for now.");
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(120, 46))) p.lan.refresh();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rescan LAN/Direct IP");
    ImGui::SameLine();
    if (ImGui::Button("Join Direct", ImVec2(170, 46))) {
        p.direct_status.clear();
        p.open_direct = true;
    }
}

// One seat row. `m` is null for an open seat.
void seat_row(HubModel& hub, const Theme& th, int slot, const np::Member* m, bool spectator) {
    Page& p = page();
    const np::Snapshot& s = p.snap;
    ImGui::PushID(slot + (spectator ? 1000 : 0));
    ImGui::TableNextRow(0, 42.f);
    ImGui::TableNextColumn();
    const bool mine = m && m->is_local;
    const bool can_drag = !spectator && m && (s.is_host || mine);
    ImGui::BeginDisabled(!can_drag);
    ImGui::Button("##grip", ImVec2(24, 32));
    ImGui::EndDisabled();
    {
        // Three bars: no glyph for a grip is in every font the hub loads.
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f;
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(can_drag ? th.text : th.border);
        for (int i = -1; i <= 1; ++i)
            ImGui::GetWindowDrawList()->AddLine(ImVec2(cx - 6.f, cy + 5.f * i),
                                                ImVec2(cx + 6.f, cy + 5.f * i), col, 2.f);
    }
    if (can_drag && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("NETPLAY_MEMBER_SLOT", &slot, sizeof(slot));
        ImGui::Text("Move %s", m->display_name.c_str());
        ImGui::EndDragDropSource();
    }
    auto drop_here = [&] {
        if (spectator || !ImGui::BeginDragDropTarget()) return;
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("NETPLAY_MEMBER_SLOT")) {
            const int from = *static_cast<const int*>(pl->Data);
            if (from == slot) {
            } else if (s.is_host) {
                p.client.move_slot(from, slot);
            } else if (!m) {
                p.client.seat_move(slot);
            } else {
                p.client.seat_swap_request(slot);
                p.room_status = "Waiting for the other player to accept the seat swap\xE2\x80\xA6";
            }
        }
        ImGui::EndDragDropTarget();
    };
    ImGui::TableNextColumn();
    ImGui::Text("%s%d", spectator ? "S" : "P", slot + 1);
    drop_here();
    ImGui::TableNextColumn();
    if (m) {
        if (!m->country.empty()) {
            ImGui::TextColored(th.text_muted, "%s", m->country.c_str());
            ImGui::SameLine();
        }
        ImGui::TextColored(mine ? th.good : th.text, "%s", m->display_name.c_str());
        if (!mine && ImGui::BeginPopupContextItem("who")) {
            player_menu(hub, th, m->display_name, m->account, {});
            ImGui::EndPopup();
        }
    } else {
        ImGui::TextColored(th.text_muted, spectator ? "Open seat" : "Open slot");
    }
    drop_here();
    ImGui::TableNextColumn();
    if (m) {
        if (spectator) ImGui::TextColored(th.text_muted, "Watching");
        else if (m->is_host) ImGui::TextColored(th.accent, "Host");
        else ImGui::TextUnformatted(m->ready ? "Connected" : "Waiting");
    }
    ImGui::TableNextColumn();
    if (m) ImGui::TextColored(th.text_muted, "\xE2\x80\x94");
    ImGui::TableNextColumn();
    if (m && !mine) {
        const char* why = !s.is_host ? "Only the host can kick" : m->is_host ? "Cannot kick the host" : nullptr;
        ImGui::BeginDisabled(why != nullptr);
        if (danger_button("Kick", th, ImVec2(-FLT_MIN, 32))) p.client.kick(slot);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", why ? why : "Kick player");
    }
    ImGui::PopID();
}

// Every seated player's pak has arrived (a Transfer Pak lobby's PLAY gate).
bool paks_complete(const np::Snapshot& s) {
    const Page& p = page();
    for (const np::Member& m : s.members)
        if (!m.is_spectator && !p.paks.count(m.player_id)) return false;
    return true;
}

void draw_room(HubModel& hub, const Theme& th, const std::vector<Game>& games) {
    Page& p = page();
    const np::Snapshot& s = p.snap;
    (void)games;

    // Which game: the row we joined from, or the server's echo.
    std::string game_label, version;
    for (const np::LobbyRow& r : s.rooms)
        if (r.lobby_id == s.lobby_id) {
            game_label = title_for(hub, r.game_name);
            version = r.game_version;
        }

    ImGui::TextColored(th.accent, "%s", s.room_name.empty() ? "Lobby" : s.room_name.c_str());
    if (!game_label.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(th.text_muted, "\xC2\xB7 %s %s", game_label.c_str(), version.c_str());
    }
    const bool tpak_room = s.match_caps.is_object() && s.match_caps.value("tpak", false);
    ImGui::PushTextWrapPos(0.f);
    if (p.scope) {
        ImGui::TextColored(th.text_muted, "Transfer Paks: %s. The host's PLAY starts the match "
                                          "here on every player's machine.",
                           tpak_room ? "on" : "off");
    } else if (p.lib && p.lib->capable) {
        ImGui::TextColored(th.text_muted, "The host's PLAY starts %s on every player's machine, "
                                          "into this match.",
                           p.lib->title.name.c_str());
    } else {
        ImGui::TextColored(th.warn, "%s",
                           p.lib && !p.lib->why.empty()
                               ? p.lib->why.c_str()
                               : "Retro cannot start this game into a match: update the game, "
                                 "or start the match from its own Netplay menu.");
    }
    // This machine cannot take a seat in any match (a VRU Microphone seat):
    // said here, before the host's PLAY lands and the launch is refused.
    if (p.scope && p.scope->match_problem) {
        if (const std::string mw = p.scope->match_problem(); !mw.empty())
            ImGui::TextColored(th.warn, "%s", mw.c_str());
    }
    if (tpak_room && p.scope) {
        std::string line = "Transfer Paks:";
        bool all = true;
        for (const np::Member& m : s.members) {
            if (m.is_spectator) continue;
            const auto it = p.paks.find(m.player_id);
            line += "  P" + std::to_string(m.slot + 1) + " ";
            if (it == p.paks.end()) {
                line += "\xE2\x80\xA6";
                all = false;
            } else {
                line += it->second.cart < 0 ? "no pak" : supported_gb_rom(it->second.cart).label;
            }
        }
        ImGui::TextColored(all ? th.good : th.text_muted, "%s", line.c_str());
        if (!p.pak_note.empty()) ImGui::TextColored(th.warn, "Your pak: %s", p.pak_note.c_str());
    }
    if (p.scope && !p.relay_note.empty())
        ImGui::TextColored(p.relay_note_bad ? th.warn : th.text_muted, "%s", p.relay_note.c_str());
    ImGui::PopTextWrapPos();

    const float footer_h = 46.f + ImGui::GetStyle().ItemSpacing.y * 2.f;
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float body_h = ImGui::GetContentRegionAvail().y - footer_h;
    const bool split = avail_w >= 380.f + 20.f + 520.f;
    const float seats_w = split ? avail_w - 400.f : 0.f;
    const float seats_h = split ? body_h : body_h - 320.f;

    ImGui::BeginChild("##np_seats", ImVec2(seats_w, seats_h));
    int seated = 0;
    for (const np::Member& m : s.members)
        if (!m.is_spectator) ++seated;
    ImGui::TextColored(th.accent, "PLAYERS");
    ImGui::SameLine();
    ImGui::TextColored(th.text_muted, "  %d / %d seated", seated, s.max_slots);
    if (!p.room_status.empty()) ImGui::TextColored(th.warn, "%s", p.room_status.c_str());
    if (ImGui::BeginTable("##np_seat_table", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("##move", ImGuiTableColumnFlags_WidthFixed, 32.f);
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 40.f);
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.f);
        ImGui::TableSetupColumn("Latency", ImGuiTableColumnFlags_WidthFixed, 72.f);
        ImGui::TableSetupColumn("Kick", ImGuiTableColumnFlags_WidthFixed, 64.f);
        ImGui::TableHeadersRow();
        const int seats = std::max(s.max_slots, seated);
        for (int slot = 0; slot < seats; ++slot) {
            const np::Member* m = nullptr;
            for (const np::Member& x : s.members)
                if (!x.is_spectator && x.slot == slot) m = &x;
            seat_row(hub, th, slot, m, false);
        }
        ImGui::EndTable();
    }
    bool any_spec = false;
    for (const np::Member& m : s.members) any_spec = any_spec || m.is_spectator;
    if (any_spec && ImGui::BeginTable("##np_spec_table", 6,
                                      ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("##move", ImGuiTableColumnFlags_WidthFixed, 32.f);
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 40.f);
        ImGui::TableSetupColumn("Spectator", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.f);
        ImGui::TableSetupColumn("Latency", ImGuiTableColumnFlags_WidthFixed, 72.f);
        ImGui::TableSetupColumn("Kick", ImGuiTableColumnFlags_WidthFixed, 64.f);
        ImGui::TableHeadersRow();
        for (const np::Member& m : s.members)
            if (m.is_spectator) seat_row(hub, th, m.slot, &m, true);
        ImGui::EndTable();
    }
    ImGui::TextColored(th.text_muted, s.is_host ? "Drag a row to move a player to another seat."
                                                : "Drag your row to an open seat, or onto a player "
                                                  "to ask to swap.");
    ImGui::EndChild();
    if (split) ImGui::SameLine(0, 20.f);
    ImGui::BeginChild("##np_room_chat", ImVec2(0, split ? body_h : 320.f));
    ChatBinding b{"room_chat", "CHAT", "Say hello \xE2\x80\x94 everyone in the room sees this.",
                  "Message the lobby\xE2\x80\xA6", &s.room_chat, &p.room_chat_seen, p.room_chat_in,
                  sizeof(p.room_chat_in), s.state == np::ConnState::Connected,
                  "Chat is not available in this room."};
    draw_chat_panel(hub, th, b, [&](const std::string& t) { p.client.chat(t); });
    ImGui::EndChild();

    ImGui::Separator();
    if (danger_button("Leave Lobby", th, ImVec2(150, 46))) {
        if (s.is_host) p.client.close_room();
        else p.client.leave();
    }
    ImGui::SameLine();
    if (ImGui::Button(s.is_host ? "Settings" : "Room Info", ImVec2(130, 46))) p.open_room_info = true;
    ImGui::SameLine();
    const float play_w = 210.f;
    ImGui::SameLine(0, std::max(ImGui::GetStyle().ItemSpacing.x,
                                ImGui::GetContentRegionAvail().x - play_w));
    if (s.is_host) {
        const Page::LibGame* lib = p.scope ? nullptr : p.lib.get();
        const std::string lib_why =
            p.scope ? std::string()
            : !lib  ? std::string("Retro cannot start this game into a match")
            : !lib->capable ? lib->why
            : !lib->identity.is_object() ? "Preparing " + lib->title.name + "\xE2\x80\xA6"
            : lib->settle_f.valid() ? std::string("Settling the match\xE2\x80\xA6")
                                    : std::string();
        const std::string match_why =
            p.scope && p.scope->match_problem ? p.scope->match_problem() : std::string();
        const char* why = seated < 2   ? "Waiting for another player to join"
                          : !lib_why.empty() ? lib_why.c_str()
                          : !match_why.empty() ? match_why.c_str()
                          : tpak_room && !paks_complete(s)
                                         ? "Waiting for every player's Transfer Pak"
                                         : nullptr;
        ImGui::BeginDisabled(why != nullptr);
        if (good_button("PLAY", th, ImVec2(play_w, 46))) {
            if (p.scope) {
                p.client.start_match();
                p.room_status = "Starting the match\xE2\x80\xA6";
            } else {
                // The game settles the match from the room, as its own PLAY
                // would; the start message it returns is sent as it is.
                const json req = np::handoff_settle_request(s);
                p.lib->settle_f = std::async(
                    std::launch::async, [paths = hub.paths, title = p.lib->title,
                                         opts = p.lib->opts, dir = p.lib->work_dir, req]() {
                        return np::run_handoff_query(paths, title, opts, req, dir);
                    });
                p.room_status = "Settling the match\xE2\x80\xA6";
            }
        }
        ImGui::EndDisabled();
        if (why && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", why);
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(th.text_muted, "Waiting for the host to start\xE2\x80\xA6");
    }
}

// ---- modals ---------------------------------------------------------------------------------

bool begin_modal(const char* id, bool* open_flag, float width) {
    if (*open_flag) {
        ImGui::OpenPopup(id);
        *open_flag = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(width, vp->WorkSize.x - 40.f), 0.f));
    return ImGui::BeginPopupModal(id, nullptr,
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
}

bool cancel_pressed() {
    return ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
           ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
}

void draw_modals(HubModel& hub, const Theme& th, const std::vector<Game>& games) {
    Page& p = page();

    if (begin_modal("Host Lobby###np_host", &p.open_host, 520.f)) {
        if (!games.empty()) {
            p.host_game = std::clamp(p.host_game, 0, static_cast<int>(games.size()) - 1);
            const Game& g = games[static_cast<size_t>(p.host_game)];
            ImGui::TextUnformatted("Game");
            if (p.scope) {
                // Direct mode: this title, and only it.
                ImGui::TextColored(th.accent, "%s", g.title.c_str());
            } else {
                ImGui::SetNextItemWidth(430.f);
                if (ImGui::BeginCombo("##host_game", g.title.c_str())) {
                    for (size_t i = 0; i < games.size(); ++i)
                        if (ImGui::Selectable(games[i].title.c_str(), p.host_game == static_cast<int>(i)))
                            p.host_game = static_cast<int>(i);
                    ImGui::EndCombo();
                }
            }
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextColored(th.text_muted, "Version %s \xC2\xB7 guests need the same build",
                               g.pin.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextUnformatted("Lobby name");
            ImGui::SetNextItemWidth(430.f);
            ImGui::InputText("##host_name", p.host_name, sizeof(p.host_name));
            ImGui::TextUnformatted("Max Players");
            const int top = std::min(8, g.max_slots);
            p.host_max = std::clamp(p.host_max, 2, std::max(2, top));
            ImGui::BeginDisabled(top <= 2);
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::BeginCombo("##host_max", std::to_string(p.host_max).c_str())) {
                for (int n = 2; n <= top; ++n)
                    if (ImGui::Selectable(std::to_string(n).c_str(), p.host_max == n)) p.host_max = n;
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if (top <= 2) {
                ImGui::SameLine();
                ImGui::TextColored(th.text_muted, "(this game is 2-player)");
            }
            ImGui::Checkbox("Allow Spectators", &p.host_spectators);
            ImGui::SameLine();
            ImGui::TextColored(th.text_muted, "up to 4, on top of the players");
            if (p.scope && p.scope->tpak_supported) {
                ImGui::Checkbox("Transfer Paks", &p.host_tpak);
                ImGui::SameLine();
                ImGui::TextColored(th.text_muted, "every player brings Red, Blue and Yellow");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("A Transfer Pak lobby needs all three Game Boy cartridges "
                                      "set in Transfer Pak Support, on every player's machine.");
            }
            ImGui::TextUnformatted("Password (optional)");
            ImGui::SetNextItemWidth(430.f);
            ImGui::InputText("##host_pw", p.host_pw, sizeof(p.host_pw), ImGuiInputTextFlags_Password);
            if (!p.host_error.empty()) ImGui::TextColored(th.warn, "%s", p.host_error.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            if (ImGui::Button("Cancel", ImVec2(120, 0)) || cancel_pressed()) ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            ImGui::BeginDisabled(p.snap.state != np::ConnState::Connected);
            if (accent_button("Create Lobby", th, ImVec2(160, 0))) {
                const std::string name = trim(p.host_name);
                p.host_error = name_problem(name, "lobby name");
                if (p.host_error.empty() && p.host_tpak && p.scope && p.scope->tpak_problem)
                    p.host_error = p.scope->tpak_problem();
                if (p.host_error.empty() && p.scope) {
                    json caps = json::object();
                    caps["tpak"] = p.host_tpak;
                    caps["relay"] = "host"; // the lobby server's relay only as a fallback
                    p.client.create_for(g.game_name, g.pin, name, p.host_pw, p.host_max,
                                        p.host_spectators, caps);
                    if (p.filter.empty()) p.scope_dirty = true;
                    p.status = "Creating lobby\xE2\x80\xA6";
                    ImGui::CloseCurrentPopup();
                } else if (p.host_error.empty()) {
                    // The library: the room carries the game's own match caps.
                    Page::PendingSeat ps;
                    ps.create = true;
                    ps.game_name = g.game_name;
                    ps.version = g.pin;
                    ps.name = name;
                    ps.password = p.host_pw;
                    ps.max_slots = p.host_max;
                    ps.spectators = p.host_spectators;
                    begin_seat(hub, g, std::move(ps));
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }

    if (begin_modal("Join Lobby###np_join_pw", &p.open_join_pw, 400.f)) {
        ImGui::TextUnformatted("Password");
        ImGui::SetNextItemWidth(320.f);
        const bool enter = ImGui::InputText("##join_pw", p.join_pw, sizeof(p.join_pw),
                                            ImGuiInputTextFlags_Password |
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Cancel", ImVec2(120, 0)) || cancel_pressed()) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (accent_button("Join", th, ImVec2(120, 0)) || enter) {
            Page::PendingSeat ps;
            ps.lobby_id = p.join_pw_lobby;
            ps.password = p.join_pw;
            ps.game_name = p.join_pw_game;
            ps.version = p.join_pw_version;
            if (const Game* g = game_by_wire_name(games, p.join_pw_game))
                begin_seat(hub, *g, std::move(ps));
            else
                send_seat(ps, json::object());
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (begin_modal("Player Name###np_name", &p.open_name, 440.f)) {
        if (ImGui::IsWindowAppearing()) {
            copy_to(p.name_buf, sizeof(p.name_buf), local_name(hub));
            p.name_error.clear();
        }
        const Account::State st = p.account.state();
        if (p.account.offered()) {
            if (st == Account::State::SignedIn) {
                const auto info = p.account.info();
                ImGui::TextColored(th.good, "Signed in as %s", info.handle.c_str());
                if (!info.discord_username.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(th.text_muted, "@%s", info.discord_username.c_str());
                }
                if (ImGui::Button("Sign out", ImVec2(120, 0))) {
                    p.account.sign_out();
                    if (p.client_started) connect(hub);
                }
            } else if (st == Account::State::Waiting) {
                ImGui::TextColored(th.accent, "Waiting for Discord\xE2\x80\xA6");
                ImGui::TextWrapped("Finish signing in on the page that opened in your browser, then "
                                   "come back here.");
            } else {
                ImGui::TextWrapped("Sign in with Discord so your name is yours across sessions. "
                                   "Optional \xE2\x80\x94 you can play as a guest.");
                if (accent_button("Sign in with Discord", th, ImVec2(200, 0))) p.account.login();
                if (st == Account::State::Failed)
                    ImGui::TextColored(th.warn, "%s", p.account.error().c_str());
            }
            ImGui::Separator();
        }
        const bool signed_in = st == Account::State::SignedIn;
        ImGui::TextUnformatted(signed_in ? "Your name comes from your account." : "Player name");
        ImGui::BeginDisabled(signed_in);
        ImGui::SetNextItemWidth(320.f);
        ImGui::InputText("##np_name_in", p.name_buf, sizeof(p.name_buf));
        ImGui::EndDisabled();
        if (!p.name_error.empty()) ImGui::TextColored(th.warn, "%s", p.name_error.c_str());
        if (ImGui::Button("Cancel", ImVec2(120, 0)) || cancel_pressed()) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (!signed_in && accent_button("Save", th, ImVec2(120, 0))) {
            const std::string n = trim(p.name_buf);
            p.name_error = name_problem(n, "name");
            if (p.name_error.empty()) {
                hub.cfg = load_app_config(hub.paths.config_path);
                hub.cfg.netplay.display_name = n;
                save_app_config(hub.paths.config_path, hub.cfg);
                if (p.client_started) p.client.set_display_name(n);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    if (begin_modal("Network Settings###np_net", &p.open_net, 520.f)) {
        ImGui::TextUnformatted("Lobby server URL");
        ImGui::SetNextItemWidth(440.f);
        ImGui::InputText("##np_url", p.url_buf, sizeof(p.url_buf));
        ImGui::TextColored(th.text_muted, "Default: %s", kDefaultNetplayLobbyUrl);
        if (ImGui::Button("Cancel", ImVec2(120, 0)) || cancel_pressed()) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (accent_button("Save", th, ImVec2(120, 0))) {
            hub.cfg = load_app_config(hub.paths.config_path);
            hub.cfg.netplay.lobby_url = trim(p.url_buf);
            if (hub.cfg.netplay.lobby_url.empty()) hub.cfg.netplay.lobby_url = kDefaultNetplayLobbyUrl;
            save_app_config(hub.paths.config_path, hub.cfg);
            p.account.shutdown();
            p.account.configure(lobby_url(hub), hub.paths.data_dir / "netplay" / "account.key");
            p.account.restore();
            connect(hub);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (begin_modal("Join Direct###np_direct", &p.open_direct, 520.f)) {
        ImGui::PushTextWrapPos(480.f);
        ImGui::TextUnformatted("Look up a LAN/Direct IP lobby by address: a host's LAN IP on your "
                               "network, or their public IP with the game's UDP port forwarded.");
        ImGui::TextColored(th.text_muted, "Retro finds the room; joining it happens in the game's own "
                                          "Netplay menu (LAN / Direct IP), with the same address.");
        ImGui::PopTextWrapPos();
        ImGui::TextUnformatted("Host IP");
        ImGui::SetNextItemWidth(280.f);
        ImGui::InputText("##direct_ip", p.direct_ip, sizeof(p.direct_ip));
        ImGui::TextUnformatted("Port");
        ImGui::SetNextItemWidth(160.f);
        ImGui::InputText("##direct_port", p.direct_port, sizeof(p.direct_port),
                         ImGuiInputTextFlags_CharsDecimal);
        const auto probe = p.lan.last_probe();
        if (!p.direct_status.empty()) {
            if (probe.pending) ImGui::TextColored(th.text_muted, "Asking %s\xE2\x80\xA6", probe.endpoint.c_str());
            else if (probe.answered)
                ImGui::TextColored(th.good, "Found a lobby at %s \xE2\x80\x94 it is in the list now.",
                                   probe.endpoint.c_str());
            else
                ImGui::TextColored(th.warn,
                                   "No answer from %s. A PlayStation host answers while its LAN "
                                   "waiting room is open; other games' hosts only answer a join, "
                                   "which their own Netplay menu makes.",
                                   probe.endpoint.c_str());
        }
        if (ImGui::Button("Close", ImVec2(120, 0)) || cancel_pressed()) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (accent_button("Look Up", th, ImVec2(140, 0))) {
            std::string ip;
            int port = 0;
            const std::string ep = trim(p.direct_ip) + ":" + trim(p.direct_port);
            if (np::split_endpoint(ep, &ip, &port)) {
                p.lan.probe(ip, port);
                p.direct_status = "asked";
            } else {
                p.direct_status.clear();
                p.status = "Enter an IPv4 address and a port, like 192.168.1.20 and 7777.";
            }
        }
        ImGui::EndPopup();
    }

    if (begin_modal("Report Message###np_report", &p.open_report, 480.f)) {
        static const char* kReasons[] = {"Harassment",     "Hate speech", "Threats", "Sexual content",
                                         "Spam",           "Cheating claim", "Something else"};
        static const char* kCodes[] = {"harassment", "hate_speech", "threats", "sexual_content",
                                       "spam",       "cheating_claim", "other"};
        ImGui::Text("Report a message from %s", p.report_name.c_str());
        ImGui::SetNextItemWidth(300.f);
        ImGui::Combo("Reason", &p.report_reason, kReasons, IM_ARRAYSIZE(kReasons));
        ImGui::SetNextItemWidth(400.f);
        ImGui::InputTextWithHint("##report_note", "What happened, in a sentence", p.report_note,
                                 sizeof(p.report_note));
        ImGui::TextColored(th.text_muted, "The server keeps the message you report and sends it to "
                                          "its moderators with your note.");
        if (ImGui::Button("Cancel", ImVec2(120, 0)) || cancel_pressed()) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (danger_button("Send Report", th, ImVec2(140, 0))) {
            p.client.chat_report({p.report_mid}, kCodes[p.report_reason], trim(p.report_note));
            p.status = "Sending the report\xE2\x80\xA6";
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (begin_modal("Ignored & Blocked###np_modlist", &p.open_modlist, 480.f)) {
        if (p.mod.by_account.empty()) ImGui::TextColored(th.text_muted, "Nobody is ignored or blocked.");
        std::string remove;
        if (!p.mod.by_account.empty() &&
            ImGui::BeginTable("##modlist", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80.f);
            ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 80.f);
            ImGui::TableHeadersRow();
            for (const auto& [acct, e] : p.mod.by_account) {
                ImGui::PushID(acct.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.name.empty() ? acct.c_str() : e.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.blocked ? "Blocked" : "Ignored");
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Remove")) remove = acct;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!remove.empty()) {
            p.mod.by_account.erase(remove);
            p.mod.save();
        }
        if (ImGui::Button("Close", ImVec2(120, 0)) || cancel_pressed()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (begin_modal("Lobby Settings###np_room_info", &p.open_room_info, 520.f)) {
        const np::Snapshot& s = p.snap;
        ImGui::TextColored(th.accent, "ROOM");
        ImGui::TextColored(th.text_muted, "Lobby Server");
        ImGui::TextUnformatted(p.client_url.c_str());
        ImGui::TextColored(th.text_muted, "Lobby id");
        ImGui::TextUnformatted(s.lobby_id.c_str());
        if (s.match_caps.is_object() && !s.match_caps.empty()) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextColored(th.accent, "MATCH SETTINGS");
            ImGui::SameLine();
            ImGui::TextColored(th.text_muted, "(set by the host)");
            for (auto it = s.match_caps.begin(); it != s.match_caps.end(); ++it) {
                if (it.key() == "v" || it.key() == "mods") continue;
                ImGui::Text("%s: %s", it.key().c_str(), it->dump().c_str());
            }
        }
        if (ImGui::Button("Close", ImVec2(120, 0)) || cancel_pressed()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (begin_modal("Swap seats?###np_swap", &p.open_swap, 440.f)) {
        ImGui::Text("%s wants to swap seats with you.", p.swap.asker_name.c_str());
        ImGui::TextColored(th.text_muted, "They are in P%d; you are in P%d.", p.swap.from_slot + 1,
                           p.swap.target_slot + 1);
        if (accent_button("Swap", th, ImVec2(140, 0))) {
            p.client.seat_swap_answer(true, p.swap.asker_player_id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep my seat", ImVec2(140, 0)) || cancel_pressed()) {
            p.client.seat_swap_answer(false, p.swap.asker_player_id);
            p.decline_swaps_until = now_s() + 30.0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---- the overlay's Netplay ---------------------------------------------------------------
//
// The same pages as above (mode, sign-in, online, LAN / Direct IP, host, room) as rows a
// finger or a controller can reach: a header with where you are and Back, sections as a
// row of buttons, every list entry a row, dialogs as modals in the overlay's look, and
// the page's main actions pinned along the bottom. Used in the shell (no NetplayScope);
// Direct mode keeps the older pages. The state and the lobby code are the older pages'.

namespace ov = retcomm::hub::overlay;

constexpr float kRowGap = 8.f;

// A list entry: title and a muted line under it on the left, a value on the right.
// Returns true when pressed.
bool ov_item(const ov::Metrics& m, const char* id, const std::string& title, const std::string& sub,
             const std::string& right, ImU32 right_col, bool enabled = true) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = sub.empty() ? m.item_h + 10.f : (m.compact ? 66.f : 78.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ov::nav_button("##it", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool hot = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    const ImVec2 p1(p0.x + w, p0.y + h);
    ov::card_frame(p0, p1, hot, ImGui::IsItemFocused() && ImGui::GetIO().NavVisible);
    float right_w = 0.f;
    if (!right.empty()) {
        right_w = ImGui::CalcTextSize(right.c_str()).x;
        dl->AddText(ImVec2(p1.x - 18.f - right_w, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f), right_col,
                    right.c_str());
    }
    const float tw = std::max(80.f, w - 56.f - right_w);
    dl->PushClipRect(p0, ImVec2(p0.x + 18.f + tw, p1.y), true);
    const ImU32 tc = enabled ? IM_COL32(233, 237, 248, 255) : IM_COL32(153, 163, 188, 160);
    if (sub.empty()) {
        dl->AddText(ImVec2(p0.x + 18.f, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f), tc, title.c_str());
    } else {
        dl->AddText(ImVec2(p0.x + 18.f, p0.y + 11.f), tc, title.c_str());
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.88f,
                    ImVec2(p0.x + 18.f, p0.y + 11.f + ImGui::GetTextLineHeight() + 3.f),
                    IM_COL32(153, 163, 188, 255), sub.c_str());
    }
    dl->PopClipRect();
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.f, kRowGap - 2.f));
    return pressed && enabled;
}

constexpr ImU32 kColMuted = IM_COL32(153, 163, 188, 255);
constexpr ImU32 kColGood = IM_COL32(52, 211, 153, 255);
constexpr ImU32 kColWarn = IM_COL32(245, 165, 36, 255);
constexpr ImU32 kColAccent = IM_COL32(180, 150, 255, 255);

// A row of buttons choosing one of `names`; p.sub follows. Reset when the view changes.
void ov_sections(const std::vector<std::string>& names, bool end_row = true) {
    Page& p = page();
    if (p.sub_view != p.view) {
        p.sub_view = p.view;
        p.sub = 0;
    }
    p.sub = std::clamp(p.sub, 0, static_cast<int>(names.size()) - 1);
    for (int i = 0; i < static_cast<int>(names.size()); ++i) {
        if (i) ImGui::SameLine(0, 8.f);
        if (ov::action_button(names[static_cast<size_t>(i)].c_str(), i == p.sub, 0.f) && i != p.sub) {
            p.sub = i;
            ov::row_info_reset();
        }
    }
    if (end_row) ImGui::Dummy(ImVec2(0, 8));
}

// A short text kept in a std::string: a row, and its dialog. True when it changed.
bool ov_text(const ov::Metrics& m, const char* id, const char* label, std::string& value, const char* hint,
             const char* desc, bool secret = false, bool enabled = true, const char* shown_override = nullptr) {
    bool changed = false;
    ImGui::PushID(id);
    const std::string shown = shown_override ? std::string(shown_override)
                              : value.empty() ? std::string("not set")
                              : secret        ? std::string("set")
                                              : value;
    if (ov::setting_row(m, "row", label, shown, desc, enabled)) ImGui::OpenPopup("##td");
    std::string v = value;
    const int r = ov::text_dialog("##td", label, v, hint, secret);
    if (r == 1) {
        value = v;
        changed = true;
    } else if (r == 2) {
        value.clear();
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

void ov_status_line(const Theme& th) {
    Page& p = page();
    (void)th;
    if (!p.status.empty()) ov::banner(p.status.c_str());
}

// The page frame: "Netplay > <where>", Back on the right (not in a room), and the
// body. The caller draws the body between begin and end.
void ov_begin(const char* where, bool back, View back_to) {
    Page& p = page();
    ImGui::BeginChild("##npov", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    ImGui::SetWindowFontScale(1.1f);
    ImGui::TextColored(ImVec4(0.60f, 0.64f, 0.74f, 1.f), "Netplay");
    if (where && *where) {
        ImGui::SameLine(0, 8.f);
        ImGui::TextColored(ImVec4(0.60f, 0.64f, 0.74f, 1.f), ">");
        ImGui::SameLine(0, 8.f);
        ImGui::TextUnformatted(where);
    }
    ImGui::SetWindowFontScale(1.f);
    if (back) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 130.f);
        ImGui::SetCursorPosY(0.f);
        if (ov::action_button("< Back", false, 130.f)) {
            p.view = back_to;
            p.status.clear();
            ov::row_info_reset();
        }
    }
    ImGui::Dummy(ImVec2(0, 4));
}
void ov_end() { ImGui::EndChild(); }

// ---- mode ------------------------------------------------------------------------------

bool ov_mode_card(const ov::Metrics& m, const char* id, const char* title, const char* body, bool accent,
                  bool enabled, float w) {
    const float h = m.compact ? 118.f : 150.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ov::nav_button("##card", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool hot = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    const ImVec2 p1(p0.x + w, p0.y + h);
    ov::card_frame(p0, p1, hot || accent, ImGui::IsItemFocused() && ImGui::GetIO().NavVisible, 14.f);
    ImGui::SetWindowFontScale(1.2f);
    dl->AddText(ImVec2(p0.x + 20.f, p0.y + 16.f), IM_COL32(233, 237, 248, 255), title);
    ImGui::SetWindowFontScale(1.f);
    dl->PushClipRect(p0, p1, true);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.92f, ImVec2(p0.x + 20.f, p0.y + 52.f),
                enabled ? kColMuted : IM_COL32(153, 163, 188, 110), body, nullptr, w - 40.f);
    dl->PopClipRect();
    ImGui::PopID();
    return pressed && enabled;
}

void ov_mode(HubModel& hub, const Theme& th, const ov::Metrics& m) {
    Page& p = page();
    ov_begin("", false, View::Mode);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 14.f;
    const float w = (avail - gap) * 0.5f;
    const Account::State st = p.account.state();
    const bool offered = p.account.offered();
    const double waited = st == Account::State::Waiting ? now_s() - p.account.waiting_since() : 0;
    const bool holding = st == Account::State::Waiting && waited < 5.0;
    const char* online_body = !offered ? "Find players on the lobby server. This server does not offer sign-in, so "
                                         "you play as a guest."
                              : holding ? "Checking your saved sign-in..."
                                        : "Find players on the lobby server. Sign in with Discord so your name is "
                                          "yours across sessions.";
    ImGui::BeginChild("##npmodes", ImVec2(0, std::max(100.f, ImGui::GetContentRegionAvail().y)),
                      ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
    {
        const ImVec2 o = ImGui::GetCursorPos();
        if (ov_mode_card(m, "lan", "LAN / Direct IP",
                         "Play with someone on your network, or look up a host by address. No account, no lobby server.",
                         false, true, w))
            enter_lan(hub);
        ImGui::SetCursorPos(ImVec2(o.x + w + gap, o.y));
        if (ov_mode_card(m, "online", "Online Netplay", online_body, true, !holding, w)) {
            if (!offered || st == Account::State::SignedIn || st == Account::State::Waiting) enter_online(hub);
            else p.view = View::SignIn;
        }
        ImGui::SetCursorPos(ImVec2(o.x, o.y + (m.compact ? 118.f : 150.f) + 12.f));
    }
    ov_status_line(th);
    ov::row_info_reset();
    // Who you are here.
    {
        const std::string who = st == Account::State::SignedIn ? "Signed in as " + p.account.info().handle
                                : holding                       ? std::string("Checking your saved sign-in...")
                                : st == Account::State::Failed  ? "Not signed in: " + p.account.error()
                                                                 : std::string("Guest");
        if (ov::setting_row(m, "account", "Account", who,
                            offered ? "Signing in with Discord gives you a name that is yours across sessions."
                                    : "This lobby server does not offer sign-in.",
                            offered && !holding)) {
            if (st == Account::State::SignedIn) ImGui::OpenPopup("##signout");
            else p.view = View::SignIn;
        }
        if (ov::confirm_dialog("##signout", "Sign out?", "You will play as a guest until you sign in again.", "Sign out")) {
            p.account.sign_out();
            if (p.client_started) connect(hub);
        }
    }
    {
        std::string name = hub.cfg.netplay.display_name;
        const bool signed_in = st == Account::State::SignedIn;
        if (ov_text(m, "name", "Player name", name, "Your name in lobbies",
                    signed_in ? "Your name comes from your account." : "The name other players see.", false,
                    !signed_in, signed_in ? p.account.info().handle.c_str() : nullptr)) {
            const std::string n = trim(name);
            p.name_error = name_problem(n, "name");
            if (p.name_error.empty()) {
                hub.cfg = load_app_config(hub.paths.config_path);
                hub.cfg.netplay.display_name = n;
                save_app_config(hub.paths.config_path, hub.cfg);
                if (p.client_started) p.client.set_display_name(n);
            } else {
                p.status = p.name_error;
            }
        }
    }
    {
        std::string url = lobby_url(hub);
        if (ov_text(m, "server", "Lobby server", url, kDefaultNetplayLobbyUrl,
                    "The lobby server's address. Empty goes back to the default.")) {
            hub.cfg = load_app_config(hub.paths.config_path);
            hub.cfg.netplay.lobby_url = trim(url);
            if (hub.cfg.netplay.lobby_url.empty()) hub.cfg.netplay.lobby_url = kDefaultNetplayLobbyUrl;
            save_app_config(hub.paths.config_path, hub.cfg);
            p.account.shutdown();
            p.account.configure(lobby_url(hub), hub.paths.data_dir / "netplay" / "account.key");
            p.account.restore();
            connect(hub);
        }
    }
    {
        if (ov::setting_row(m, "mods", "Ignored and blocked players", std::to_string(p.mod.by_account.size()),
                            "Players whose chat you hide, and those you ask the matchmaker not to pair you with. "
                            "Tap to remove one.",
                            !p.mod.by_account.empty()))
            ov::open_picker("##modlist");
        std::vector<ov::PickerChoice> choices;
        std::vector<std::string> accounts;
        for (const auto& [acct, e] : p.mod.by_account) {
            choices.push_back({(e.name.empty() ? acct : e.name) + (e.blocked ? "  (blocked)" : "  (ignored)"), "Tap to remove"});
            accounts.push_back(acct);
        }
        const int pick = ov::picker(m, "##modlist", "Ignored and blocked", choices, -1);
        if (pick >= 0) {
            p.mod.by_account.erase(accounts[static_cast<size_t>(pick)]);
            p.mod.save();
        }
    }
    ImGui::EndChild();
    ov_end();
}

// ---- sign in -------------------------------------------------------------------------------

void ov_signin(HubModel& hub, const Theme& th, const ov::Metrics& m) {
    Page& p = page();
    const Account::State st = p.account.state();
    if (st == Account::State::SignedIn) {
        enter_online(hub);
        return;
    }
    ov_begin("Sign in", true, View::Mode);
    ov::muted("Signing in with Discord gives you a name that is yours across sessions and devices. Online play on this "
              "server needs an account; LAN and Direct IP do not.");
    ImGui::Dummy(ImVec2(0, 6));
    if (st == Account::State::Waiting) {
        const std::string url = p.account.login_url();
        ov::banner("Waiting for Discord. Finish signing in in your browser, then come back to this app.");
        ImGui::BeginChild("##sirows", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
        if (!url.empty()) {
            if (ov::setting_row(m, "again", "Open the sign-in page again", "Browser",
                                "Opens the page in your browser. It closes when you have signed in."))
                open_url_in_browser(url, nullptr);
            if (ov::setting_row(m, "copy", "Copy the sign-in link", "Copy",
                                "For a browser on another device. The sign-in finishes here either way."))
                SDL_SetClipboardText(url.c_str());
        }
        if (now_s() - p.account.waiting_since() > 5.0 &&
            ov::setting_row(m, "again2", "Start over", "New sign-in",
                            "If the page never opened, or you closed it: a fresh sign-in."))
            p.account.relogin();
        if (ov::setting_row(m, "cancel", "Cancel", "Stop waiting", "Go on as a guest.")) {
            p.account.cancel();
            p.view = View::Mode;
        }
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("##sirows", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
        if (st == Account::State::Failed) ov::banner(p.account.error().c_str());
        if (ov::action_button("Sign in with Discord", true, 0.f)) p.account.login();
        ImGui::Dummy(ImVec2(0, 8));
        if (ov::setting_row(m, "guest", "Continue as a guest", "Browse lobbies",
                            "Look at the lobbies without an account. The server may ask you to sign in to host or join."))
            enter_online(hub);
        ImGui::EndChild();
    }
    (void)th;
    ov_end();
}

// ---- who / what a tap on a player or a message does ------------------------------------

void open_person_actions(const std::string& name, const std::string& account, const std::string& mid) {
    Page& p = page();
    p.act_name = name;
    p.act_account = account;
    p.act_mid = mid;
    ov::open_picker("##person");
}

void ov_person_dialogs(const ov::Metrics& m) {
    Page& p = page();
    {
        std::vector<ov::PickerChoice> choices;
        std::vector<int> kinds; // 0 ignore/unignore, 1 block/unblock, 2 report
        const bool has_acct = !p.act_account.empty();
        const bool hidden = has_acct && p.mod.hidden(p.act_account);
        const bool blocked = has_acct && p.mod.blocked(p.act_account);
        if (has_acct) {
            if (!blocked) {
                choices.push_back({hidden ? "Stop ignoring" : "Ignore player", "Hide their chat. They can still be in your lobby."});
                kinds.push_back(0);
            }
            choices.push_back({blocked ? "Unblock player" : "Block player",
                               "Hide their chat and ask the matchmaker not to pair you."});
            kinds.push_back(1);
        } else {
            choices.push_back({"Signed-out players cannot be ignored", "There is no account to remember."});
            kinds.push_back(-1);
        }
        if (!p.act_mid.empty()) {
            choices.push_back({"Report this message", "The server keeps it and sends it to its moderators."});
            kinds.push_back(2);
        }
        const int pick = ov::picker(m, "##person", p.act_name.c_str(), choices, -1);
        if (pick >= 0) {
            const int k = kinds[static_cast<size_t>(pick)];
            if (k == 0) {
                if (hidden) p.mod.by_account.erase(p.act_account);
                else p.mod.by_account[p.act_account] = {p.act_name, false};
                p.mod.save();
            } else if (k == 1) {
                if (blocked) p.mod.by_account.erase(p.act_account);
                else p.mod.by_account[p.act_account] = {p.act_name, true};
                p.mod.save();
            } else if (k == 2) {
                p.report_mid = p.act_mid;
                p.report_name = p.act_name;
                p.open_report = true;
            }
        }
    }
    if (p.open_report) {
        p.open_report = false;
        ov::open_picker("##reason");
    }
    {
        static const char* kReasons[] = {"Harassment", "Hate speech", "Threats", "Sexual content",
                                         "Spam",       "Cheating claim", "Something else"};
        static const char* kCodes[] = {"harassment", "hate_speech", "threats", "sexual_content",
                                       "spam",       "cheating_claim", "other"};
        std::vector<ov::PickerChoice> choices;
        for (const char* r : kReasons) choices.push_back({r, ""});
        const int pick = ov::picker(m, "##reason", ("Report a message from " + p.report_name).c_str(), choices, -1);
        if (pick >= 0) {
            p.client.chat_report({p.report_mid}, kCodes[pick], "");
            p.status = "Sending the report...";
        }
    }
}

// ---- chat -----------------------------------------------------------------------------------

void ov_chat(const Theme& th, const ov::Metrics& m, const char* id, const std::deque<np::ChatLine>& lines,
             std::uint64_t* seen, bool enabled, const char* empty_text, const char* disabled_text,
             const std::function<void(const std::string&)>& send) {
    Page& p = page();
    (void)th;
    const float input_h = m.item_h + 22.f;
    ImGui::BeginChild(id, ImVec2(0, std::max(80.f, ImGui::GetContentRegionAvail().y - input_h)),
                      ImGuiChildFlags_NavFlattened);
    bool any = false;
    std::uint64_t newest = 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const np::ChatLine& l : lines) {
        newest = l.seq;
        if (!l.is_local && p.mod.hidden(l.from_account)) continue;
        any = true;
        ImGui::PushID(static_cast<int>(l.seq));
        const float w = ImGui::GetContentRegionAvail().x;
        if (l.is_system) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.64f, 0.74f, 1.f));
            ImGui::TextWrapped("%s", l.text.c_str());
            ImGui::PopStyleColor();
        } else {
            const std::string text = l.from + ":  " + l.text;
            const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, w - 36.f);
            const float h = ts.y + 22.f;
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const bool pressed = !l.is_local && ov::nav_button("##ln", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
            if (l.is_local) ImGui::Dummy(ImVec2(w, h));
            const ImVec2 p1(p0.x + w, p0.y + h);
            ov::card_frame(p0, p1, !l.is_local && (ImGui::IsItemHovered() || ImGui::IsItemFocused()),
                           !l.is_local && ImGui::IsItemFocused() && ImGui::GetIO().NavVisible, 10.f);
            const float nx = ImGui::CalcTextSize((l.from + ":").c_str()).x;
            dl->AddText(ImVec2(p0.x + 14.f, p0.y + 11.f), l.is_local ? kColGood : kColAccent, (l.from + ":").c_str());
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p0.x + 14.f + nx + 8.f, p0.y + 11.f),
                        IM_COL32(233, 237, 248, 255), l.text.c_str(), nullptr, std::max(40.f, w - 36.f - nx - 8.f));
            if (pressed) open_person_actions(l.from, l.from_account, l.mid);
            ImGui::Dummy(ImVec2(0, 4));
        }
        ImGui::PopID();
    }
    if (!any) ov::muted(enabled ? empty_text : disabled_text);
    if (newest != *seen) {
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.f) ImGui::SetScrollHereY(1.f);
        *seen = newest;
    }
    ImGui::EndChild();
    ImGui::PushID(id);
    std::string msg;
    if (ov::setting_row(m, "send", "Message", enabled ? "Write a message" : "unavailable", nullptr, enabled))
        ImGui::OpenPopup("##msg");
    const int r = ov::text_dialog("##msg", "Message", msg, "Say something");
    if (r == 1 && !trim(msg).empty() && enabled) send(trim(msg));
    ImGui::PopID();
}

// ---- online ------------------------------------------------------------------------------------

void ov_online(HubModel& hub, const Theme& th, const ov::Metrics& m, const std::vector<Game>& games) {
    Page& p = page();
    const np::Snapshot& s = p.snap;
    const bool all = p.filter.empty();
    {
        std::string where = "Online";
        if (s.state == np::ConnState::Connected)
            where += std::string("  \xC2\xB7  ") + (s.signed_in ? "signed in as " : "guest ") + s.display_name +
                     (s.rtt_ms >= 0 ? "  \xC2\xB7  " + std::to_string(s.rtt_ms) + " ms" : std::string());
        else if (s.state == np::ConnState::Connecting) where += "  \xC2\xB7  connecting...";
        else where += "  \xC2\xB7  can't reach the lobby server";
        ov_begin(where.c_str(), true, View::Mode);
    }

    // The game filter.
    {
        const std::string label = all ? "All games" : title_for(hub, p.filter);
        if (ov::setting_row(m, "game", "Game", label,
                            "Show every game's lobbies, or one installed game's. Server chat and players online "
                            "follow the game you pick."))
            ov::open_picker("##gamefilter");
        std::vector<ov::PickerChoice> choices{{"All games", ""}};
        int cur = all ? 0 : -1;
        for (size_t i = 0; i < games.size(); ++i) {
            choices.push_back({games[i].title, games[i].pin});
            if (games[i].game_name == p.filter) cur = static_cast<int>(i) + 1;
        }
        const int pick = ov::picker(m, "##gamefilter", "Game", choices, cur);
        if (pick == 0) set_filter(hub, "", games);
        else if (pick > 0) set_filter(hub, games[static_cast<size_t>(pick - 1)].game_name, games);
    }
    if (s.state == np::ConnState::Disconnected || s.state == np::ConnState::Failed)
        if (!s.transport_error.empty()) ov::banner(s.transport_error.c_str());
    ov_status_line(th);
    ov_sections({"Lobbies  " + std::to_string(s.rooms.size()), "Players online", "Server chat"}, false);
    ImGui::SameLine(0, 24.f);
    if (ov::action_button("Host a lobby", true, 0.f)) {
        open_host_modal(hub, games);
        if (p.open_host) {
            p.open_host = false;
            p.view = View::Host;
            ov::row_info_reset();
        }
    }
    ImGui::SameLine(0, 8.f);
    if (ov::action_button(s.state == np::ConnState::Connected ? "Refresh" : "Reconnect", false, 0.f)) {
        if (s.state == np::ConnState::Connected) p.client.request_list();
        else {
            p.account.restore();
            connect(hub);
        }
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::BeginChild("##onbody", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_None);
    if (p.sub == 0) {
        if (s.rooms.empty()) ov::muted("No lobbies yet. Host one.");
        for (const np::LobbyRow& r : s.rooms) {
            const std::string why = join_blocker(hub, games, r.game_name, r.game_version, nullptr);
            std::string sub = title_for(hub, r.game_name);
            if (r.allow_spectators) sub += "  \xC2\xB7  spectators " + std::to_string(r.spectator_count) + "/" + std::to_string(r.max_spectators);
            if (!why.empty()) sub = why;
            const std::string title = (r.host_country.empty() ? "" : r.host_country + "  ") +
                                      (r.name.empty() ? "Unnamed lobby" : r.name) + (r.has_password ? "  [locked]" : "");
            const std::string right = std::to_string(r.player_count) + "/" + std::to_string(r.max_slots) +
                                      (why.empty() ? "  Join" : "");
            if (ov_item(m, r.lobby_id.c_str(), title, sub, right, why.empty() ? kColGood : kColMuted, true))
                join_room(hub, games, r);
        }
    } else if (p.sub == 1) {
        int shown = 0;
        for (const np::OnlinePlayer& o : p.snap.players) {
            if (!o.is_local && p.mod.blocked(o.account)) continue;
            ++shown;
            const std::string game = o.game_name.empty() ? std::string() : title_for(hub, o.game_name);
            std::string where = o.hosting ? "Hosting" : !o.lobby_id.empty() ? "In lobby" : "Browsing";
            std::string sub = !o.lobby_name.empty() ? o.lobby_name : std::string();
            if (!game.empty()) sub += (sub.empty() ? "" : "  \xC2\xB7  ") + game;
            std::string title = (o.country.empty() ? "" : o.country + "  ") + o.display_name;
            if (o.is_local) title += "  (you)";
            else if (p.mod.hidden(o.account)) title += "  (ignored)";
            if (ov_item(m, (o.display_name + std::to_string(shown)).c_str(), title, sub, where,
                        o.hosting ? kColGood : kColMuted, !o.is_local))
                open_person_actions(o.display_name, o.account, {});
        }
        if (shown == 0 || (shown == 1 && p.snap.players.size() <= 1))
            ov::muted(all ? "Nobody else is online." : "Nobody else is online for this game.");
    } else {
        ov_chat(th, m, "server_chat", s.server_chat, &p.server_chat_seen, !all && s.state == np::ConnState::Connected,
                "Everyone online for this game sees this. There is no history, only what is said while you are here.",
                all ? "Server chat is per game: pick one in Game to talk with its players."
                    : "Server chat is not available right now.",
                [&](const std::string& t) { p.client.server_chat(t); });
    }
    ImGui::EndChild();
    ov_person_dialogs(m);
    ov_end();
}

// ---- host a lobby ------------------------------------------------------------------------------------

void ov_host(HubModel& hub, const Theme& th, const ov::Metrics& m, const std::vector<Game>& games) {
    Page& p = page();
    ov_begin("Host a lobby", true, View::Online);
    if (games.empty()) {
        ov::muted("Install a game that supports netplay to host a lobby.");
        ov_end();
        return;
    }
    p.host_game = std::clamp(p.host_game, 0, static_cast<int>(games.size()) - 1);
    const Game& g = games[static_cast<size_t>(p.host_game)];
    ImGui::BeginChild("##hostrows", ImVec2(0, std::max(80.f, ImGui::GetContentRegionAvail().y - (m.item_h + 22.f))),
                      ImGuiChildFlags_NavFlattened);
    if (ov::setting_row(m, "game", "Game", g.title, "The game the lobby is for. Guests need the same build.", games.size() > 1))
        ov::open_picker("##hostgame");
    {
        std::vector<ov::PickerChoice> choices;
        for (const Game& x : games) choices.push_back({x.title, "Version " + x.pin});
        const int pick = ov::picker(m, "##hostgame", "Game", choices, p.host_game);
        if (pick >= 0) p.host_game = pick;
    }
    {
        std::string name = p.host_name;
        if (ov_text(m, "lname", "Lobby name", name, "A name for the lobby", "What players see in the list.")) {
            copy_to(p.host_name, sizeof(p.host_name), name);
        }
    }
    {
        const int top = std::min(8, g.max_slots);
        p.host_max = std::clamp(p.host_max, 2, std::max(2, top));
        if (top > 2) {
            int mx = p.host_max;
            if (ov::stepper_row(m, "max", "Max players", &mx, 2, top, 1, "", "How many players can sit in the match."))
                p.host_max = mx;
        } else {
            ov::value_row(m, "max", "Max players", "2 (this game is 2-player)");
        }
    }
    ov::toggle_row(m, "spec", "Allow spectators", &p.host_spectators, "Up to 4 watchers, on top of the players.");
    {
        std::string pw = p.host_pw;
        if (ov_text(m, "pw", "Password", pw, "optional", "Players need it to join. Empty means anyone can.", true))
            copy_to(p.host_pw, sizeof(p.host_pw), pw);
    }
    if (!p.host_error.empty()) ov::banner(p.host_error.c_str());
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 4));
    const bool connected = p.snap.state == np::ConnState::Connected;
    ImGui::BeginDisabled(!connected);
    if (ov::action_button("Create lobby", true, 0.f)) {
        const std::string name = trim(p.host_name);
        p.host_error = name_problem(name, "lobby name");
        if (p.host_error.empty()) {
            Page::PendingSeat ps;
            ps.create = true;
            ps.game_name = g.game_name;
            ps.version = g.pin;
            ps.name = name;
            ps.password = p.host_pw;
            ps.max_slots = p.host_max;
            ps.spectators = p.host_spectators;
            begin_seat(hub, g, std::move(ps));
            p.view = View::Online;
        }
    }
    ImGui::EndDisabled();
    if (!connected) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(th.warn, "Not connected to the lobby server.");
    }
    ov_end();
}

// ---- LAN / Direct IP ---------------------------------------------------------------------------------

void ov_lan(HubModel& hub, const Theme& th, const ov::Metrics& m, const std::vector<Game>& games) {
    Page& p = page();
    ov_begin("LAN / Direct IP", true, View::Mode);
    ov_sections({"Rooms on your network", "Direct IP"});
    ov_status_line(th);
    const float footer_h = m.item_h + 22.f;
    ImGui::BeginChild("##lanbody", ImVec2(0, std::max(80.f, ImGui::GetContentRegionAvail().y - footer_h)),
                      ImGuiChildFlags_NavFlattened);
    if (p.sub == 0) {
        const std::string lerr = p.lan.listen_error();
        if (!lerr.empty())
            ov::banner(("Not listening for recomp-net rooms: " + lerr + ". PlayStation rooms are still found.").c_str());
        const auto rooms = p.lan.rooms();
        if (rooms.empty())
            ov::muted("No LAN rooms found yet. Hosts on your network show up here while their waiting room is open.");
        for (size_t i = 0; i < rooms.size(); ++i) {
            const np::LanRoom& r = rooms[i];
            const Game* g = game_by_wire_name(games, r.game_name);
            const std::string title = (r.name.empty() ? std::string("Unnamed room") : r.name) +
                                      (r.has_password ? "  [locked]" : "") + (r.direct ? "  [direct]" : "");
            const std::string sub = (g ? g->title : title_for(hub, r.game_name)) + "  \xC2\xB7  " + r.endpoint +
                                    (r.game_version.empty() ? "" : "  \xC2\xB7  " + r.game_version);
            if (ov_item(m, ("lan" + std::to_string(i)).c_str(), title, sub,
                        std::to_string(r.players) + "/" + std::to_string(r.max_slots), kColMuted)) {
                p.status = "A LAN room is held by the host's game, and a seat is taken by yours. Join from " +
                           (g ? g->title : std::string("the game")) +
                           "'s own Netplay menu (LAN / Direct IP): Retro cannot hand a seat to a game yet.";
            }
        }
    } else {
        ov::muted("Look up a lobby by address: a host's LAN IP on your network, or their public IP with the game's UDP "
                  "port forwarded. Retro finds the room; joining happens in the game's own Netplay menu with the same "
                  "address.");
        std::string ip = p.direct_ip, port = p.direct_port;
        if (ov_text(m, "ip", "Host IP", ip, "192.168.1.20", "The address of the host's machine."))
            copy_to(p.direct_ip, sizeof(p.direct_ip), trim(ip));
        if (ov_text(m, "port", "Port", port, "7777", "The game's UDP port (7777 by default)."))
            copy_to(p.direct_port, sizeof(p.direct_port), trim(port));
        if (ov::setting_row(m, "look", "Look up", "Ask this address", "Sends a lookup and lists the room if it answers.")) {
            std::string ip_s;
            int port_n = 0;
            const std::string ep = trim(p.direct_ip) + ":" + trim(p.direct_port);
            if (np::split_endpoint(ep, &ip_s, &port_n)) {
                p.lan.probe(ip_s, port_n);
                p.direct_status = "asked";
            } else {
                p.direct_status.clear();
                p.status = "Enter an IPv4 address and a port, like 192.168.1.20 and 7777.";
            }
        }
        const auto probe = p.lan.last_probe();
        if (!p.direct_status.empty()) {
            if (probe.pending) ov::muted(("Asking " + probe.endpoint + "...").c_str());
            else if (probe.answered)
                ov::muted(("Found a lobby at " + probe.endpoint + ". It is in the list under Rooms on your network.").c_str());
            else
                ov::banner(("No answer from " + probe.endpoint +
                            ". A PlayStation host answers while its LAN waiting room is open; other games' hosts only "
                            "answer a join, which their own Netplay menu makes.")
                               .c_str());
        }
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 4));
    if (ov::action_button("Refresh", false, 0.f)) p.lan.refresh();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(th.text_muted, "Hosting from here needs the game's own Netplay menu: its game owns the lobby.");
    ov_end();
}

// ---- the room ---------------------------------------------------------------------------------------------

void ov_room(HubModel& hub, const Theme& th, const ov::Metrics& m, const std::vector<Game>& games) {
    Page& p = page();
    const np::Snapshot& s = p.snap;
    (void)games;
    std::string game_label, version;
    for (const np::LobbyRow& r : s.rooms)
        if (r.lobby_id == s.lobby_id) {
            game_label = title_for(hub, r.game_name);
            version = r.game_version;
        }
    const std::string where = (s.room_name.empty() ? std::string("Lobby") : s.room_name) +
                              (game_label.empty() ? "" : "  \xC2\xB7  " + game_label + " " + version);
    ov_begin(where.c_str(), false, View::Online);
    if (p.lib && p.lib->capable)
        ov::muted(("The host's PLAY starts " + p.lib->title.name + " on every player's machine, into this match.").c_str());
    else
        ov::banner((p.lib && !p.lib->why.empty()
                        ? p.lib->why
                        : std::string("Retro cannot start this game into a match: update the game, or start the match "
                                      "from its own Netplay menu."))
                       .c_str());
    if (!p.room_status.empty()) ov::banner(p.room_status.c_str());

    int seated = 0;
    for (const np::Member& mm : s.members)
        if (!mm.is_spectator) ++seated;
    ov_sections({"Seats  " + std::to_string(seated) + "/" + std::to_string(s.max_slots), "Chat", "Info"}, false);
    // Leave and Play ride the sections' row: the page is short.
    std::string play_why;
    ImGui::SameLine(0, 24.f);
    if (ov::action_button("Leave lobby", false, 0.f)) {
        if (s.is_host) p.client.close_room();
        else p.client.leave();
    }
    if (s.is_host) {
        const Page::LibGame* lib = p.lib.get();
        const std::string lib_why = !lib ? std::string("Retro cannot start this game into a match")
                                    : !lib->capable ? lib->why
                                    : !lib->identity.is_object() ? "Preparing " + lib->title.name + "..."
                                    : lib->settle_f.valid() ? std::string("Settling the match...")
                                                            : std::string();
        play_why = seated < 2 ? std::string("Waiting for another player to join") : lib_why;
        ImGui::SameLine(0, 8.f);
        ImGui::BeginDisabled(!play_why.empty());
        if (ov::action_button("Play", true, 0.f)) {
            const json req = np::handoff_settle_request(s);
            p.lib->settle_f = std::async(std::launch::async,
                                         [paths = hub.paths, title = p.lib->title, opts = p.lib->opts,
                                          dir = p.lib->work_dir, req]() {
                                             return np::run_handoff_query(paths, title, opts, req, dir);
                                         });
            p.room_status = "Settling the match...";
        }
        ImGui::EndDisabled();
    } else {
        play_why = "Waiting for the host to start...";
    }
    if (!play_why.empty()) ov::muted(play_why.c_str());
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::BeginChild("##roombody", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    if (p.sub == 0) {
        auto seat = [&](int slot, const np::Member* mem, bool spectator) {
            const bool mine = mem && mem->is_local;
            std::string title = std::string(spectator ? "S" : "P") + std::to_string(slot + 1) + "   ";
            std::string right, sub;
            bool seated_me = false;
            for (const np::Member& x : s.members)
                if (x.is_local && !x.is_spectator) seated_me = true;
            if (mem) {
                title += (mem->country.empty() ? "" : mem->country + "  ") + mem->display_name + (mine ? "  (you)" : "");
                right = spectator ? "Watching" : mem->is_host ? "Host" : mem->ready ? "Connected" : "Waiting";
            } else {
                title += "Open slot";
                right = seated_me ? "Move here" : "";
            }
            if (ov_item(m, ((spectator ? "s" : "p") + std::to_string(slot)).c_str(), title, sub, right,
                        mine ? kColGood : mem ? kColMuted : kColAccent, mem || seated_me)) {
                p.seat_action_slot = slot;
                p.seat_action_spec = spectator;
                ov::open_picker("##seat");
            }
        };
        const int seats = std::max(s.max_slots, seated);
        for (int slot = 0; slot < seats; ++slot) {
            const np::Member* mem = nullptr;
            for (const np::Member& x : s.members)
                if (!x.is_spectator && x.slot == slot) mem = &x;
            seat(slot, mem, false);
        }
        for (const np::Member& x : s.members)
            if (x.is_spectator) seat(x.slot, &x, true);
    } else if (p.sub == 1) {
        ov_chat(th, m, "room_chat", s.room_chat, &p.room_chat_seen, s.state == np::ConnState::Connected,
                "Say hello. Everyone in the room sees this.", "Chat is not available in this room.",
                [&](const std::string& t) { p.client.chat(t); });
    } else {
        ov::value_row(m, "srv", "Lobby server", p.client_url);
        ov::value_row(m, "lid", "Lobby id", s.lobby_id);
        if (s.match_caps.is_object() && !s.match_caps.empty())
            for (auto it = s.match_caps.begin(); it != s.match_caps.end(); ++it) {
                if (it.key() == "v" || it.key() == "mods") continue;
                ov::value_row(m, it.key().c_str(), it.key().c_str(), it->dump());
            }
    }
    ImGui::EndChild();

    // The seat a tap opened: move there, swap, kick, ignore.
    {
        std::vector<ov::PickerChoice> choices;
        std::vector<std::function<void()>> acts;
        const int slot = p.seat_action_slot;
        const np::Member* target = nullptr;
        for (const np::Member& x : s.members)
            if (x.is_spectator == p.seat_action_spec && x.slot == slot) target = &x;
        const np::Member* me = nullptr;
        for (const np::Member& x : s.members)
            if (x.is_local) me = &x;
        const std::string title = target ? target->display_name : std::string("Open seat");
        if (!p.seat_action_spec) {
            if (!target && me && !me->is_spectator) {
                choices.push_back({"Move here", "Take this open seat"});
                acts.push_back([&, slot] {
                    if (s.is_host) p.client.move_slot(me->slot, slot);
                    else p.client.seat_move(slot);
                });
            } else if (target && me && !me->is_spectator && !target->is_local && !s.is_host) {
                choices.push_back({"Ask to swap seats", "They are asked to accept"});
                acts.push_back([&, slot] {
                    p.client.seat_swap_request(slot);
                    p.room_status = "Waiting for the other player to accept the seat swap...";
                });
            } else if (target && target->is_local) {
                // A host moves anyone; a guest moves themselves: to an open seat, or by asking.
                for (int to = 0; to < std::max(s.max_slots, seated); ++to) {
                    if (to == slot) continue;
                    const np::Member* occ = nullptr;
                    for (const np::Member& x : s.members)
                        if (!x.is_spectator && x.slot == to) occ = &x;
                    if (!occ) {
                        choices.push_back({"Move to P" + std::to_string(to + 1), "Open"});
                        acts.push_back([&, slot, to] { if (s.is_host) p.client.move_slot(slot, to); else p.client.seat_move(to); });
                    } else if (!s.is_host) {
                        choices.push_back({"Swap with " + occ->display_name + " (P" + std::to_string(to + 1) + ")", "They are asked to accept"});
                        acts.push_back([&, to] {
                            p.client.seat_swap_request(to);
                            p.room_status = "Waiting for the other player to accept the seat swap...";
                        });
                    }
                }
            }
            if (s.is_host && target && !target->is_local) {
                for (int to = 0; to < std::max(s.max_slots, seated); ++to) {
                    if (to == slot) continue;
                    choices.push_back({"Move to P" + std::to_string(to + 1), "Swaps if someone is there"});
                    acts.push_back([&, slot, to] { p.client.move_slot(slot, to); });
                }
            }
        }
        if (s.is_host && target && !target->is_local && !target->is_host) {
            choices.push_back({"Kick", "Remove them from the lobby"});
            acts.push_back([&, slot] { p.client.kick(slot); });
        }
        if (target && !target->is_local) {
            choices.push_back({"Ignore or block...", "Chat and matchmaking"});
            acts.push_back([&] {
                if (target) open_person_actions(target->display_name, target->account, {});
            });
        }
        if (choices.empty()) choices.push_back({"Nothing to do here", ""}), acts.push_back([] {});
        const int pick = ov::picker(m, "##seat", title.c_str(), choices, -1);
        if (pick >= 0) acts[static_cast<size_t>(pick)]();
    }
    ov_person_dialogs(m);

    ov_end();
}

// Dialogs the older pages opened through flags (a password, the name prompt, a swap ask).
void ov_flag_dialogs(HubModel& hub, const Theme& th, const ov::Metrics& m, const std::vector<Game>& games) {
    Page& p = page();
    (void)th;
    (void)m;
    if (p.open_join_pw) {
        p.open_join_pw = false;
        ImGui::OpenPopup("##joinpw");
    }
    {
        std::string pw;
        const int r = ov::text_dialog("##joinpw", "Lobby password", pw, "password", true);
        if (r == 1) {
            Page::PendingSeat ps;
            ps.lobby_id = p.join_pw_lobby;
            ps.password = pw;
            ps.game_name = p.join_pw_game;
            ps.version = p.join_pw_version;
            if (const Game* g = game_by_wire_name(games, p.join_pw_game)) begin_seat(hub, *g, std::move(ps));
            else send_seat(ps, json::object());
        }
    }
    if (p.open_name) {
        p.open_name = false;
        ImGui::OpenPopup("##npname");
    }
    {
        std::string n = hub.cfg.netplay.display_name;
        const int r = ov::text_dialog("##npname", "Player name", n, "Your name in lobbies");
        if (r == 1) {
            const std::string t = trim(n);
            p.name_error = name_problem(t, "name");
            if (p.name_error.empty()) {
                hub.cfg = load_app_config(hub.paths.config_path);
                hub.cfg.netplay.display_name = t;
                save_app_config(hub.paths.config_path, hub.cfg);
                if (p.client_started) p.client.set_display_name(t);
            } else {
                p.status = p.name_error;
            }
        }
    }
    if (p.open_swap) {
        p.open_swap = false;
        ImGui::OpenPopup("##swap");
    }
    {
        static bool was_open = false, answered = false;
        if (ov::confirm_dialog("##swap", "Swap seats?",
                               (p.swap.asker_name + " wants to swap seats with you. They are in P" +
                                std::to_string(p.swap.from_slot + 1) + "; you are in P" +
                                std::to_string(p.swap.target_slot + 1) + ".")
                                   .c_str(),
                               "Swap")) {
            p.client.seat_swap_answer(true, p.swap.asker_player_id);
            answered = true;
        }
        // Closed any other way: keep the seat, and decline further asks for 30 s.
        const bool open = ImGui::IsPopupOpen("##swap");
        if (was_open && !open) {
            if (!answered) {
                p.client.seat_swap_answer(false, p.swap.asker_player_id);
                p.decline_swaps_until = now_s() + 30.0;
            }
            answered = false;
        }
        was_open = open;
    }
}

void draw_netplay_overlay(HubModel& hub, const Theme& th, const std::vector<Game>& games) {
    Page& p = page();
    const ov::Metrics m = ov::metrics(ImGui::GetMainViewport()->Size);
    if (p.view != p.last_view) {
        ov::row_info_reset();
        p.last_view = p.view;
    }
    switch (p.view) {
    case View::Mode: ov_mode(hub, th, m); break;
    case View::SignIn: ov_signin(hub, th, m); break;
    case View::Online: ov_online(hub, th, m, games); break;
    case View::Lan: ov_lan(hub, th, m, games); break;
    case View::Host: ov_host(hub, th, m, games); break;
    case View::Room: ov_room(hub, th, m, games); break;
    }
    ov_flag_dialogs(hub, th, m, games);
}

// Per frame: what the lobby said since last frame.
void ingest(HubModel& hub) {
    Page& p = page();
    if (p.client_started) p.snap = p.client.snapshot();
    const np::Snapshot& s = p.snap;

    if (s.state == np::ConnState::Connected && p.status == "Connecting to lobby server\xE2\x80\xA6")
        p.status.clear();
    if (s.error_seq != p.seen_error_seq) {
        p.seen_error_seq = s.error_seq;
        const std::string msg = error_sentence(s.last_error_code, s.last_error_detail);
        if (s.in_room) p.room_status = msg;
        else p.status = msg;
        if (s.last_error_code == "session_invalid") p.account.restore();
    }
    if (s.report_ok_seq != p.seen_report_seq) {
        p.seen_report_seq = s.report_ok_seq;
        p.status = "Report sent to the moderation queue.";
    }
    // Blocks go to the server whenever they change, and on every connection.
    if (s.state == np::ConnState::Connected && p.pushed_blocks_rev != p.mod.rev) {
        p.pushed_blocks_rev = p.mod.rev;
        p.client.set_blocks(p.mod.blocked_accounts());
    }
    if (s.swap_ask.seq != p.seen_swap_seq) {
        p.seen_swap_seq = s.swap_ask.seq;
        if (now_s() < p.decline_swaps_until) {
            p.client.seat_swap_answer(false, s.swap_ask.asker_player_id);
        } else {
            p.swap = s.swap_ask;
            p.open_swap = true;
        }
    }
    if (s.swap_result.seq != p.seen_swap_result_seq) {
        p.seen_swap_result_seq = s.swap_result.seq;
        p.room_status = s.swap_result.accepted ? "" : "That player kept their seat.";
    }
    // ---- library games: identity, ready, settle, the match's status -------
    if (p.lib && future_ready(p.lib->identity_f)) {
        np::QueryAnswer qa = p.lib->identity_f.get();
        if (qa.ok && !qa.answer.contains("error")) {
            p.lib->identity = qa.answer;
            p.ready_sent_at = -1;
            if (p.pending_seat) {
                send_seat(*p.pending_seat, qa.answer.value("join_fields", json::object()));
                p.pending_seat.reset();
            }
        } else {
            const std::string why = qa.ok ? qa.answer.value("error", std::string()) : qa.error;
            p.lib->capable = false;
            p.lib->why = p.lib->title.name + " could not prepare for netplay: " + why;
            if (p.pending_seat) {
                p.pending_seat.reset();
                p.status = p.lib->why;
            }
        }
    }
    // Ready, with what the game offers (PSX: its BIOS), whenever the server
    // has cleared it (it does on every membership change).
    if (s.in_room && !p.scope && p.lib && p.lib->identity.is_object() &&
        p.lib->identity.contains("ready")) {
        bool ready = false;
        for (const np::Member& m : s.members)
            if (m.is_local) ready = m.ready;
        if (!ready && (p.ready_sent_at < 0 || now_s() - p.ready_sent_at > 2.0)) {
            p.client.send_raw(p.lib->identity["ready"]);
            p.ready_sent_at = now_s();
        }
    }
    if (p.lib && future_ready(p.lib->settle_f)) {
        np::QueryAnswer qa = p.lib->settle_f.get();
        if (qa.ok && qa.answer.contains("start") && qa.answer["start"].is_object()) {
            p.client.send_raw(qa.answer["start"]);
            p.room_status = "Starting the match\xE2\x80\xA6";
        } else {
            p.room_status = "Cannot start the match: " +
                            (qa.ok ? qa.answer.value("error", std::string("the game refused"))
                                   : qa.error);
        }
    }
    if (p.lib && p.lib->record_open) {
        std::string why;
        switch (np::read_handoff_status(p.lib->record, &why)) {
        case np::HandoffState::Pending: break;
        case np::HandoffState::Started:
            p.lib->record_open = false;
            p.room_status = p.lib->title.name + " is playing the match.";
            break;
        case np::HandoffState::Refused:
            p.lib->record_open = false;
            p.room_status = p.lib->title.name + " did not start the match: " + why;
            break;
        }
    }

    // ---- host relay: the host's port, the guests' probes -------------------
    const bool relay_room = p.scope && s.in_room && s.match_caps.is_object() &&
                            s.match_caps.value("relay", std::string()) == "host";
    const bool playing = p.scope && p.scope->match_running && p.scope->match_running();
    if (relay_room && s.is_host && !s.launch_pending && !playing) {
        if (!p.host_port_open) {
            std::string err;
            p.host_port_open = p.host_port.start(0, "stun.l.google.com:19302", &err);
            p.host_endpoint_sent = false;
            p.relay_note = p.host_port_open ? "Host relay: opening your port\xE2\x80\xA6"
                                            : "Host relay: " + err + ". The lobby server will relay.";
            p.relay_note_bad = !p.host_port_open;
        }
        const np::HostPort::Status hs = p.host_port.status();
        if (hs.done && !p.host_endpoint_sent) {
            p.host_endpoint_sent = true;
            if (!hs.endpoint.empty()) p.client.set_host_endpoint(hs.endpoint);
        }
        if (hs.probes_answered) {
            // A probe answered is the proof, whatever the router said: the
            // guest reached this port at the address the server holds for us.
            p.relay_note = "Host relay: guests reach this machine directly on UDP port " +
                           std::to_string(hs.local_port) + " (" +
                           std::to_string(hs.probes_answered) + " probe(s) answered).";
            p.relay_note_bad = false;
        } else if (hs.done) {
            p.relay_note = "Host relay: " + hs.detail;
            p.relay_note_bad = hs.endpoint.empty();
        }
    }
    if (relay_room && !s.is_host && !s.host_endpoint.empty() && s.host_endpoint != p.probed_endpoint &&
        !p.probe.valid()) {
        p.probed_endpoint = s.host_endpoint;
        const std::string ep = s.host_endpoint;
        p.probe = std::async(std::launch::async, [ep] { return np::probe_host(ep); });
        p.relay_note = "Host relay: checking that you can reach the host\xE2\x80\xA6";
        p.relay_note_bad = false;
    }
    if (p.probe.valid() && p.probe.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const bool ok = p.probe.get();
        p.client.path_report(ok ? "direct" : "fail");
        p.relay_note = ok ? "Host relay: you reach the host directly."
                          : "Host relay: the host's port does not answer you, so the lobby "
                            "server will relay the match.";
        p.relay_note_bad = !ok;
    }
    if (!s.in_room && (p.host_port_open || !p.probed_endpoint.empty())) {
        // Left the room: give the port (and any router mapping) back.
        p.host_port.stop();
        p.host_port.unmap();
        p.host_port_open = false;
        p.probed_endpoint.clear();
        p.relay_note.clear();
    }

    // ---- Transfer Pak lobbies: every seat's pak to every peer ---------------
    const bool tpak_room = p.scope && s.in_room && s.match_caps.is_object() &&
                           s.match_caps.value("tpak", false);
    if (!tpak_room) {
        if (!p.paks.empty() || !p.pak_members.empty()) {
            p.paks.clear();
            p.pak_parts.clear();
            p.pak_members.clear();
            p.pak_ready = false;
        }
    } else {
        // Ours: taken once per room, sent to the room whenever its members change.
        std::string members;
        for (const np::Member& m : s.members)
            if (!m.is_spectator) members += m.player_id + ",";
        if (!p.pak_ready && p.scope->local_pak) {
            // A new room: the last room's paks are staging, not keepsakes (a
            // match copied what it used into its own sandbox), and their
            // names are player ids that never come back.
            std::error_code ec;
            if (!p.scope->pak_dir.empty())
                for (const auto& e : fs::directory_iterator(p.scope->pak_dir, ec))
                    if (e.path().extension() == ".sav") fs::remove(e.path(), ec);
            p.my_pak = p.scope->local_pak();
            p.pak_ready = true;
            p.pak_note = p.my_pak.cart < 0 ? p.my_pak.note : std::string();
        }
        if (p.pak_ready && members != p.pak_members && s.local_slot >= 0) {
            p.pak_members = members;
            const std::string sha = p.my_pak.cart >= 0 ? sha256_hex(p.my_pak.save) : std::string();
            const std::string b64 = p.my_pak.cart >= 0 ? b64_encode(p.my_pak.save) : std::string();
            const int n = static_cast<int>((b64.size() + kPakChunk - 1) / kPakChunk);
            for (int i = 0; i < std::max(1, n); ++i) {
                json m = {{"k", "tpak"}, {"v", 1}, {"seat", s.local_slot}, {"cart", p.my_pak.cart},
                          {"save", sha}, {"n", n}, {"i", i},
                          {"d", n ? b64.substr(static_cast<std::size_t>(i) * kPakChunk, kPakChunk) : ""}};
                p.client.signal(kPakSignal, m.dump());
            }
            // Ours, as every peer will hold it.
            if (p.my_pak.cart >= 0 && !p.scope->pak_dir.empty()) {
                std::error_code ec;
                fs::create_directories(p.scope->pak_dir, ec);
                const fs::path f = fs::path(p.scope->pak_dir) / "mine.sav";
                std::ofstream(f, std::ios::binary) << p.my_pak.save;
                p.paks[s.player_id] = {s.local_slot, p.my_pak.cart, sha, f.string()};
            } else {
                p.paks[s.player_id] = {s.local_slot, -1, "", ""};
            }
        }
        // Theirs.
        for (const np::SignalMsg& sm : s.signals) {
            if (sm.seq <= p.signals_seen) continue;
            p.signals_seen = sm.seq;
            if (sm.type != kPakSignal) continue;
            const json m = json::parse(sm.text, nullptr, false);
            if (!m.is_object() || m.value("k", "") != "tpak") continue;
            Page::PakParts& pp = p.pak_parts[sm.from_player_id];
            const int n = m.value("n", 0), i = m.value("i", 0);
            if (pp.n != n || pp.save_sha != m.value("save", std::string())) pp = Page::PakParts{};
            pp.seat = m.value("seat", -1);
            pp.cart = m.value("cart", -1);
            pp.n = n;
            pp.save_sha = m.value("save", std::string());
            pp.parts[i] = m.value("d", std::string());
            if (static_cast<int>(pp.parts.size()) < std::max(1, n)) continue;
            if (pp.cart < 0 || n == 0) {
                p.paks[sm.from_player_id] = {pp.seat, -1, "", ""};
                continue;
            }
            std::string b64, bytes;
            for (const auto& [k, v] : pp.parts) b64 += v;
            if (!b64_decode(b64, &bytes) || sha256_hex(bytes) != pp.save_sha) {
                p.room_status = "A player's Transfer Pak save arrived damaged; it will be sent again.";
                pp = Page::PakParts{};
                p.pak_members.clear(); // resend ours too, so theirs follows
                continue;
            }
            std::error_code ec;
            fs::create_directories(p.scope->pak_dir, ec);
            const fs::path f = fs::path(p.scope->pak_dir) / (sm.from_player_id + ".sav");
            std::ofstream(f, std::ios::binary) << bytes;
            p.paks[sm.from_player_id] = {pp.seat, pp.cart, pp.save_sha, f.string()};
        }
    }

    if (s.launch_pending) {
        const long long sid = s.launch.value("session_id", 0LL);
        if (sid != p.launched_session) {
            p.launched_session = sid;
            if (!p.scope) {
                // A library game: the game takes the match from a launch record.
                std::string why;
                if (!p.lib || !p.lib->capable) {
                    p.room_status = "The host started the match, but " +
                                    (p.lib && !p.lib->why.empty()
                                         ? p.lib->why
                                         : std::string("Retro cannot start this game."));
                } else if (np::start_handoff_match(hub.paths, p.lib->title, p.lib->opts, s,
                                                   p.lib->work_dir, &p.lib->record, &why)) {
                    p.lib->record_open = true;
                    p.room_status = "Starting " + p.lib->title.name + "\xE2\x80\xA6";
                } else {
                    p.room_status = "Could not start the match: " + why;
                }
                p.client.ack_launch();
            } else if (!p.scope->launch) {
                p.room_status = "The host started the match, but this game cannot run it here.";
            } else {
                NetplayLaunch l;
                l.session_id = static_cast<std::uint32_t>(sid);
                l.slots = std::max(2, s.launch.value("max_slots", s.max_slots));
                l.transport = s.launch.value("transport", std::string());
                l.relay_endpoint = s.launch.value("relay_endpoint", std::string());
                l.host_endpoint = s.launch.value("host_endpoint", std::string());
                l.host = s.is_host;
                l.tpak = s.match_caps.is_object() && s.match_caps.value("tpak", false);
                l.seat = -1;
                const auto slots = s.launch.find("slots");
                if (slots != s.launch.end() && slots->is_array()) {
                    for (const json& e : *slots) {
                        const int seat = e.value("slot", -1);
                        if (seat < 0 || seat >= 32) continue;
                        l.occupied |= 1u << seat;
                        if (e.value("player_id", std::string()) == s.player_id) l.seat = seat;
                    }
                }
                if (l.tpak) {
                    for (const auto& [pid, got] : p.paks) {
                        if (got.seat < 0 || got.seat >= 4 || got.cart < 0) continue;
                        l.tpak_rom[static_cast<std::size_t>(got.seat)] =
                            p.scope->cart_rom ? p.scope->cart_rom(got.cart) : std::string();
                        l.tpak_save[static_cast<std::size_t>(got.seat)] = got.path;
                    }
                }
                if (l.host && l.transport == "host") {
                    // The runner binds the port this held: free it first.
                    l.host_port = p.host_port.status().local_port;
                    p.host_port.stop();
                    p.host_port_open = false;
                }
                if (l.seat < 0) {
                    p.room_status = "The match started without you in a player seat "
                                    "(spectating is not built yet).";
                } else {
                    const std::string why = p.scope->launch(l);
                    p.room_status = why.empty() ? std::string() : "Could not start the match: " + why;
                    p.client.ack_launch();
                }
            }
        }
    }

    // Signed in after connecting as a guest: connect again as the account,
    // but never out from under a seat.
    if (p.client_started && !s.in_room && !p.client_signed &&
        p.account.state() == Account::State::SignedIn)
        connect(hub);

    // The room is entered and left by seat state, as in recomp-ui.
    if (s.in_room && !p.was_in_room && p.scope && s.match_caps.is_object() &&
        s.match_caps.value("tpak", false) && p.scope->tpak_problem) {
        if (const std::string why = p.scope->tpak_problem(); !why.empty()) {
            p.client.leave();
            p.status = "That is a Transfer Pak lobby. " + why;
        }
    }
    if (s.in_room && !p.was_in_room) {
        p.view = View::Room;
        p.status.clear();
        p.room_status.clear();
        p.saw_launch = false;
        p.room_chat_seen = 0;
    } else if (!s.in_room && p.was_in_room) {
        p.view = View::Online;
        if (!s.room_status.empty()) p.status = s.room_status;
        // Hosting or joining named a game; "All games" needs the connection back.
        if (p.scope_dirty && p.filter.empty()) connect(hub);
    }
    p.was_in_room = s.in_room;
}

} // namespace

void draw_netplay_page(HubModel& hub, const Theme& th, SDL_Window* /*window*/,
                       const NetplayScope* scope) {
    Page& p = page();
    p.scope = scope;
    ensure_init(hub);
    ingest(hub);
    const std::vector<Game> games = installed_games(hub);

    // In the shell (not one game's Direct mode) the page is the overlay's.
    if (!scope && retcomm::hub::overlay::enabled()) {
        draw_netplay_overlay(hub, th, games);
        return;
    }

    // Page header: title, the name button, and Back to the mode page.
    ImGui::Dummy(ImVec2(0, 4));
    const float header_x = ImGui::GetCursorPosX();
    const float header_w = ImGui::GetContentRegionAvail().x;
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextColored(th.text, "NETPLAY");
    ImGui::SetWindowFontScale(1.f);
    ImGui::SameLine();
    const char* where = p.view == View::Mode ? "" : p.view == View::SignIn ? "Sign in"
                        : p.view == View::Lan    ? "LAN / Direct IP"
                        : p.view == View::Room   ? "Lobby"
                                                 : "Online";
    if (where[0]) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(th.text_muted, "\xC2\xB7 %s", where);
    }
    {
        const std::string name = local_name(hub);
        const std::string label = (name.empty() ? std::string("Set player name") : name) + "##np_name_btn";
        const float right = header_x + header_w;
        const bool back = p.view == View::Online || p.view == View::Lan || p.view == View::SignIn;
        ImGui::SameLine(right - 170.f - (back ? 120.f : 0.f));
        if (ImGui::Button(label.c_str(), ImVec2(170, 34))) p.open_name = true;
        if (back) {
            ImGui::SameLine();
            if (ImGui::Button("< Back", ImVec2(110, 34))) {
                p.view = View::Mode;
                p.status.clear();
            }
        }
    }
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));

    switch (p.view) {
    case View::Host: p.view = View::Online; break;
    case View::Mode: draw_mode(hub, th); break;
    case View::SignIn: draw_signin(hub, th); break;
    case View::Online: draw_online(hub, th, games); break;
    case View::Lan: draw_lan(hub, th, games); break;
    case View::Room: draw_room(hub, th, games); break;
    }
    draw_modals(hub, th, games);
}

std::vector<std::string> netplay_runner_args(const NetplayLaunch& l, std::string* error) {
    // The clock epoch every peer agrees on: fixed, so it needs no negotiation
    // (a Game Boy cartridge's clock reads guest time from it).
    constexpr const char* kEpoch = "1700000000";
    std::vector<std::string> a = {"--net-slot",    std::to_string(l.seat),
                                  "--net-slots",   std::to_string(l.slots),
                                  "--net-session", std::to_string(l.session_id),
                                  "--net-epoch",   kEpoch,
                                  "--net-delay",   "2"};
    if (l.occupied) {
        a.push_back("--net-occupied");
        a.push_back(std::to_string(l.occupied));
    }
    if (l.transport == "sfu") {
        if (l.relay_endpoint.empty()) {
            if (error) *error = "the server named no relay";
            return {};
        }
        a.insert(a.end(), {"--net-relay", l.relay_endpoint, "--net-bind", "0.0.0.0:0"});
    } else if (l.transport == "host") {
        std::string ip;
        int port = 0;
        if (!np::split_endpoint(l.host_endpoint, &ip, &port)) {
            if (error) *error = "the host's address '" + l.host_endpoint + "' is not ip:port";
            return {};
        }
        if (l.host)
            a.insert(a.end(), {"--net-bind",
                               "0.0.0.0:" + std::to_string(l.host_port ? l.host_port : port)});
        else a.insert(a.end(), {"--net-peer", l.host_endpoint, "--net-bind", "0.0.0.0:0"});
    } else {
        if (error) *error = "the server chose transport '" + l.transport + "', which this hub does not run";
        return {};
    }
    return a;
}

bool netplay_step_back() {
    Page& p = page();
    if (p.view == View::Online || p.view == View::Lan || p.view == View::SignIn) {
        p.view = View::Mode;
        p.status.clear();
        return true;
    }
    if (p.view == View::Host) {
        p.view = View::Online;
        return true;
    }
    return false;
}

void netplay_shutdown() {
    Page& p = page();
    p.host_port.stop();
    p.host_port.unmap();
    p.client.stop(); // sends leave when seated
    p.lan.stop();
    p.account.shutdown();
}

} // namespace retcomm::hub
