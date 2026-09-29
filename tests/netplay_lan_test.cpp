// netplay_lan: parsing both LAN beacons, and a LanBrowser that hears a
// recomp-net announce and a psxrecomp host's BEACON over loopback. Run as
// `retcomm-netplay-lan-test`. POSIX only (the fake hosts use BSD sockets).

#include "retcomm/netplay_lan.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace retcomm::netplay;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int udp_bound(int port) {
    const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<unsigned short>(port));
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(s);
        return -1;
    }
    return s;
}

void send_loop(int s, int port, const std::string& msg) {
    sockaddr_in d{};
    d.sin_family = AF_INET;
    d.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    d.sin_port = htons(static_cast<unsigned short>(port));
    ::sendto(s, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&d), sizeof(d));
}

template <class F>
bool wait_for(F f, int ms) {
    for (int i = 0; i < ms / 20; ++i) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}

} // namespace

int main() {
    // ---- RNETBC1
    {
        LanRoom r;
        check(parse_rnet_announce("RNETBC1\nANNOUNCE\nlan:192.168.1.5:7777\n192.168.1.5:7777\n"
                                  "Gundam Wing\n0.1.1\nAlex's Lobby\n"
                                  "pw=1 players=1 max=2 started=0\n",
                                  &r),
              "a V2 announce parses");
        check(r.proto == LanRoom::Proto::Rnet && r.lobby_id == "lan:192.168.1.5:7777" &&
                  r.endpoint == "192.168.1.5:7777" && r.game_name == "Gundam Wing" &&
                  r.game_version == "0.1.1" && r.name == "Alex's Lobby" && r.has_password &&
                  r.players == 1 && r.max_slots == 2 && !r.started,
              "every V2 field");
        check(parse_rnet_announce("RNETBC1\nANNOUNCE\nlan:10.0.0.2:7777\n10.0.0.2:7777\nGame\n", &r) &&
                  r.name.empty() && r.max_slots == 0,
              "a V1 announce (no version, name or flags) still parses");
        check(!parse_rnet_announce("RNETBC1\nANNOUNCE\n3f2a-online-id\n10.0.0.2:7777\nGame\n", &r),
              "an online host's same-LAN announce is not a LAN room");
        check(!parse_rnet_announce("RNETBC1\nANNOUNCE\nlan:x\nnot-an-endpoint\nGame\n", &r),
              "a bad endpoint is refused");
        check(!parse_rnet_announce("MOTK1 BEACON\nx\ny\n1.2.3.4:5\n1\n2\n0\n", &r),
              "another protocol is not RNETBC1");
    }
    // ---- MOTK1 BEACON
    {
        LanRoom r;
        check(parse_motk_beacon("MOTK1 BEACON\nFriday\nTombaRecomp\n0.0.0.0:7781\n1\n2\n0\n3\n2\n6\n1\n",
                                "192.168.1.9", &r),
              "a BEACON parses");
        check(r.proto == LanRoom::Proto::Psx && r.endpoint == "192.168.1.9:7781" &&
                  r.lobby_id == "lan:192.168.1.9:7781" && r.name == "Friday" &&
                  r.game_name == "TombaRecomp" && r.players == 1 && r.max_slots == 2 &&
                  !r.has_password,
              "the endpoint is the source address with the advertised port");
        check(!parse_motk_beacon("MOTK1 BEACON\nx\ny\n", "1.2.3.4", &r), "a short BEACON is refused");
    }
    {
        std::string ip;
        int port = 0;
        check(split_endpoint("10.1.2.3:7777", &ip, &port) && ip == "10.1.2.3" && port == 7777,
              "split_endpoint");
        check(!split_endpoint("host:7777", &ip, &port) && !split_endpoint("10.1.2.3", &ip, &port) &&
                  !split_endpoint("10.1.2.3:0", &ip, &port),
              "split_endpoint refuses names, a missing port and port 0");
    }

    // ---- a browser over loopback
    constexpr int kBeaconPort = 48991;
    constexpr int kPsxHostPort = 7799;
    const int psx = udp_bound(kPsxHostPort);
    check(psx >= 0, "fake PSX host bound");
    std::atomic<bool> done{false};
    std::atomic<int> browses{0};
    // The fake PSX host answers each BROWSE with its BEACON, to the sender.
    std::thread host([&] {
        while (!done && psx >= 0) {
            pollfd p{psx, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            char buf[512];
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            const auto n = ::recvfrom(psx, buf, sizeof(buf), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fl);
            if (n > 0 && std::string(buf, static_cast<size_t>(n)) == "MOTK1 BROWSE\n") {
                ++browses;
                const std::string beacon = "MOTK1 BEACON\nPSX Room\nTombaRecomp\n0.0.0.0:7799\n1\n2\n1\n1\n2\n6\n1\n";
                ::sendto(psx, beacon.data(), beacon.size(), 0, reinterpret_cast<sockaddr*>(&from), fl);
            }
        }
    });

    LanBrowser b;
    b.start(kBeaconPort, kPsxHostPort, kPsxHostPort, "127.0.0.1");
    const int announcer = ::socket(AF_INET, SOCK_DGRAM, 0);
    send_loop(announcer, kBeaconPort,
              "RNETBC1\nANNOUNCE\nlan:127.0.0.1:7801\n127.0.0.1:7801\nGundam Wing\n0.1.1\nSNES Room\n"
              "pw=0 players=1 max=2 started=0\n");
    const bool both = wait_for(
        [&] {
            send_loop(announcer, kBeaconPort,
                      "RNETBC1\nANNOUNCE\nlan:127.0.0.1:7801\n127.0.0.1:7801\nGundam Wing\n0.1.1\n"
                      "SNES Room\npw=0 players=1 max=2 started=0\n");
            return b.rooms().size() == 2;
        },
        3000);
    check(both, "the browser hears the announce and the BEACON");
    check(b.listen_error().empty(), "the beacon port was bound");
    const auto rooms = b.rooms();
    if (rooms.size() == 2) {
        check(rooms[0].name == "PSX Room" && rooms[0].proto == LanRoom::Proto::Psx &&
                  rooms[0].endpoint == "127.0.0.1:7799" && rooms[0].has_password,
              "the PSX room, sorted first by name");
        check(rooms[1].name == "SNES Room" && rooms[1].proto == LanRoom::Proto::Rnet &&
                  rooms[1].game_version == "0.1.1",
              "the recomp-net room");
    }

    // Direct IP: a probe of the PSX host is answered; one of nothing is not.
    b.probe("127.0.0.1", kPsxHostPort);
    check(wait_for([&] { return b.last_probe().answered; }, 2000), "a probe of a PSX host is answered");
    for (const LanRoom& r : b.rooms())
        if (r.proto == LanRoom::Proto::Psx) check(r.direct, "the probed room is marked direct");
    b.probe("127.0.0.1", 7811);
    check(b.last_probe().pending, "a probe starts pending");
    check(wait_for([&] { return !b.last_probe().pending; }, 3000) && !b.last_probe().answered,
          "a probe of nothing times out unanswered");

    b.stop();
    done = true;
    host.join();
    ::close(psx);
    ::close(announcer);

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("netplay_lan: all checks passed\n");
    return 0;
}
