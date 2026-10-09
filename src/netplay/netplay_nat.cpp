#include "retcomm/netplay_nat.hpp"

#include "retcomm/http.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>

#include <curl/curl.h>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fstream>
#endif

namespace retcomm::netplay {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// scheme://host:port of a URL, and its path.
bool split_url(const std::string& url, std::string* origin, std::string* path) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return false;
    const auto slash = url.find('/', scheme + 3);
    if (origin) *origin = url.substr(0, slash);
    if (path) *path = slash == std::string::npos ? "/" : url.substr(slash);
    return true;
}

std::string url_host(const std::string& url) {
    std::string origin;
    if (!split_url(url, &origin, nullptr)) return {};
    std::string h = origin.substr(origin.find("://") + 3);
    const auto colon = h.find(':');
    return colon == std::string::npos ? h : h.substr(0, colon);
}

std::string absolute_url(const std::string& ref, const std::string& base) {
    if (ref.find("://") != std::string::npos) return ref;
    std::string origin, path;
    if (!split_url(base, &origin, &path)) return ref;
    if (!ref.empty() && ref[0] == '/') return origin + ref;
    const auto last = path.rfind('/');
    return origin + path.substr(0, last + 1) + ref;
}

} // namespace

// ---- UPnP --------------------------------------------------------------------------

std::string ssdp_location(const std::string& response) {
    std::istringstream in(response);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (lower(line.substr(0, colon)) != "location") continue;
        std::string v = line.substr(colon + 1);
        v.erase(0, v.find_first_not_of(" \t"));
        return v;
    }
    return {};
}

std::string xml_text(const std::string& xml, const std::string& tag) {
    // Namespaced or not: <tag> or <x:tag>.
    for (std::size_t at = 0; (at = xml.find(tag + ">", at)) != std::string::npos; ++at) {
        const auto open = xml.rfind('<', at);
        if (open == std::string::npos) continue;
        const std::string head = xml.substr(open + 1, at - open - 1);
        if (!head.empty() && (head[0] == '/' || head.find(' ') != std::string::npos)) continue;
        if (!head.empty() && head.back() != ':') continue; // <foo:tag> or <tag>
        const auto start = at + tag.size() + 1;
        const auto close = xml.find("</", start);
        if (close == std::string::npos) return {};
        return xml.substr(start, close - start);
    }
    // plain <tag>
    const auto s = xml.find("<" + tag + ">");
    if (s == std::string::npos) return {};
    const auto b = s + tag.size() + 2;
    const auto e = xml.find("</" + tag + ">", b);
    return e == std::string::npos ? std::string() : xml.substr(b, e - b);
}

bool igd_control_url(const std::string& xml, const std::string& location,
                     std::string* control_url, std::string* service_type) {
    const std::string base_tag = xml_text(xml, "URLBase");
    const std::string base = base_tag.empty() ? location : base_tag;
    const char* wanted[] = {"urn:schemas-upnp-org:service:WANIPConnection:2",
                            "urn:schemas-upnp-org:service:WANIPConnection:1",
                            "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    for (const char* type : wanted) {
        for (std::size_t at = 0; (at = xml.find("<service>", at)) != std::string::npos; ++at) {
            const auto end = xml.find("</service>", at);
            if (end == std::string::npos) break;
            const std::string svc = xml.substr(at, end - at);
            if (xml_text(svc, "serviceType") != type) continue;
            const std::string ctl = xml_text(svc, "controlURL");
            if (ctl.empty()) continue;
            if (control_url) *control_url = absolute_url(ctl, base);
            if (service_type) *service_type = type;
            return true;
        }
    }
    return false;
}

std::string soap_body(const std::string& service_type, const std::string& action,
                      const std::vector<std::pair<std::string, std::string>>& args) {
    std::string b = "<?xml version=\"1.0\"?>\r\n"
                    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                    "<s:Body><u:" + action + " xmlns:u=\"" + service_type + "\">";
    for (const auto& [k, v] : args) b += "<" + k + ">" + v + "</" + k + ">";
    b += "</u:" + action + "></s:Body></s:Envelope>\r\n";
    return b;
}

// ---- NAT-PMP -----------------------------------------------------------------------

