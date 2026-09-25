#include "core_link.hpp"

#include "link_io.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace retcomm::corelink {

CoreLink::~CoreLink() {
    if (state_ != LinkState::Idle) stop();
    for (Region& r : regions_) {
        if (r.data) ::munmap(r.data, r.size ? r.size : 1);
    }
    if (shm_) ::munmap(shm_, shared_total_size());
    if (shm_fd_ >= 0) ::close(shm_fd_);
    if (sock_ >= 0) ::close(sock_);
}

bool CoreLink::start(const LaunchSpec& spec, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    if (state_ != LinkState::Idle) return fail("this link was already started");
    spec_ = spec;
    std::error_code ec;
    fs::create_directories(spec_.session_dir, ec);

    // ---- the shared region: the hub's, so it outlives a crashed runner ------
    shm_fd_ = ::memfd_create("retcomm-core-link", MFD_CLOEXEC);
    if (shm_fd_ < 0 || ::ftruncate(shm_fd_, static_cast<off_t>(shared_total_size())) != 0) {
        return fail(std::string("shared region: ") + std::strerror(errno));
    }
    void* map = ::mmap(nullptr, shared_total_size(), PROT_READ | PROT_WRITE, MAP_SHARED,
                       shm_fd_, 0);
    if (map == MAP_FAILED) return fail(std::string("map shared region: ") + std::strerror(errno));
    shm_ = new (map) SharedHeader{};
    std::memcpy(shm_->magic, kMagic, sizeof kMagic);
    shm_->version = kProtocolVersion;
    shm_->header_size = sizeof(SharedHeader);
    shm_->total_size = shared_total_size();
    shm_->frames_offset = shared_frames_offset();
    shm_->audio_offset = shared_audio_offset();
    shm_->middle.store(1, std::memory_order_relaxed); // front 0, middle 1, back 2
    shm_->audio_capacity = kAudioCapacity;

    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        return fail(std::string("socketpair: ") + std::strerror(errno));
    }
    sock_ = sv[0];

    // ---- argv: the headless flags, plus --link ------------------------------
    std::vector<std::string> args = {spec_.runner.string(), "--link",
                                     "--core", spec_.core.string(),
                                     "--rom", spec_.rom,
                                     "--title-dir", spec_.title_dir.string(),
                                     "--out", spec_.session_dir.string()};
    if (spec_.gl) args.push_back("--gl");
    if (spec_.strict) args.push_back("--strict");
    for (const auto& [k, v] : spec_.options) {
        args.push_back("--opt");
        args.push_back(k + "=" + v);
    }
    if (spec_.load_state) {
        args.push_back("--load-state");
        args.push_back(spec_.load_state->string());
    }
    if (!spec_.tpak_rom.empty()) {
        args.push_back("--tpak1-rom");
        args.push_back(spec_.tpak_rom);
    }
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    const std::string log_path = runner_log().string();
    // The whole environment, built before fork: nothing between fork and exec
    // may allocate (the hub has other threads). spec_.env overrides by name.
    std::vector<std::string> env_strings;
    for (char** e = environ; e && *e; ++e) {
        const std::string kv = *e;
        const std::string name = kv.substr(0, kv.find('='));
        const bool overridden = std::any_of(spec_.env.begin(), spec_.env.end(),
            [&](const std::string& o) { return o.compare(0, name.size() + 1, name + "=") == 0; });
        if (!overridden) env_strings.push_back(kv);
    }
    env_strings.insert(env_strings.end(), spec_.env.begin(), spec_.env.end());
    std::vector<char*> envp;
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) return fail(std::string("fork: ") + std::strerror(errno));
    if (pid == 0) {
        // Child: only async-signal-safe calls from here to exec.
        const int log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            ::dup2(log_fd, 1);
            ::dup2(log_fd, 2);
        }
        // Move both fds clear of 3 and 4 before placing them, so neither
        // dup2 clobbers the other; dup2 also clears CLOEXEC on the result.
        const int s_tmp = ::fcntl(sv[1], F_DUPFD, 10);
        const int m_tmp = ::fcntl(shm_fd_, F_DUPFD, 10);
        ::dup2(s_tmp, kSocketFd);
        ::dup2(m_tmp, kSharedFd);
        ::execve(argv[0], argv.data(), envp.data());
        const char msg[] = "retcomm-core-runner: exec failed\n";
        (void)!::write(2, msg, sizeof msg - 1);
        ::_exit(127);
    }
    pid_ = pid;
    ::close(sv[1]);
    state_ = LinkState::Starting;
    return true;
}

