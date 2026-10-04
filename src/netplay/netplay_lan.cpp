#include "retcomm/netplay_lan.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
constexpr sock_t kNoSock = INVALID_SOCKET;
#define RETCOMM_CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
constexpr sock_t kNoSock = -1;
#define RETCOMM_CLOSESOCK ::close
#endif

namespace retcomm::netplay {

namespace {

constexpr long long kRnetStaleMs = 5000;   // recomp-net's RNET_BC_STALE_MS
constexpr long long kMotkStaleMs = 8000;   // psxrecomp's kAeLanDiscoverStaleMs
constexpr long long kDirectStaleMs = 30000;
constexpr long long kBrowseEveryMs = 3000;
constexpr long long kProbeWaitMs = 1500;

long long steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::vector<std::string> lines_of(const std::string& pkt) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : pkt) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

int to_int(const std::string& s, int def) {
    if (s.empty()) return def;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return end && end != s.c_str() ? static_cast<int>(v) : def;
}

void net_startup() {
#if defined(_WIN32)
    static bool done = false;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = true;
    }
#endif
}

sock_t udp_socket(unsigned short port, bool reuse, std::string* err) {
    net_startup();
    sock_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSock) {
        if (err) *err = "cannot open a UDP socket";
        return kNoSock;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
    if (reuse) {
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#if defined(SO_REUSEPORT)
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&one), sizeof(one));
#endif
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        if (err) *err = "UDP port " + std::to_string(port) + " is in use by another program";
        RETCOMM_CLOSESOCK(s);
        return kNoSock;
    }
    return s;
}

void send_to(sock_t s, const std::string& ip, int port, const std::string& msg) {
    if (s == kNoSock) return;
    sockaddr_in d{};
    d.sin_family = AF_INET;
    d.sin_port = htons(static_cast<unsigned short>(port));
    if (inet_pton(AF_INET, ip.c_str(), &d.sin_addr) != 1) return;
    ::sendto(s, msg.data(), static_cast<int>(msg.size()), 0, reinterpret_cast<sockaddr*>(&d),
             sizeof(d));
}

// One datagram, or false after timeout_ms with nothing waiting.
bool recv_one(sock_t s, int timeout_ms, std::string* pkt, std::string* src_ip) {
    if (s == kNoSock) return false;
#if defined(_WIN32)
    WSAPOLLFD p{s, POLLRDNORM, 0};
    if (WSAPoll(&p, 1, timeout_ms) <= 0) return false;
#else
    pollfd p{s, POLLIN, 0};
    if (::poll(&p, 1, timeout_ms) <= 0) return false;
#endif
    char buf[2048];
    sockaddr_in from{};
#if defined(_WIN32)
    int fl = sizeof(from);
#else
    socklen_t fl = sizeof(from);
#endif
    const auto n = ::recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n <= 0) return false;
    pkt->assign(buf, static_cast<size_t>(n));
    char ip[64] = {};
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    *src_ip = ip;
    return true;
}

} // namespace

bool split_endpoint(const std::string& ep, std::string* ip, int* port) {
    const auto colon = ep.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    const std::string host = ep.substr(0, colon);
    const int p = to_int(ep.substr(colon + 1), -1);
    in_addr tmp{};
    if (p <= 0 || p > 65535 || inet_pton(AF_INET, host.c_str(), &tmp) != 1) return false;
    if (ip) *ip = host;
    if (port) *port = p;
    return true;
}

bool parse_rnet_announce(const std::string& pkt, LanRoom* out) {
    const auto l = lines_of(pkt);
    if (l.size() < 5 || l[0] != "RNETBC1" || l[1] != "ANNOUNCE") return false;
    // An online host announces its server lobby_id for same-LAN latency; that
    // room is joined through the server, not here.
    if (l[2].rfind("lan:", 0) != 0 || !split_endpoint(l[3], nullptr, nullptr)) return false;
    LanRoom r;
    r.proto = LanRoom::Proto::Rnet;
    r.lobby_id = l[2];
    r.endpoint = l[3];
    r.game_name = l[4];
    if (l.size() > 5) r.game_version = l[5];
    if (l.size() > 6) r.name = l[6];
    if (l.size() > 7) {
        int pw = 0, players = 0, max = 0, started = 0;
        if (std::sscanf(l[7].c_str(), "pw=%d players=%d max=%d started=%d", &pw, &players, &max,
                        &started) == 4) {
            r.has_password = pw != 0;
            r.players = players;
            r.max_slots = max;
            r.started = started != 0;
        }
    }
    if (out) *out = r;
    return true;
}