std::vector<std::uint8_t> natpmp_mapping_request(std::uint16_t internal_port,
                                                 std::uint16_t external_port,
                                                 std::uint32_t lifetime_s) {
    return {0, 1, 0, 0,
            static_cast<std::uint8_t>(internal_port >> 8), static_cast<std::uint8_t>(internal_port),
            static_cast<std::uint8_t>(external_port >> 8), static_cast<std::uint8_t>(external_port),
            static_cast<std::uint8_t>(lifetime_s >> 24), static_cast<std::uint8_t>(lifetime_s >> 16),
            static_cast<std::uint8_t>(lifetime_s >> 8), static_cast<std::uint8_t>(lifetime_s)};
}

bool natpmp_parse_mapping(const std::vector<std::uint8_t>& r, std::uint16_t* external_port,
                          std::uint16_t* result_code) {
    if (r.size() < 16 || r[0] != 0 || r[1] != 129) return false;
    const std::uint16_t rc = static_cast<std::uint16_t>(r[2] << 8 | r[3]);
    if (result_code) *result_code = rc;
    if (rc != 0) return false;
    if (external_port) *external_port = static_cast<std::uint16_t>(r[10] << 8 | r[11]);
    return true;
}

bool natpmp_parse_address(const std::vector<std::uint8_t>& r, std::string* ip) {
    if (r.size() < 12 || r[0] != 0 || r[1] != 128 || r[2] != 0 || r[3] != 0) return false;
    if (ip)
        *ip = std::to_string(r[8]) + "." + std::to_string(r[9]) + "." + std::to_string(r[10]) + "." +
              std::to_string(r[11]);
    return true;
}

std::string default_gateway_from_route(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line)) {
        std::istringstream f(line);
        std::string iface, dest, gw;
        if (!(f >> iface >> dest >> gw)) continue;
        if (dest != "00000000" || gw == "00000000") continue;
        const unsigned long v = std::strtoul(gw.c_str(), nullptr, 16); // little-endian hex
        return std::to_string(v & 0xFF) + "." + std::to_string((v >> 8) & 0xFF) + "." +
               std::to_string((v >> 16) & 0xFF) + "." + std::to_string((v >> 24) & 0xFF);
    }
    return {};
}

// ---- STUN --------------------------------------------------------------------------

std::vector<std::uint8_t> stun_binding_request(const std::uint8_t txid[12]) {
    std::vector<std::uint8_t> m = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    m.insert(m.end(), txid, txid + 12);
    return m;
}

bool stun_parse_mapped(const std::vector<std::uint8_t>& r, const std::uint8_t txid[12],
                       std::string* ip, std::uint16_t* port) {
    if (r.size() < 20 || r[0] != 0x01 || r[1] != 0x01) return false; // binding success
    if (r[4] != 0x21 || r[5] != 0x12 || r[6] != 0xA4 || r[7] != 0x42) return false;
    if (!std::equal(txid, txid + 12, r.begin() + 8)) return false;
    const std::size_t len = static_cast<std::size_t>(r[2] << 8 | r[3]);
    std::size_t at = 20;
    bool got = false;
    while (at + 4 <= 20 + len && at + 4 <= r.size()) {
        const std::uint16_t type = static_cast<std::uint16_t>(r[at] << 8 | r[at + 1]);
        const std::uint16_t alen = static_cast<std::uint16_t>(r[at + 2] << 8 | r[at + 3]);
        const std::size_t v = at + 4;
        if (v + alen > r.size()) break;
        if ((type == 0x0020 || type == 0x0001) && alen >= 8 && r[v + 1] == 0x01) {
            std::uint16_t p = static_cast<std::uint16_t>(r[v + 2] << 8 | r[v + 3]);
            std::uint8_t a[4] = {r[v + 4], r[v + 5], r[v + 6], r[v + 7]};
            if (type == 0x0020) { // XOR with the magic cookie
                p ^= 0x2112;
                const std::uint8_t cookie[4] = {0x21, 0x12, 0xA4, 0x42};
                for (int i = 0; i < 4; ++i) a[i] ^= cookie[i];
            }
            if (ip)
                *ip = std::to_string(a[0]) + "." + std::to_string(a[1]) + "." + std::to_string(a[2]) +
                      "." + std::to_string(a[3]);
            if (port) *port = p;
            got = true;
            if (type == 0x0020) return true; // XOR-MAPPED wins
        }
        at = v + ((alen + 3u) & ~3u);
    }
    return got;
}