void CoreLink::pump(int timeout_ms) {
    if (state_ == LinkState::Idle || state_ == LinkState::Ended) return;
    std::vector<unsigned char> buf;
    std::vector<int> fds;
    bool first = true;
    for (;;) {
        if (first && timeout_ms > 0) {
            pollfd p{sock_, POLLIN, 0};
            ::poll(&p, 1, timeout_ms);
        }
        first = false;
        fds.clear();
        const RecvResult rr = recv_packet(sock_, buf, &fds, false);
        if (rr == RecvResult::WouldBlock) break;
        if (rr != RecvResult::Packet) {
            // EOF or error: the runner is gone. Collect how it ended.
            reap(true);
            return;
        }
        handle_packet(buf, fds);
        for (int fd : fds) ::close(fd);
        if (state_ == LinkState::Ended) return;
    }
    reap(false);
}

void CoreLink::handle_packet(const std::vector<unsigned char>& buf, std::vector<int>& fds) {
    Msg t{};
    if (!packet_type(buf, t)) return;
    switch (t) {
        case Msg::Hello: {
            HelloMsg m{};
            if (!as_msg(buf, m)) break;
            identity_.sha256 = m.sha256;
            identity_.core_id = m.core_id;
            identity_.core_version = m.core_version;
            identity_.platforms = m.platforms;
            identity_.capabilities = m.capabilities;
            identity_.abi_major = m.abi_major;
            identity_.draft_revision = m.draft_revision;
            identity_.engine_dirty = m.engine_dirty != 0;
            break;
        }
        case Msg::SaveRegions: {
            SaveRegionsMsg m{};
            if (!as_msg(buf, m) || m.count > kMaxRegions || fds.size() != m.count) {
                exit_reason_ = "link: malformed save regions";
                stop(0);
                return;
            }
            // Map each region, fill it -- erase value, then the save file --
            // and keep the mapping: persistence is the hub's from here on.
            for (std::uint32_t i = 0; i < m.count; ++i) {
                const RegionDesc& d = m.region[i];
                const std::size_t len = d.size ? static_cast<std::size_t>(d.size) : 1;
                void* p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
                Region r;
                r.id = d.id;
                r.size = static_cast<std::size_t>(d.size);
                if (p == MAP_FAILED) {
                    exit_reason_ = "link: cannot map save region " + r.id;
                    stop(0);
                    return;
                }
                r.data = static_cast<std::uint8_t*>(p);
                std::memset(r.data, static_cast<int>(d.erase_value & 0xff), r.size);
                if (auto f = spec_.save_files.find(r.id); f != spec_.save_files.end()) {
                    r.file = f->second;
                } else if (!spec_.save_dir.empty()) {
                    std::error_code dir_ec;
                    fs::create_directories(spec_.save_dir, dir_ec);
                    r.file = spec_.save_dir / (r.id + ".sav");
                }
                if (r.file) {
                    std::ifstream in(*r.file, std::ios::binary);
                    if (in) in.read(reinterpret_cast<char*>(r.data), std::streamsize(r.size));
                }
                regions_.push_back(std::move(r));
            }
            SavesFilledMsg ack{};
            ack.h.type = Msg::SavesFilled;
            for (std::uint32_t i = 0; i < RCORE_MAX_SEATS; ++i) {
                ack.pads[i] = spec_.initial_pads[i];
                ack.pads[i].struct_size = sizeof(rcore_pad);
            }
            send_msg(sock_, ack);
            break;
        }
        case Msg::Ready:
            state_ = LinkState::Ready;
            break;
        case Msg::FrameDone: {
            FrameDoneMsg m{};
            if (!as_msg(buf, m)) break;
            if (outstanding_ > 0) --outstanding_;
            done_ = m.frame_number;
            break;
        }
        case Msg::Log: {
            LogMsg m{};
            if (as_msg(buf, m)) logs.push_back({m.level, m.text});
            break;
        }
        case Msg::Event: {
            EventMsg m{};
            if (as_msg(buf, m)) events.push_back({m.kind, m.frame_number, m.guest_address, m.detail});
            break;
        }
        case Msg::Exiting: {
            ExitingMsg m{};
            if (as_msg(buf, m)) exit_reason_ = m.reason;
            break;
        }
        default:
            break;
    }
}

