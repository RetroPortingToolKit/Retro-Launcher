#pragma once

// The hub's side of the link to retcomm-core-runner (docs/CORE_LINK.md):
// spawn the runner, own the shared region and the save memory, grant frames,
// take pictures and audio, and report how the runner ended. The hub's
// frontend and the link test tool both drive a session through this class.

#include "link_protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace retcomm::corelink {

namespace fs = std::filesystem;

struct LaunchSpec {
    fs::path runner;       // the retcomm-core-runner binary
    fs::path core;         // <title>_core.so, its .rcore.toml beside it
    std::string rom;
    fs::path title_dir;
    fs::path session_dir;  // runner.log, core.log, events.tsv, the core's cache
    bool gl = true;
    bool strict = false;
    std::map<std::string, std::string> options;
    std::optional<fs::path> load_state;
    std::string tpak_rom;
    std::map<std::string, fs::path> save_files; // region id -> file
    // Regions not named above land here as <id>.sav. Empty = only the named
    // ones persist. (Region ids are only known once the core has loaded.)
    fs::path save_dir;
    // The seats before the first frame (connected flags matter: a core may
    // read input while it loads a state). Frame pads come with each grant.
    rcore_pad initial_pads[RCORE_MAX_SEATS]{};
    std::vector<std::string> env;               // extra NAME=value for the runner
};

struct CoreIdentity {
    std::string sha256, core_id, core_version, platforms;
    std::uint64_t capabilities = 0;
    std::uint32_t abi_major = 0, draft_revision = 0;
    bool engine_dirty = false;
};

struct LinkLog {
    std::uint32_t level;
    std::string text;
};

struct LinkEvent {
    std::uint32_t kind;
    std::uint64_t frame_number, guest_address;
    std::string detail;
};

enum class LinkState {
    Idle,     // not started
    Starting, // spawned; identity, load and saves in progress
    Ready,    // frames may be granted
    Ended,    // the runner exited (see exit_code / exit_reason)
};

class CoreLink {
public:
    CoreLink() = default;
    CoreLink(const CoreLink&) = delete;
    CoreLink& operator=(const CoreLink&) = delete;
    ~CoreLink();

    bool start(const LaunchSpec& spec, std::string* error);

    // Handles every message waiting (up to timeout_ms for the first).
    void pump(int timeout_ms = 0);

    LinkState state() const { return state_; }
    const CoreIdentity& identity() const { return identity_; }

    // One frame, with every seat's pad. Only one grant is outstanding at a
    // time; can_grant() says whether the previous one has finished.
    bool can_grant() const { return state_ == LinkState::Ready && outstanding_ == 0; }
    bool grant(const rcore_pad pads[RCORE_MAX_SEATS]);
    std::uint64_t frames_granted() const { return granted_; }
    std::uint64_t frames_done() const { return done_; }

    // Swaps in the newest picture if there is one the hub has not taken.
    bool take_frame();
    // The picture last taken (nullptr before the first one).
    const FrameInfo* frame_info() const;
    const std::uint8_t* frame_pixels() const;

    std::uint32_t audio_rate() const;
    // Up to max_frames stereo frames into out (2 * max_frames samples).
    std::size_t drain_audio(std::int16_t* out, std::size_t max_frames);

    // Everything the runner reported since start; the caller may clear them.
    std::vector<LinkLog> logs;
    std::vector<LinkEvent> events;

    // Once Ended: the process exit code (or -signal), and the runner's own
    // reason when it gave one.
    int exit_code() const { return exit_code_; }
    const std::string& exit_reason() const { return exit_reason_; }
    fs::path runner_log() const { return spec_.session_dir / "runner.log"; }

    // Writes every save region to its file. Safe after the runner died: the
    // hub holds its own mapping of each region.
    void persist_saves();

    // Quit, wait up to grace_ms, then kill. Persists saves either way.
    void stop(int grace_ms = 3000);

private:
    void handle_packet(const std::vector<unsigned char>& buf, std::vector<int>& fds);
    void on_ended(int code);
    void reap(bool block);

    struct Region {
        std::string id;
        std::uint8_t* data = nullptr;
        std::size_t size = 0;
        std::optional<fs::path> file;
    };

    LaunchSpec spec_;
    LinkState state_ = LinkState::Idle;
    CoreIdentity identity_;
    int sock_ = -1;
    int shm_fd_ = -1;
    SharedHeader* shm_ = nullptr;
    int pid_ = -1;
    std::vector<Region> regions_;
    std::uint64_t granted_ = 0, done_ = 0;
    int outstanding_ = 0;
    std::uint32_t front_ = 0; // the triple buffer's initial front slot
    bool have_frame_ = false;
    int exit_code_ = 0;
    std::string exit_reason_;
};

} // namespace retcomm::corelink