#if !defined(_WIN32)

namespace {

bool resolve4(const std::string& host, std::uint16_t port, sockaddr_in* out) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        return false;
    *out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);
    return true;
}

bool split_hostport(const std::string& ep, std::string* host, std::uint16_t* port) {
    const auto colon = ep.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    const long p = std::strtol(ep.c_str() + colon + 1, nullptr, 10);
    if (p <= 0 || p > 65535) return false;
    *host = ep.substr(0, colon);
    *port = static_cast<std::uint16_t>(p);
    return true;
}

bool recv_within(int s, int ms, std::vector<std::uint8_t>* out, sockaddr_in* from = nullptr) {
    pollfd p{s, POLLIN, 0};
    if (poll(&p, 1, ms) <= 0) return false;
    std::uint8_t buf[2048];
    sockaddr_in f{};
    socklen_t fl = sizeof f;
    const auto n = recvfrom(s, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&f), &fl);
    if (n <= 0) return false;
    out->assign(buf, buf + n);
    if (from) *from = f;
    return true;
}

// The address this machine uses to reach `ip` (a connected UDP socket's name).
std::string local_ip_toward(const std::string& ip) {
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in d{};
    d.sin_family = AF_INET;
    d.sin_port = htons(9);
    inet_pton(AF_INET, ip.c_str(), &d.sin_addr);
    std::string out;
    if (connect(s, reinterpret_cast<sockaddr*>(&d), sizeof d) == 0) {
        sockaddr_in me{};
        socklen_t ml = sizeof me;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &ml) == 0) {
            char b[64];
            inet_ntop(AF_INET, &me.sin_addr, b, sizeof b);
            out = b;
        }
    }
    close(s);
    return out;
}

size_t curl_sink(char* p, size_t size, size_t n, void* ud) {
    static_cast<std::string*>(ud)->append(p, size * n);
    return size * n;
}

bool http(const std::string& url, const std::string& soap_action, const std::string& body,
          std::string* out, long* status) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    http_apply_tls_trust(c);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 4000L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
#if LIBCURL_VERSION_NUM >= 0x075500
    // CURLOPT_PROTOCOLS_STR since 7.85.0
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP));
#endif
    curl_slist* h = nullptr;
    if (!soap_action.empty()) {
        h = curl_slist_append(h, "Content-Type: text/xml; charset=\"utf-8\"");
        h = curl_slist_append(h, ("SOAPAction: \"" + soap_action + "\"").c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    const CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    if (h) curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return rc == CURLE_OK;
}

std::string upnp_discover() {
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return {};
    sockaddr_in d{};
    d.sin_family = AF_INET;
    d.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &d.sin_addr);
    for (const char* st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                           "urn:schemas-upnp-org:service:WANIPConnection:1"}) {
        const std::string req = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                                            "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") +
                                st + "\r\n\r\n";
        sendto(s, req.data(), req.size(), 0, reinterpret_cast<sockaddr*>(&d), sizeof d);
    }
    std::string loc;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    std::vector<std::uint8_t> r;
    while (loc.empty() && std::chrono::steady_clock::now() < until) {
        if (!recv_within(s, 250, &r)) continue;
        loc = ssdp_location(std::string(r.begin(), r.end()));
    }
    close(s);
    return loc;
}

} // namespace

bool probe_host(const std::string& endpoint, int tries) {
    std::string host;
    std::uint16_t port = 0;
    sockaddr_in to{};
    if (!split_hostport(endpoint, &host, &port) || !resolve4(host, port, &to)) return false;
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    std::mt19937 rng(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string nonce = std::to_string(rng());
    const std::string msg = std::string(kProbe) + nonce;
    bool ok = false;
    for (int i = 0; i < tries && !ok; ++i) {
        sendto(s, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
        std::vector<std::uint8_t> r;
        while (recv_within(s, 400, &r)) {
            if (std::string(r.begin(), r.end()) == std::string(kProbeAck) + nonce) {
                ok = true;
                break;
            }
        }
    }
    close(s);
    return ok;
}

HostPort::~HostPort() {
    stop();
    unmap();
}

bool HostPort::start(std::uint16_t port, const std::string& stun_server, std::string* error,
                     bool try_router) {
    stop();
    int s = -1;
    std::uint16_t bound = 0;
    for (std::uint16_t p = port ? port : 7777; p <= (port ? port : 7808); ++p) {
        s = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(p);
        if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
            bound = p;
            break;
        }
        close(s);
        s = -1;
    }
    if (s < 0) {
        if (error) *error = port ? "UDP port " + std::to_string(port) + " is in use"
                                 : "no free UDP port in 7777-7808";
        return false;
    }
    sock_ = s;
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_ = Status{};
        st_.local_port = bound;
        st_.detail = "Opening UDP port " + std::to_string(bound) + "\xE2\x80\xA6";
    }
    stop_ = false;
    {
        std::lock_guard<std::mutex> lk(inbox_mu_);
        inbox_.clear();
    }
    responder_ = std::thread([this] { respond(); });
    worker_ = std::thread([this, stun_server, try_router] { run(stun_server, try_router); });
    return true;
}

