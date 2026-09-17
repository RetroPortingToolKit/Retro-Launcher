#pragma once

// Minimal RFC 6455 client on libcurl's WebSocket API (curl_ws_send /
// curl_ws_recv, libcurl >= 7.86). Text frames only, one connection per object,
// no reconnect policy of its own. Blocking calls take an explicit timeout so a
// worker thread can interleave sends, receives and a stop flag.
//
// Why libcurl and not a dedicated library: it is already linked, it gives
// wss:// (TLS) for free on every platform we ship, and the lobby protocol is
// small text frames where a full-featured client buys nothing.

#include <string>

typedef void CURL;

namespace retcomm::netplay {

class WsConnection {
public:
    WsConnection() = default;
    ~WsConnection();
    WsConnection(const WsConnection&) = delete;
    WsConnection& operator=(const WsConnection&) = delete;

    // True when this libcurl was built with the ws/wss protocols.
    static bool supported();

    // Performs the HTTP upgrade. On failure `error` names the cause
    // (transport error, or the HTTP status when the server refused).
    bool connect(const std::string& url, std::string* error, long connect_timeout_s = 15);

    // Sends one text frame. Blocks up to `timeout_ms` for socket writability.
    bool send_text(const std::string& text, std::string* error, int timeout_ms = 5000);

    // Waits up to `timeout_ms` for one complete text message.
    //   1  message stored in `out`
    //   0  timed out, nothing received
    //  -1  connection closed or failed (`error` set)
    // Control frames are consumed internally; libcurl answers PING itself.
    int recv_text(std::string& out, int timeout_ms, std::string* error);

    void close();
    bool connected() const { return curl_ != nullptr; }

private:
    int wait_socket(bool writable, int timeout_ms);

    CURL* curl_ = nullptr;
    long long sock_ = -1;  // curl_socket_t, kept opaque here
    std::string partial_;  // fragments of the message being assembled
};

}  // namespace retcomm::netplay
