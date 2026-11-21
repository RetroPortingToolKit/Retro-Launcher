#include "hub_game_control_socket.hpp"

#include <algorithm>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace retcomm::hub {
namespace {

constexpr int kRequestBytes = 256;

#if defined(_WIN32)
using Socket = SOCKET;
using SocketLength = int;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
constexpr int kSendFlags = 0;
void close_socket(Socket socket) { closesocket(socket); }
bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool nonblocking(Socket socket) {
    u_long enable = 1;
    return ioctlsocket(socket, FIONBIO, &enable) == 0;
}
#else
using Socket = int;
using SocketLength = socklen_t;
constexpr Socket kInvalidSocket = -1;
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
void close_socket(Socket socket) { close(socket); }
bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
bool nonblocking(Socket socket) {
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

} // namespace

GameControlSocket::~GameControlSocket() {
    disconnect();
    if (listener_ != no_socket) close_socket(static_cast<Socket>(listener_));
#if defined(_WIN32)
    if (initialized_) WSACleanup();
#endif
}

std::optional<std::uint16_t> GameControlSocket::listen(std::uint16_t port) {
    if (listener_ != no_socket) return std::nullopt;
#if defined(_WIN32)
    if (!initialized_) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return std::nullopt;
        initialized_ = true;
    }
#endif
    const Socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalidSocket) return std::nullopt;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    SocketLength size = sizeof(address);
    if (!nonblocking(socket) || ::bind(socket, reinterpret_cast<sockaddr*>(&address), size) != 0 ||
        ::listen(socket, 1) != 0 || getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        close_socket(socket);
        return std::nullopt;
    }
    listener_ = static_cast<std::uintptr_t>(socket);
    return ntohs(address.sin_port);
}

void GameControlSocket::disconnect() {
    if (connected()) close_socket(static_cast<Socket>(client_));
    client_ = no_socket;
    input_.clear();
    output_.clear();
    sent_ = 0;
}

void GameControlSocket::poll() {
    if (listener_ == no_socket) return;
    if (!connected()) {
        const Socket socket = accept(static_cast<Socket>(listener_), nullptr, nullptr);
        if (socket == kInvalidSocket) return;
#ifdef SO_NOSIGPIPE
        const int no_sigpipe = 1;
        if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0) {
            close_socket(socket);
            return;
        }
#endif
        if (!nonblocking(socket)) {
            close_socket(socket);
            return;
        }
        client_ = static_cast<std::uintptr_t>(socket);
    }
    const auto socket = static_cast<Socket>(client_);
    if (!output_.empty()) {
        const auto count = static_cast<int>(std::min<std::size_t>(output_.size() - sent_, 256 * 1024));
        const auto wrote = send(socket, output_.data() + sent_, count, kSendFlags);
        if (wrote <= 0) {
            if (wrote == 0 || !would_block()) disconnect();
            return;
        }
        sent_ += static_cast<std::size_t>(wrote);
        if (sent_ == output_.size()) {
            output_.clear();
            sent_ = 0;
        }
    }
    char bytes[kRequestBytes];
    const auto read = recv(socket, bytes, sizeof(bytes), 0);
    if (read <= 0) {
        if (read == 0 || !would_block()) disconnect();
        return;
    }
    input_.append(bytes, static_cast<std::size_t>(read));
    if (input_.size() > kRequestBytes) disconnect();
}

std::optional<std::string> GameControlSocket::request() {
    if (!output_.empty()) return std::nullopt;
    const auto end = input_.find('\n');
    if (end == std::string::npos) return std::nullopt;
    auto line = input_.substr(0, end);
    input_.erase(0, end + 1);
    return line;
}

void GameControlSocket::reply(std::string message) {
    output_ = std::move(message);
    sent_ = 0;
}

} // namespace retcomm::hub