void HostPort::respond() {
    while (!stop_) {
        std::vector<std::uint8_t> r;
        sockaddr_in from{};
        if (!recv_within(sock_, 200, &r, &from)) continue;
        const std::string m(r.begin(), r.end());
        if (m.rfind(kProbe, 0) == 0) {
            const std::string ack = std::string(kProbeAck) + m.substr(std::strlen(kProbe));
            sendto(sock_, ack.data(), ack.size(), 0, reinterpret_cast<sockaddr*>(&from), sizeof from);
            std::lock_guard<std::mutex> lk(mu_);
            ++st_.probes_answered;
            continue;
        }
        std::lock_guard<std::mutex> lk(inbox_mu_);
        if (inbox_.size() < 32) inbox_.push_back(std::move(r));
        inbox_cv_.notify_one();
    }
}

bool HostPort::inbox_pop(int ms, std::vector<std::uint8_t>* out) {
    std::unique_lock<std::mutex> lk(inbox_mu_);
    if (!inbox_cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return !inbox_.empty(); }))
        return false;
    *out = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
}

void HostPort::stop() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    if (responder_.joinable()) responder_.join();
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
}

void HostPort::unmap() {
    if (!upnp_control_.empty() && mapped_port_) {
        std::string out;
        long status = 0;
        http(upnp_control_, upnp_service_ + "#DeletePortMapping",
             soap_body(upnp_service_, "DeletePortMapping",
                       {{"NewRemoteHost", ""},
                        {"NewExternalPort", std::to_string(mapped_port_)},
                        {"NewProtocol", "UDP"}}),
             &out, &status);
    }
    if (natpmp_mapped_ && !gateway_.empty() && mapped_port_) {
        const int s = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in g{};
        if (s >= 0 && resolve4(gateway_, 5351, &g)) {
            const auto req = natpmp_mapping_request(mapped_port_, 0, 0); // lifetime 0 = delete
            sendto(s, req.data(), req.size(), 0, reinterpret_cast<sockaddr*>(&g), sizeof g);
        }
        if (s >= 0) close(s);
    }
    upnp_control_.clear();
    natpmp_mapped_ = false;
    mapped_port_ = 0;
}

HostPort::Status HostPort::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_;
}

