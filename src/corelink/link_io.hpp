#pragma once

// One packet at a time over the link's SOCK_SEQPACKET socket, with optional
// file descriptors (SCM_RIGHTS). Linux/POSIX; the Windows link is not built.

#include "link_protocol.hpp"

#include <cstddef>
#include <cstring>
#include <vector>

namespace retro::corelink {

// True when the whole packet (and any fds) went out.
bool send_packet(int sock, const void* msg, std::size_t size, const int* fds = nullptr,
                 std::size_t nfds = 0);

template <typename M>
bool send_msg(int sock, M& m, const int* fds = nullptr, std::size_t nfds = 0) {
    m.h.size = sizeof(M);
    return send_packet(sock, &m, sizeof(M), fds, nfds);
}

enum class RecvResult { Packet, WouldBlock, Closed, Error };

// Receives one packet into `buf` (resized to fit kMaxMsgSize). Received fds
// are appended to *fds (the caller owns and closes them).
RecvResult recv_packet(int sock, std::vector<unsigned char>& buf, std::vector<int>* fds,
                       bool block);

// The packet's type, once its size is at least a header.
inline bool packet_type(const std::vector<unsigned char>& buf, Msg& out) {
    if (buf.size() < sizeof(MsgHeader)) return false;
    out = reinterpret_cast<const MsgHeader*>(buf.data())->type;
    return true;
}

// Copies a packet into a message struct when the size matches exactly.
template <typename M>
bool as_msg(const std::vector<unsigned char>& buf, M& out) {
    if (buf.size() != sizeof(M)) return false;
    std::memcpy(&out, buf.data(), sizeof(M));
    return true;
}

} // namespace retro::corelink
