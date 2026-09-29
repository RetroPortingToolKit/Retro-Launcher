// netplay_nat: the host relay's reachability pieces. Pure encoders and
// parsers against fixtures; then a HostPort over loopback with a fake STUN
// server, answering a probe. The real router is never asked (try_router off).
// Run as `retcomm-netplay-nat-test`. POSIX only.

#include "retcomm/netplay_nat.hpp"

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
} // namespace

int main() {
    // ---- SSDP / IGD / SOAP
    check(ssdp_location("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nLocation: "
                        "http://192.168.1.1:5000/rootDesc.xml\r\nST: x\r\n\r\n") ==
              "http://192.168.1.1:5000/rootDesc.xml",
          "SSDP LOCATION, any case");
    check(ssdp_location("HTTP/1.1 200 OK\r\n\r\n").empty(), "no LOCATION: empty");
    const std::string desc =
        "<?xml version=\"1.0\"?><root><device><serviceList>"
        "<service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
        "<controlURL>/ctl/L3F</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
        "<controlURL>/ctl/IPConn</controlURL></service>"
        "</serviceList></device></root>";
    std::string ctl, svc;
    check(igd_control_url(desc, "http://192.168.1.1:5000/rootDesc.xml", &ctl, &svc) &&
              ctl == "http://192.168.1.1:5000/ctl/IPConn" &&
              svc == "urn:schemas-upnp-org:service:WANIPConnection:1",
          "WANIPConnection control URL, absolute against the location");
    check(igd_control_url("<root><URLBase>http://10.0.0.1:49152/</URLBase><service><serviceType>"
                          "urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>"
                          "<controlURL>upnp/control/WANPPPConn1</controlURL></service></root>",
                          "http://10.0.0.1:49152/desc.xml", &ctl, &svc) &&
              ctl == "http://10.0.0.1:49152/upnp/control/WANPPPConn1",
          "WANPPPConnection, relative to URLBase");
    check(!igd_control_url("<root><service><serviceType>x</serviceType></service></root>", "http://a/",
                           &ctl, &svc),
          "no WAN connection service: false");
    const std::string body = soap_body("urn:schemas-upnp-org:service:WANIPConnection:1",
                                       "AddPortMapping", {{"NewExternalPort", "7777"}});
    check(body.find("<u:AddPortMapping xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"
                    "<NewExternalPort>7777</NewExternalPort></u:AddPortMapping>") != std::string::npos,
          "the SOAP envelope");
    check(xml_text("<s:Body><u:GetExternalIPAddressResponse><NewExternalIPAddress>203.0.113.9"
                   "</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body>",
                   "NewExternalIPAddress") == "203.0.113.9",
          "a SOAP answer's value");

    // ---- NAT-PMP
    const auto req = natpmp_mapping_request(7777, 7777, 7200);
    check(req.size() == 12 && req[1] == 1 && req[4] == 0x1E && req[5] == 0x61 &&
              req[10] == 0x1C && req[11] == 0x20,
          "the UDP mapping request");
    std::uint16_t ext = 0, rc = 0;
    check(natpmp_parse_mapping({0, 129, 0, 0, 0, 0, 0, 1, 0x1E, 0x61, 0x1E, 0x62, 0, 0, 0x1C, 0x20},
                               &ext, &rc) && ext == 7778,
          "a mapping answer's external port");
    check(!natpmp_parse_mapping({0, 129, 0, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, &ext, &rc) && rc == 3,
          "a refused mapping");
    std::string ip;
    check(natpmp_parse_address({0, 128, 0, 0, 0, 0, 0, 1, 203, 0, 113, 7}, &ip) && ip == "203.0.113.7",
          "the external address answer");
    check(default_gateway_from_route(
              "Iface\tDestination\tGateway \tFlags\n"
              "enp5s0\t0001A8C0\t00000000\t0001\n"
              "enp5s0\t00000000\t0101A8C0\t0003\n") == "192.168.1.1",
          "the default gateway from /proc/net/route");

    // ---- STUN
    std::uint8_t tx[12];
    for (int i = 0; i < 12; ++i) tx[i] = static_cast<std::uint8_t>(i * 7 + 1);
    const auto sreq = stun_binding_request(tx);
    check(sreq.size() == 20 && sreq[0] == 0 && sreq[1] == 1 && sreq[4] == 0x21, "a binding request");
    auto stun_answer = [&](const char* dotted, std::uint16_t port) {
        std::vector<std::uint8_t> r = {0x01, 0x01, 0x00, 0x0C, 0x21, 0x12, 0xA4, 0x42};
        r.insert(r.end(), tx, tx + 12);
        in_addr a{};
        inet_pton(AF_INET, dotted, &a);
        const auto* b = reinterpret_cast<const std::uint8_t*>(&a.s_addr);
        const std::uint16_t xp = port ^ 0x2112;
        r.insert(r.end(), {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, static_cast<std::uint8_t>(xp >> 8),
                           static_cast<std::uint8_t>(xp), static_cast<std::uint8_t>(b[0] ^ 0x21),
                           static_cast<std::uint8_t>(b[1] ^ 0x12), static_cast<std::uint8_t>(b[2] ^ 0xA4),
                           static_cast<std::uint8_t>(b[3] ^ 0x42)});
        return r;
    };
    std::uint16_t mp = 0;
    check(stun_parse_mapped(stun_answer("198.51.100.20", 40123), tx, &ip, &mp) &&
              ip == "198.51.100.20" && mp == 40123,
          "XOR-MAPPED-ADDRESS decoded");
    std::uint8_t other[12] = {};
    check(!stun_parse_mapped(stun_answer("198.51.100.20", 40123), other, &ip, &mp),
          "another transaction's answer is refused");

    // ---- a HostPort on loopback: a fake STUN server, then a probe
    const int stun = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    bind(stun, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    socklen_t sl = sizeof sa;
    getsockname(stun, reinterpret_cast<sockaddr*>(&sa), &sl);
    const std::uint16_t stun_port = ntohs(sa.sin_port);
    std::atomic<bool> done{false};
    std::thread fake([&] {
        while (!done) {
            pollfd p{stun, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            std::uint8_t buf[512];
            sockaddr_in from{};
            socklen_t fl = sizeof from;
            const auto n = recvfrom(stun, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n < 20) continue;
            std::copy(buf + 8, buf + 20, tx); // answer this transaction
            auto r = stun_answer("198.51.100.77", ntohs(from.sin_port));
            sendto(stun, r.data(), r.size(), 0, reinterpret_cast<sockaddr*>(&from), fl);
        }
    });
    HostPort hp;
    std::string err;
    check(hp.start(7799, "127.0.0.1:" + std::to_string(stun_port), &err, /*try_router=*/false),
          ("HostPort binds its port: " + err).c_str());
    for (int i = 0; i < 60 && !hp.status().done; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const HostPort::Status st = hp.status();
    check(st.done && st.how == "stun" && st.endpoint == "198.51.100.77:7799",
          "STUN names this socket's public mapping");
    check(probe_host("127.0.0.1:7799", 3), "a probe of the host's port is answered");
    check(hp.status().probes_answered >= 1, "and counted");
    check(!probe_host("127.0.0.1:7798", 2), "a probe of nothing is not");
    hp.stop();
    check(!probe_host("127.0.0.1:7799", 2), "after stop() the port is free and silent");
    done = true;
    fake.join();
    close(stun);

    // A guest probes as soon as it joins, while the host is still asking:
    // the probe is answered then, not after STUN gives up (a silent server
    // takes 1.5 s; the probe window is 3 x 400 ms).
    const int silent = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in qa{};
    qa.sin_family = AF_INET;
    qa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(silent, reinterpret_cast<sockaddr*>(&qa), sizeof qa);
    socklen_t ql = sizeof qa;
    getsockname(silent, reinterpret_cast<sockaddr*>(&qa), &ql);
    HostPort early;
    check(early.start(7799, "127.0.0.1:" + std::to_string(ntohs(qa.sin_port)), &err, false),
          ("HostPort binds its port again: " + err).c_str());
    check(!early.status().done, "STUN is still being asked");
    check(probe_host("127.0.0.1:7799", 3), "a probe during discovery is answered at once");
    check(!early.status().done, "before discovery finished");
    early.stop();
    close(silent);

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("netplay_nat: all checks passed\n");
    return 0;
}
