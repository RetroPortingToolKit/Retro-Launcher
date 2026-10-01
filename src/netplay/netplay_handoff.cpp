// A library game started into a match the hub's lobby negotiated:
// docs/NETPLAY_HANDOFF.md, and include/retcomm/netplay_handoff.hpp.

#include "retcomm/netplay_handoff.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>

namespace retcomm::netplay {

using json = nlohmann::json;

namespace {

bool write_file(const fs::path& path, const std::string& text, std::string* why) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out.good()) {
        if (why) *why = "cannot write " + path.string();
        return false;
    }
    return true;
}

bool read_json(const fs::path& path, json* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        *out = json::parse(ss.str());
        return out->is_object();
    } catch (const std::exception&) {
        return false;
    }
}

// One file per request, so two queries never read each other's answer.
fs::path fresh_path(const fs::path& dir, const std::string& stem) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return dir / (stem + "-" + std::to_string(now) + ".json");
}

} // namespace

json handoff_settle_request(const Snapshot& s) {
    json r = {{"v", 1},
              {"handoff_query", "settle"},
              {"handoff_player_id", s.player_id},
              {"handoff_lobby_url", s.lobby_url},
              {"handoff_seat", s.seat_msg.is_object() ? s.seat_msg : json::object()}};
    if (s.room_msg.is_object()) r["handoff_room"] = s.room_msg;
    return r;
}

json handoff_record(const Snapshot& s) {
    json r = handoff_settle_request(s);
    r.erase("handoff_query");
    r["handoff_launch"] = s.launch;
    return r;
}

QueryAnswer run_handoff_query(const Paths& paths, const Title& title, LaunchOptions opts,
                              const json& request, const fs::path& work_dir) {
    QueryAnswer qa;
    const fs::path req = fresh_path(work_dir, "query");
    if (!write_file(req, request.dump(), &qa.error)) return qa;
    opts.mode = LaunchMode::Netplay;
    opts.netplay_query = fs::absolute(req);
    opts.netplay_record.clear();
    opts.detach = false;
    opts.dry_run = false;
    const LaunchResult r = launch_title(paths, title, opts);
    fs::path answer = req;
    answer += ".answer";
    if (!read_json(answer, &qa.answer)) {
        qa.error = r.ok ? title.name + " did not answer the netplay query"
                        : "could not run " + title.name;
    } else {
        qa.ok = true;
    }
    std::error_code ec;
    fs::remove(req, ec);
    fs::remove(answer, ec);
    return qa;
}

bool start_handoff_match(const Paths& paths, const Title& title, LaunchOptions opts,
                         const Snapshot& s, const fs::path& work_dir, fs::path* record,
                         std::string* why) {
    const fs::path rec =
        fs::absolute(work_dir / ("launch-" + std::to_string(s.launch.value("session_id", 0LL)) +
                                 ".json"));
    std::error_code ec;
    fs::path status = rec;
    status += ".status";
    fs::remove(status, ec);
    if (!write_file(rec, handoff_record(s).dump(), why)) return false;
    opts.mode = LaunchMode::Netplay;
    opts.netplay_record = rec;
    opts.netplay_query.clear();
    opts.detach = true;
    opts.dry_run = false;
    const LaunchResult r = launch_title(paths, title, opts);
    if (!r.ok) {
        if (why) *why = "could not start " + title.name;
        return false;
    }
    if (record) *record = rec;
    return true;
}

HandoffState read_handoff_status(const fs::path& record, std::string* why) {
    fs::path status = record;
    status += ".status";
    json j;
    if (!read_json(status, &j)) return HandoffState::Pending;
    if (j.value("ok", false)) return HandoffState::Started;
    if (why) *why = j.value("why", std::string("the game refused the match"));
    return HandoffState::Refused;
}

} // namespace retcomm::netplay
