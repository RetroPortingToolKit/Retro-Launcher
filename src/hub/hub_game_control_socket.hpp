#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace retcomm::hub {

// One local QA client. All socket operations are nonblocking so rendering and
// input continue while a client is receiving a large capture.
class GameControlSocket {
public:
    GameControlSocket() = default;
    GameControlSocket(const GameControlSocket&) = delete;
    GameControlSocket& operator=(const GameControlSocket&) = delete;
    ~GameControlSocket();

    std::optional<std::uint16_t> listen(std::uint16_t port);
    void poll();
    bool enabled() const { return listener_ != no_socket; }
    bool reply_pending() const { return !output_.empty(); }
    bool connected() const { return client_ != no_socket; }
    std::optional<std::string> request();
    void reply(std::string message);

private:
    void disconnect();
    static constexpr auto no_socket = static_cast<std::uintptr_t>(-1);
    std::uintptr_t listener_ = no_socket;
    std::uintptr_t client_ = no_socket;
    bool initialized_ = false;
    std::string input_;
    std::string output_;
    std::size_t sent_ = 0;
};

} // namespace retcomm::hub
