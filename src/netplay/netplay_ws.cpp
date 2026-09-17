#include "retcomm/netplay_ws.hpp"

#include "retcomm/http.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <poll.h>
#endif

// curl_ws_send / curl_ws_recv arrived in 7.86.0 (0x075600).
#if LIBCURL_VERSION_NUM >= 0x075600
#define RETCOMM_HAVE_CURL_WS 1
#else
#define RETCOMM_HAVE_CURL_WS 0
#endif

namespace retcomm::netplay {

namespace {

long long steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

WsConnection::~WsConnection() { close(); }

bool WsConnection::supported() {
#if !RETCOMM_HAVE_CURL_WS
    return false;
#else
    const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
    if (!v || !v->protocols) return false;
    for (const char* const* p = v->protocols; *p; ++p) {
        if (std::strcmp(*p, "ws") == 0) return true;
    }
    return false;
#endif
}

int WsConnection::wait_socket(bool writable, int timeout_ms) {
    if (sock_ < 0) return -1;
#if defined(_WIN32)
    WSAPOLLFD p{};
    p.fd = static_cast<SOCKET>(sock_);
    p.events = writable ? POLLWRNORM : POLLRDNORM;
    return WSAPoll(&p, 1, timeout_ms);
#else
    pollfd p{};
    p.fd = static_cast<int>(sock_);
    p.events = writable ? POLLOUT : POLLIN;
    return ::poll(&p, 1, timeout_ms);
#endif
}

bool WsConnection::connect(const std::string& url, std::string* error, long connect_timeout_s) {
    close();
#if !RETCOMM_HAVE_CURL_WS
    (void)url;
    (void)connect_timeout_s;
    if (error) *error = "this libcurl is too old for WebSocket (need 7.86+)";
    return false;
#else
    if (!supported()) {
        if (error) *error = "this libcurl was built without WebSocket support";
        return false;
    }
    http_global_init();
    CURL* c = curl_easy_init();
    if (!c) {
        if (error) *error = "curl_easy_init failed";
        return false;
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    // 2 = "connect only, WebSocket mode": perform() does the upgrade and then
    // hands the socket to curl_ws_send / curl_ws_recv.
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, connect_timeout_s);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "retcomm-launcher");
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "ws,wss");
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
    curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
    const CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        if (error) *error = curl_easy_strerror(rc);
        curl_easy_cleanup(c);
        return false;
    }
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (status != 101) {
        if (error) *error = "WebSocket upgrade refused (HTTP " + std::to_string(status) + ")";
        curl_easy_cleanup(c);
        return false;
    }
    curl_socket_t s = CURL_SOCKET_BAD;
    curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &s);
    curl_ = c;
    sock_ = static_cast<long long>(s);
    partial_.clear();
    return true;
#endif
}

bool WsConnection::send_text(const std::string& text, std::string* error, int timeout_ms) {
#if !RETCOMM_HAVE_CURL_WS
    (void)text;
    (void)timeout_ms;
    if (error) *error = "no WebSocket support";
    return false;
#else
    if (!curl_) {
        if (error) *error = "not connected";
        return false;
    }
    const long long deadline = steady_ms() + timeout_ms;
    const char* p = text.data();
    size_t left = text.size();
    bool first = true;
    while (left > 0) {
        size_t sent = 0;
        const unsigned flags = CURLWS_TEXT | (first ? 0u : CURLWS_OFFSET);
        const CURLcode rc = curl_ws_send(curl_, p, left, &sent, 0, flags);
        if (rc == CURLE_OK) {
            first = false;
            p += sent;
            left -= sent;
            if (left == 0) return true;
        } else if (rc != CURLE_AGAIN) {
            if (error) *error = curl_easy_strerror(rc);
            return false;
        }
        const long long remaining = deadline - steady_ms();
        if (remaining <= 0) {
            if (error) *error = "send timed out";
            return false;
        }
        if (wait_socket(true, static_cast<int>(remaining)) < 0) {
            if (error) *error = "socket poll failed";
            return false;
        }
    }
    return true;
#endif
}

int WsConnection::recv_text(std::string& out, int timeout_ms, std::string* error) {
#if !RETCOMM_HAVE_CURL_WS
    (void)out;
    (void)timeout_ms;
    if (error) *error = "no WebSocket support";
    return -1;
#else
    if (!curl_) {
        if (error) *error = "not connected";
        return -1;
    }
    const long long deadline = steady_ms() + timeout_ms;
    char buf[16 * 1024];
    for (;;) {
        size_t n = 0;
        const curl_ws_frame* meta = nullptr;
        const CURLcode rc = curl_ws_recv(curl_, buf, sizeof(buf), &n, &meta);
        if (rc == CURLE_AGAIN) {
            const long long remaining = deadline - steady_ms();
            if (remaining <= 0) return 0;
            // With TLS, bytes can sit decrypted inside the SSL layer where the
            // socket poll cannot see them. The worker calls this again a few
            // hundred ms later, so at worst a message is delayed one tick.
            const int w = wait_socket(false, static_cast<int>(remaining));
            if (w < 0) {
                if (error) *error = "socket poll failed";
                return -1;
            }
            if (w == 0) return 0;
            continue;
        }
        if (rc != CURLE_OK) {
            if (error) *error = curl_easy_strerror(rc);
            return -1;
        }
        if (!meta) continue;
        if (meta->flags & CURLWS_CLOSE) {
            if (error) *error = "connection closed by server";
            return -1;
        }
        if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;  // libcurl pongs
        if (meta->flags & CURLWS_BINARY) {
            partial_.clear();  // the lobby never sends binary; drop it
            continue;
        }
        partial_.append(buf, n);
        const bool more = meta->bytesleft > 0 || (meta->flags & CURLWS_CONT) != 0;
        if (!more) {
            out.swap(partial_);
            partial_.clear();
            return 1;
        }
    }
#endif
}

void WsConnection::close() {
    if (!curl_) return;
#if RETCOMM_HAVE_CURL_WS
    size_t sent = 0;
    curl_ws_send(curl_, "", 0, &sent, 0, CURLWS_CLOSE);  // best effort
#endif
    curl_easy_cleanup(curl_);
    curl_ = nullptr;
    sock_ = -1;
    partial_.clear();
}

}  // namespace retcomm::netplay
