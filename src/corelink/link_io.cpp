#include "link_io.hpp"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/uio.h>

namespace retro::corelink {

bool send_packet(int sock, const void* msg, std::size_t size, const int* fds, std::size_t nfds) {
    iovec iov{const_cast<void*>(msg), size};
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    std::vector<char> ctrl;
    if (nfds) {
        ctrl.assign(CMSG_SPACE(sizeof(int) * nfds), 0);
        mh.msg_control = ctrl.data();
        mh.msg_controllen = ctrl.size();
        cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        std::memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
    }
    for (;;) {
        const ssize_t n = ::sendmsg(sock, &mh, MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(size)) return true;
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
}

RecvResult recv_packet(int sock, std::vector<unsigned char>& buf, std::vector<int>* fds,
                       bool block) {
    buf.resize(kMaxMsgSize);
    iovec iov{buf.data(), buf.size()};
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * kMaxRegions)];
    mh.msg_control = ctrl;
    mh.msg_controllen = sizeof ctrl;
    for (;;) {
        const ssize_t n =
            ::recvmsg(sock, &mh, MSG_CMSG_CLOEXEC | (block ? 0 : MSG_DONTWAIT));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return RecvResult::WouldBlock;
            return RecvResult::Error;
        }
        if (n == 0) return RecvResult::Closed;
        buf.resize(static_cast<std::size_t>(n));
        for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            const std::size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            const int* p = reinterpret_cast<const int*>(CMSG_DATA(c));
            for (std::size_t i = 0; i < count; ++i) {
                if (fds) fds->push_back(p[i]);
            }
        }
        if (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) return RecvResult::Error;
        return RecvResult::Packet;
    }
}

} // namespace retro::corelink
