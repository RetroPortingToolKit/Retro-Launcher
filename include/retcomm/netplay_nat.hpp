#pragma once

// Making a netplay host reachable, for the host relay (Retro-Launcher
// docs/NETPLAY_DIRECT.md; recomp-net-server WS_LOBBY.md "Host relay"). While
// the room waits, the host's hub holds the game's UDP port and:
//
//   1. asks the router to forward it: UPnP IGD (SSDP, the device description,
//      SOAP AddPortMapping / GetExternalIPAddress), else NAT-PMP (RFC 6886);
//   2. otherwise learns its public mapping with STUN (RFC 5389) -- reachable
//      only through a forwarded port or an open NAT, which the guests' probes
//      then prove or disprove;
//   3. answers guests' probes (kProbe -> kProbeAck) on that same port.
//
// The guest side probes the advertised endpoint. The lobby server launches
// the host relay only when every guest's probe succeeded; otherwise its own
// relay carries the match, so a match always connects.
//
// The encoders and parsers are pure and unit-tested; the network calls block
// (the hub runs them on a worker).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace retcomm::netplay {

// ---- the probe, on the game port ------------------------------------------------
inline constexpr const char* kProbe = "RETRO_HOSTPROBE1 ";
inline constexpr const char* kProbeAck = "RETRO_HOSTPROBE1_ACK ";

// Sends `tries` probes to `endpoint` ("ip:port", or "name:port") about 400 ms
// apart and waits for an answer; true when one came.
bool probe_host(const std::string& endpoint, int tries = 6);

// ---- UPnP IGD ----------------------------------------------------------------------
// The LOCATION header of one SSDP response, or empty.
std::string ssdp_location(const std::string& response);
// From a device description: the WANIPConnection (1 or 2) or WANPPPConnection
// control URL, made absolute against `location` (and URLBase when given), and
// its service type. False when the device has neither.
bool igd_control_url(const std::string& description_xml, const std::string& location,
                     std::string* control_url, std::string* service_type);
// The SOAP envelope for `action` with (name, value) arguments.
std::string soap_body(const std::string& service_type, const std::string& action,
                      const std::vector<std::pair<std::string, std::string>>& args);
// The text of <tag> in `xml`, or empty.
std::string xml_text(const std::string& xml, const std::string& tag);

// ---- NAT-PMP -----------------------------------------------------------------------
std::vector<std::uint8_t> natpmp_mapping_request(std::uint16_t internal_port,
                                                 std::uint16_t external_port,
                                                 std::uint32_t lifetime_s);
// A mapping response (op 129): the external port; false on an error result.
bool natpmp_parse_mapping(const std::vector<std::uint8_t>& resp, std::uint16_t* external_port,
                          std::uint16_t* result_code);
// An external-address response (op 128): dotted IPv4.
bool natpmp_parse_address(const std::vector<std::uint8_t>& resp, std::string* ip);
// The default gateway from /proc/net/route text; empty when there is none.
std::string default_gateway_from_route(const std::string& proc_net_route);

// ---- STUN --------------------------------------------------------------------------
std::vector<std::uint8_t> stun_binding_request(const std::uint8_t txid[12]);
// The XOR-MAPPED-ADDRESS (else MAPPED-ADDRESS) of a binding success response.
bool stun_parse_mapped(const std::vector<std::uint8_t>& resp, const std::uint8_t txid[12],
                       std::string* ip, std::uint16_t* port);

// ---- the host side -----------------------------------------------------------------
// Holds the game port while the room waits, makes it reachable, and answers
// probes. stop() before the runner binds the port (it must be free).
class HostPort {
public:
    HostPort() = default;
    ~HostPort();
    HostPort(const HostPort&) = delete;
    HostPort& operator=(const HostPort&) = delete;

    // Binds UDP `port` (0 = the first free of 7777..7808) and starts working
    // it out on a worker. `stun_server` "host:port". `try_router` false skips
    // UPnP and NAT-PMP (tests: nothing is asked of the real router).
    bool start(std::uint16_t port, const std::string& stun_server, std::string* error,
               bool try_router = true);
    // Releases the port (the probe responder stops; a UPnP / NAT-PMP mapping
    // stays until unmap(), so the runner inherits it).
    void stop();
    // Removes a mapping this made (on leaving the room or quitting).
    void unmap();

    struct Status {
        bool done = false;       // the work is finished (a result or none)
        std::string endpoint;    // public "ip:port" to advertise; empty = none
        std::string how;         // "upnp" | "nat-pmp" | "stun" | ""
        std::string detail;      // one sentence for the room
        std::uint16_t local_port = 0;
        std::uint32_t probes_answered = 0;
    };
    Status status() const;

private:
    void run(std::string stun_server, bool try_router);

    mutable std::mutex mu_;
    Status st_;
    int sock_ = -1;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    // What unmap() removes.
    std::string upnp_control_, upnp_service_;
    std::uint16_t mapped_port_ = 0;
    bool natpmp_mapped_ = false;
    std::string gateway_;
};

} // namespace retcomm::netplay