bool CoreLink::grant(const rcore_pad pads[RCORE_MAX_SEATS]) {
    if (!can_grant()) return false;
    GrantMsg g{};
    g.h.type = Msg::Grant;
    g.frame_number = granted_ + 1;
    for (std::uint32_t i = 0; i < RCORE_MAX_SEATS; ++i) g.pads[i] = pads[i];
    if (!send_msg(sock_, g)) return false;
    ++granted_;
    ++outstanding_;
    return true;
}

bool CoreLink::take_frame() {
    if (!shm_) return false;
    if (!(shm_->middle.load(std::memory_order_acquire) & kFresh)) return false;
    front_ = shm_->middle.exchange(front_, std::memory_order_acq_rel) & ~kFresh;
    have_frame_ = true;
    return true;
}

const FrameInfo* CoreLink::frame_info() const {
    return have_frame_ ? &shm_->slot[front_] : nullptr;
}

const std::uint8_t* CoreLink::frame_pixels() const {
    if (!have_frame_) return nullptr;
    return reinterpret_cast<const std::uint8_t*>(shm_) + shm_->frames_offset +
           std::size_t(front_) * kFrameSlotBytes;
}

std::uint32_t CoreLink::audio_rate() const {
    return shm_ ? shm_->audio_rate.load(std::memory_order_acquire) : 0;
}

std::size_t CoreLink::drain_audio(std::int16_t* out, std::size_t max_frames) {
    if (!shm_) return 0;
    const std::uint64_t cap = shm_->audio_capacity;
    const std::uint64_t r = shm_->audio_read.load(std::memory_order_relaxed);
    const std::uint64_t w = shm_->audio_write.load(std::memory_order_acquire);
    const std::uint64_t n = std::min<std::uint64_t>(w - r, max_frames);
    const auto* ring = reinterpret_cast<const std::int16_t*>(
        reinterpret_cast<const std::uint8_t*>(shm_) + shm_->audio_offset);
    for (std::uint64_t i = 0; i < n; ++i) {
        const std::uint64_t pos = (r + i) % cap;
        out[i * 2] = ring[pos * 2];
        out[i * 2 + 1] = ring[pos * 2 + 1];
    }
    shm_->audio_read.store(r + n, std::memory_order_release);
    return static_cast<std::size_t>(n);
}

void CoreLink::persist_saves() {
    for (const Region& r : regions_) {
        if (!r.file || !r.data) continue;
        // Write beside, then rename: a crash mid-write never leaves half a save.
        const fs::path tmp = r.file->string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(r.data), std::streamsize(r.size));
            if (!out) continue;
        }
        std::error_code ec;
        fs::rename(tmp, *r.file, ec);
    }
}

void CoreLink::reap(bool block) {
    if (pid_ < 0) return;
    int status = 0;
    const pid_t got = ::waitpid(pid_, &status, block ? 0 : WNOHANG);
    if (got != pid_) return;
    pid_ = -1;
    on_ended(WIFEXITED(status) ? WEXITSTATUS(status)
                               : WIFSIGNALED(status) ? -WTERMSIG(status) : -1);
}

void CoreLink::on_ended(int code) {
    exit_code_ = code;
    state_ = LinkState::Ended;
    outstanding_ = 0;
    persist_saves(); // the hub's mappings survive the runner
}

void CoreLink::stop(int grace_ms) {
    if (state_ == LinkState::Idle) return;
    if (pid_ >= 0) {
        EmptyMsg q{};
        q.h.type = Msg::Quit;
        send_msg(sock_, q);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(grace_ms);
        while (pid_ >= 0 && std::chrono::steady_clock::now() < deadline) {
            reap(false);
            if (pid_ >= 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (pid_ >= 0) {
            ::kill(pid_, SIGKILL);
            if (exit_reason_.empty()) exit_reason_ = "did not quit in time; killed";
            reap(true);
        }
    }
    if (state_ != LinkState::Ended) on_ended(exit_code_);
}

} // namespace retcomm::corelink