bool parse_motk_beacon(const std::string& pkt, const std::string& src_ip, LanRoom* out) {
    const auto l = lines_of(pkt);
    // "MOTK1 BEACON", name, game, endpoint, players, max, has_pw, session,
    // delay, pred, rollback; psxrecomp accepts it with the first six.
    if (l.size() < 7 || l[0] != "MOTK1 BEACON") return false;
    LanRoom r;
    r.proto = LanRoom::Proto::Psx;
    r.name = l[1];
    r.game_name = l[2];
    std::string adv_ip;
    int port = 0;
    if (!split_endpoint(l[3], &adv_ip, &port)) port = 7777;
    r.endpoint = (src_ip.empty() ? adv_ip : src_ip) + ":" + std::to_string(port);
    r.lobby_id = "lan:" + r.endpoint;
    r.players = to_int(l[4], 0);
    r.max_slots = std::max(2, to_int(l[5], 2));
    r.has_password = to_int(l[6], 0) != 0;
    if (out) *out = r;
    return true;
}

LanBrowser::~LanBrowser() { stop(); }

void LanBrowser::start(unsigned short beacon_port, int browse_lo, int browse_hi,
                       const std::string& broadcast) {
    if (running()) return;
    beacon_port_ = beacon_port ? beacon_port : kRnetBeaconPort;
    browse_lo_ = browse_lo;
    browse_hi_ = browse_hi;
    broadcast_ = broadcast;
    stop_ = false;
    worker_ = std::thread([this] { run(); });
}

void LanBrowser::stop() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}

void LanBrowser::refresh() {
    std::lock_guard<std::mutex> lk(mu_);
    refresh_queued_ = true;
}

void LanBrowser::probe(const std::string& ip, int port) {
    std::lock_guard<std::mutex> lk(mu_);
    probe_ip_ = ip;
    probe_port_ = port;
    probe_queued_ = true;
    probe_ = Probe{ip + ":" + std::to_string(port), true, false};
}

std::vector<LanRoom> LanBrowser::rooms() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<LanRoom> out = rooms_;
    std::sort(out.begin(), out.end(),
              [](const LanRoom& a, const LanRoom& b) { return a.name < b.name; });
    return out;
}

std::string LanBrowser::listen_error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return listen_error_;
}

LanBrowser::Probe LanBrowser::last_probe() const {
    std::lock_guard<std::mutex> lk(mu_);
    return probe_;
}

void LanBrowser::run() {
    std::string err;
    const sock_t beacon = udp_socket(beacon_port_, true, &err);
    {
        std::lock_guard<std::mutex> lk(mu_);
        listen_error_ = beacon == kNoSock ? err : std::string();
    }
    // Browse replies come back to whatever port this one got.
    const sock_t browse = udp_socket(0, false, &err);

    long long next_browse = 0;
    long long probe_deadline = 0;
    std::string probe_ep;
    auto upsert = [&](LanRoom r, long long now) {
        r.seen_ms = now;
        std::lock_guard<std::mutex> lk(mu_);
        if (!probe_ep.empty() && r.proto == LanRoom::Proto::Psx && r.endpoint == probe_ep) {
            r.direct = true;
            probe_.pending = false;
            probe_.answered = true;
        }
        for (LanRoom& have : rooms_) {
            if (have.lobby_id == r.lobby_id && have.proto == r.proto) {
                r.direct = r.direct || have.direct;
                have = r;
                return;
            }
        }
        rooms_.push_back(r);
    };

    while (!stop_) {
        const long long now = steady_ms();
        bool do_browse = false, do_probe = false;
        std::string pip;
        int pport = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (refresh_queued_ || now >= next_browse) {
                refresh_queued_ = false;
                do_browse = true;
            }
            if (probe_queued_) {
                probe_queued_ = false;
                do_probe = true;
                pip = probe_ip_;
                pport = probe_port_;
            }
            if (probe_.pending && probe_deadline && now > probe_deadline) {
                probe_.pending = false;
                probe_deadline = 0;
            }
            // Expire what stopped announcing.
            rooms_.erase(std::remove_if(rooms_.begin(), rooms_.end(),
                                        [&](const LanRoom& r) {
                                            const long long ttl =
                                                r.direct ? kDirectStaleMs
                                                : r.proto == LanRoom::Proto::Psx ? kMotkStaleMs
                                                                                 : kRnetStaleMs;
                                            return now - r.seen_ms > ttl;
                                        }),
                         rooms_.end());
        }
        if (do_browse) {
            next_browse = now + kBrowseEveryMs;
            for (int p = browse_lo_; p <= browse_hi_; ++p)
                send_to(browse, broadcast_, p, "MOTK1 BROWSE\n");
        }
        if (do_probe) {
            probe_ep = pip + ":" + std::to_string(pport);
            probe_deadline = now + kProbeWaitMs;
            send_to(browse, pip, pport, "MOTK1 BROWSE\n");
        }

        std::string pkt, src;
        while (recv_one(browse, 0, &pkt, &src)) {
            LanRoom r;
            if (parse_motk_beacon(pkt, src, &r)) upsert(r, steady_ms());
        }
        if (recv_one(beacon, 100, &pkt, &src)) {
            LanRoom r;
            if (parse_rnet_announce(pkt, &r)) upsert(r, steady_ms());
        } else if (beacon == kNoSock) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (beacon != kNoSock) RETCOMM_CLOSESOCK(beacon);
    if (browse != kNoSock) RETCOMM_CLOSESOCK(browse);
}

} // namespace retcomm::netplay