void HostPort::run(std::string stun_server, bool try_router) {
    std::uint16_t port;
    {
        std::lock_guard<std::mutex> lk(mu_);
        port = st_.local_port;
    }
    auto finish = [&](const std::string& ep, const char* how, const std::string& detail) {
        std::lock_guard<std::mutex> lk(mu_);
        st_.endpoint = ep;
        st_.how = how;
        st_.detail = detail;
        st_.done = true;
    };
    bool resolved = false;

    // 1. UPnP IGD.
    if (try_router && !stop_) {
        const std::string loc = upnp_discover();
        std::string desc;
        long status = 0;
        std::string ctl, svc;
        if (!loc.empty() && http(loc, "", "", &desc, &status) && status == 200 &&
            igd_control_url(desc, loc, &ctl, &svc)) {
            const std::string me = local_ip_toward(url_host(ctl));
            std::string out;
            status = 0;
            const bool added =
                !me.empty() &&
                http(ctl, svc + "#AddPortMapping",
                     soap_body(svc, "AddPortMapping",
                               {{"NewRemoteHost", ""},
                                {"NewExternalPort", std::to_string(port)},
                                {"NewProtocol", "UDP"},
                                {"NewInternalPort", std::to_string(port)},
                                {"NewInternalClient", me},
                                {"NewEnabled", "1"},
                                {"NewPortMappingDescription", "Retro netplay host"},
                                {"NewLeaseDuration", "7200"}}),
                     &out, &status) &&
                status == 200;
            std::string ext_ip;
            if (added) {
                out.clear();
                status = 0;
                if (http(ctl, svc + "#GetExternalIPAddress",
                         soap_body(svc, "GetExternalIPAddress", {}), &out, &status) &&
                    status == 200)
                    ext_ip = xml_text(out, "NewExternalIPAddress");
            }
            if (added) {
                upnp_control_ = ctl;
                upnp_service_ = svc;
                mapped_port_ = port;
            }
            if (added && !ext_ip.empty()) {
                finish(ext_ip + ":" + std::to_string(port), "upnp",
                       "Your router forwards UDP port " + std::to_string(port) + " (UPnP).");
                resolved = true;
            }
        }
    }

    // 2. NAT-PMP.
    if (try_router && !resolved && !stop_) {
        std::ifstream rt("/proc/net/route");
        const std::string gw =
            default_gateway_from_route(std::string(std::istreambuf_iterator<char>(rt), {}));
        const int s = gw.empty() ? -1 : socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in g{};
        if (s >= 0 && resolve4(gw, 5351, &g)) {
            const std::vector<std::uint8_t> addr_req = {0, 0};
            sendto(s, addr_req.data(), addr_req.size(), 0, reinterpret_cast<sockaddr*>(&g), sizeof g);
            std::vector<std::uint8_t> r;
            std::string ext_ip;
            if (recv_within(s, 800, &r)) natpmp_parse_address(r, &ext_ip);
            if (!ext_ip.empty()) {
                const auto req = natpmp_mapping_request(port, port, 7200);
                sendto(s, req.data(), req.size(), 0, reinterpret_cast<sockaddr*>(&g), sizeof g);
                std::uint16_t ext = 0, rc = 0;
                if (recv_within(s, 800, &r) && natpmp_parse_mapping(r, &ext, &rc) && ext) {
                    gateway_ = gw;
                    natpmp_mapped_ = true;
                    mapped_port_ = port;
                    finish(ext_ip + ":" + std::to_string(ext), "nat-pmp",
                           "Your router forwards UDP port " + std::to_string(ext) + " (NAT-PMP).");
                    resolved = true;
                }
            }
        }
        if (s >= 0) close(s);
    }

    // 3. STUN: the public mapping of this very socket, unforwarded.
    if (!resolved && !stop_) {
        std::string host;
        std::uint16_t sport = 0;
        sockaddr_in to{};
        if (split_hostport(stun_server, &host, &sport) && resolve4(host, sport, &to)) {
            std::uint8_t tx[12];
            std::mt19937 rng(static_cast<unsigned>(port) * 2654435761u);
            for (auto& b : tx) b = static_cast<std::uint8_t>(rng());
            const auto req = stun_binding_request(tx);
            std::string ip;
            std::uint16_t mp = 0;
            for (int i = 0; i < 3 && ip.empty() && !stop_; ++i) {
                sendto(sock_, req.data(), req.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
                std::vector<std::uint8_t> r;
                while (inbox_pop(500, &r)) {
                    if (stun_parse_mapped(r, tx, &ip, &mp)) break;
                }
            }
            if (!ip.empty()) {
                finish(ip + ":" + std::to_string(mp), "stun",
                       "No UPnP or NAT-PMP router answered. Players reach you only if UDP port " +
                           std::to_string(port) + " is forwarded to this machine; otherwise the "
                           "lobby server relays the match.");
                resolved = true;
            }
        }
    }
    if (!resolved)
        finish("", "", "Could not find a public address for this machine: the lobby server "
                       "will relay the match.");
    // Probes go on being answered by respond() until stop().
}

#else // _WIN32: the host relay's network half is not built for Windows yet

bool probe_host(const std::string&, int) { return false; }
HostPort::~HostPort() = default;
bool HostPort::start(std::uint16_t, const std::string&, std::string* error, bool) {
    if (error) *error = "hosting the relay is not built for Windows yet";
    return false;
}
void HostPort::stop() {}
void HostPort::unmap() {}
HostPort::Status HostPort::status() const { return {}; }
void HostPort::run(std::string, bool) {}

#endif

} // namespace retcomm::netplay
