#pragma once

// LAN / Direct IP room discovery, for the hub's Netplay page. The two wire
// protocols that games use today, both plain UDP text:
//
//   RNETBC1  recomp-net's beacon (recomp-net/include/recomp_net/lan_beacon.h):
//            hosts broadcast ANNOUNCE to 255.255.255.255:48777 about once a
//            second; listeners bind 48777 with SO_REUSEADDR. snesrecomp,
//            n64lle, nesrecomp -- everything on recomp-ui's shared backend.
//   MOTK1    psxrecomp's in-game LAN lobby (runtime/src/main.cpp): a browser
//            broadcasts "MOTK1 BROWSE\n" to ports 7777..7808 and each waiting
//            host answers the sender with a unicast "MOTK1 BEACON".
//
// This only LISTENS and ASKS. In both designs the host's game process is the
// lobby: it owns the advertised UDP port and hands it to the netplay session
// at launch. The hub cannot sit in a LAN room without taking that away from
// the game, so hosting and joining stay in the game until games accept a
// session negotiated elsewhere (docs/NETPLAY_UX_PLAN.md).
//
// A Direct IP probe sends MOTK1 BROWSE straight to an address: a PSX host
// answers with its BEACON. A recomp-net host answers nothing but a JOIN
// (which would seat the hub), so an unanswered probe says so rather than
// "nobody there".

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace retcomm::netplay {

constexpr unsigned short kRnetBeaconPort = 48777;
constexpr int kMotkBrowsePortLo = 7777;
constexpr int kMotkBrowsePortHi = 7808;

struct LanRoom {
    enum class Proto { Rnet, Psx };
    Proto proto = Proto::Rnet;
    std::string lobby_id;   // "lan:<endpoint>"
    std::string endpoint;   // ip:port a guest's game joins
    std::string game_name, game_version, name;
    int players = 0, max_slots = 0;
    bool has_password = false, started = false;
    bool direct = false;    // answered a Direct IP probe
    long long seen_ms = 0;  // steady clock, for expiry
};

// One RNETBC1 ANNOUNCE packet. False for anything else, and for an online
// host's same-LAN announce (its lobby_id is the server's, not "lan:"), which
// belongs to the server list.
bool parse_rnet_announce(const std::string& pkt, LanRoom* out);
// One "MOTK1 BEACON" reply. The endpoint is the reply's source address with
// the advertised port, as psxrecomp's own browser takes it.
bool parse_motk_beacon(const std::string& pkt, const std::string& src_ip, LanRoom* out);
// "a.b.c.d:port" -> parts; false when it is not that.
bool split_endpoint(const std::string& ep, std::string* ip, int* port);

class LanBrowser {
public:
    LanBrowser() = default;
    ~LanBrowser();
    LanBrowser(const LanBrowser&) = delete;
    LanBrowser& operator=(const LanBrowser&) = delete;

    // `beacon_port` 0 = 48777; `browse_lo..hi` the MOTK ports asked (tests
    // use loopback and one port). Idempotent.
    void start(unsigned short beacon_port = 0, int browse_lo = kMotkBrowsePortLo,
               int browse_hi = kMotkBrowsePortHi, const std::string& broadcast = "255.255.255.255");
    void stop();
    bool running() const { return worker_.joinable(); }

    // Ask again now (the Refresh button); otherwise every 3 s.
    void refresh();
    // Direct IP: MOTK1 BROWSE to that address only. The answer, if any,
    // arrives as a room with direct = true.
    void probe(const std::string& ip, int port);

    // Fresh rooms (RNETBC1 5 s, MOTK1 8 s, direct 30 s), sorted by name.
    std::vector<LanRoom> rooms() const;
    // Why listening on the beacon port failed, if it did (another program
    // holding it without SO_REUSEADDR). Browsing MOTK still works then.
    std::string listen_error() const;
    // The last Direct IP probe: its address, and whether it was answered.
    struct Probe {
        std::string endpoint;
        bool pending = false, answered = false;
    };
    Probe last_probe() const;

private:
    void run();

    mutable std::mutex mu_;
    std::vector<LanRoom> rooms_;
    std::string listen_error_;
    Probe probe_;
    std::string probe_ip_;
    int probe_port_ = 0;
    bool probe_queued_ = false;
    bool refresh_queued_ = true;
    unsigned short beacon_port_ = kRnetBeaconPort;
    int browse_lo_ = kMotkBrowsePortLo, browse_hi_ = kMotkBrowsePortHi;
    std::string broadcast_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

} // namespace retcomm::netplay
