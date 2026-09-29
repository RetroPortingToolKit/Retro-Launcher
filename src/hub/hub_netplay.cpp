#include "hub/hub_netplay.hpp"

#include "hub/hub_model.hpp"
#include "hub/hub_theme.hpp"
#include "hub/hub_widgets.hpp"
#include "retcomm/config.hpp"
#include "retcomm/netplay_account.hpp"
#include "retcomm/netplay_client.hpp"
#include "retcomm/netplay_lan.hpp"
#include "retcomm/netplay_nat.hpp"

#include "imgui.h"
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
            const bool ok = acct_->login(&info, &err, 300, true, {}, &cancel_);
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
    std::atomic<bool> cancel_{false};
    std::thread th_;
};

// ---- the page's state --------------------------------------------------------------------

enum class View { Mode, SignIn, Online, Lan, Room };

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
};

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
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
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
    p.client.join_as(r.lobby_id, "", g->game_name, g->pin);
    if (p.filter.empty()) p.scope_dirty = true;
    p.status = "Joining\xE2\x80\xA6";
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
        if (danger_button("Kick", th, ImVec2(48, 32))) p.client.kick(slot);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", why ? why : "Kick player");
    }
    ImGui::PopID();
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
    } else {
        ImGui::TextColored(th.text_muted,
                           "You're seated from Retro's library. Starting a match from here is "
                           "not built: open the game in its own app (Direct mode) to play, or "
                           "start it from the game's own Netplay menu.");
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
        ImGui::TableSetupColumn("Kick", ImGuiTableColumnFlags_WidthFixed, 56.f);
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
        ImGui::TableSetupColumn("Kick", ImGuiTableColumnFlags_WidthFixed, 56.f);
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
        const char* why = seated < 2   ? "Waiting for another player to join"
                          : !p.scope   ? "Open the game in its own app (Direct mode) to play: "
                                         "the library cannot start a match"
                          : tpak_room  ? "Transfer Pak matches need the pak save exchange, "
                                         "which is not built yet"
                                       : nullptr;
        ImGui::BeginDisabled(why != nullptr);
        if (good_button("PLAY", th, ImVec2(play_w, 46))) {
            p.client.start_match();
            p.room_status = "Starting the match\xE2\x80\xA6";
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
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
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
            ImGui::SetNextItemWidth(430.f);
            if (ImGui::BeginCombo("##host_game", g.title.c_str())) {
                for (size_t i = 0; i < games.size(); ++i)
                    if (ImGui::Selectable(games[i].title.c_str(), p.host_game == static_cast<int>(i)))
                        p.host_game = static_cast<int>(i);
                ImGui::EndCombo();
            }
            ImGui::TextColored(th.text_muted, "Version %s \xC2\xB7 guests need the same build",
                               g.pin.c_str());
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
                if (p.host_error.empty()) {
                    json caps = json::object();
                    if (p.scope) {
                        caps["tpak"] = p.host_tpak;
                        caps["relay"] = "host"; // the lobby server's relay only as a fallback
                    }
                    p.client.create_for(g.game_name, g.pin, name, p.host_pw, p.host_max,
                                        p.host_spectators, caps);
                    if (p.filter.empty()) p.scope_dirty = true;
                    p.status = "Creating lobby\xE2\x80\xA6";
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
            p.client.join_as(p.join_pw_lobby, p.join_pw, p.join_pw_game, p.join_pw_version);
            if (p.filter.empty()) p.scope_dirty = true;
            p.status = "Joining\xE2\x80\xA6";
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
        if (hs.done) {
            p.relay_note = "Host relay: " + hs.detail +
                           (hs.probes_answered ? " Guests reached you (" +
                                                     std::to_string(hs.probes_answered) + " probe(s))."
                                               : "");
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

    if (s.launch_pending) {
        const long long sid = s.launch.value("session_id", 0LL);
        if (sid != p.launched_session) {
            p.launched_session = sid;
            if (!p.scope || !p.scope->launch) {
                p.room_status = "The host started the match, but the library cannot run it. "
                                "Open the game in its own app (Direct mode) to play.";
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

void netplay_shutdown() {
    Page& p = page();
    p.host_port.stop();
    p.host_port.unmap();
    p.client.stop(); // sends leave when seated
    p.lan.stop();
    p.account.shutdown();
}

} // namespace retcomm::hub
