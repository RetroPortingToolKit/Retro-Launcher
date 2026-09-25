#pragma once

// The hub <-> retcomm-core-runner link (docs/CORE_LINK.md). Shared by both
// sides; both are built from this tree at the same commit, so the protocol
// version is a guard against a stale runner binary, not an evolution scheme.
//
//   control  SOCK_SEQPACKET socketpair: one message per packet, fds passed
//            with SCM_RIGHTS, EOF when the runner dies.
//   bulk     one memfd the HUB creates: 3 frame slots and an audio ring.
//   saves    one memfd per region, created by the RUNNER after load() (the
//            sizes are only known then), sent to the hub, which maps, fills
//            and persists them -- so a runner crash cannot lose a save.
//
// Input rides inside each GRANT: every seat's pad for exactly that frame, so
// "identical within a frame" (the contract's replay rule) holds by
// construction and there is no shared input block to race.

#include "rcore/rcore.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace retcomm::corelink {

constexpr std::uint32_t kProtocolVersion = 2;
constexpr char kMagic[8] = {'R', 'C', 'L', 'I', 'N', 'K', '1', '\0'};

// Frame slots big enough for any console this contract hosts at 1x: the
// largest is 640x480 today. A frame that does not fit is a runner FAULT, not
// a scaled copy.
constexpr std::uint32_t kFrameSlots = 3;
constexpr std::uint32_t kMaxFrameWidth = 1024;
constexpr std::uint32_t kMaxFrameHeight = 1024;
constexpr std::size_t kFrameSlotBytes = std::size_t(kMaxFrameWidth) * kMaxFrameHeight * 4;
constexpr std::uint32_t kFresh = 0x80000000u; // `middle` holds an unread frame

// Stereo S16 frames. ~1.4 s at 48 kHz: generous, because a paused hub
// stops draining and the runner must not block on audio.
constexpr std::uint32_t kAudioCapacity = 1u << 16;

// The fixed numbers the runner finds its inherited descriptors at.
constexpr int kSocketFd = 3;
constexpr int kSharedFd = 4;

static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "shared atomics must be lock-free");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "shared atomics must be lock-free");

struct FrameInfo {
    std::uint32_t width, height, stride, pixel_format;
    std::uint32_t aspect_num, aspect_den;
    std::uint64_t frame_number;
};

// At offset 0 of the shared region. Frame pixels start at frames_offset,
// kFrameSlotBytes apart, always tightly packed (stride == width * 4).
struct SharedHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint64_t total_size;
    std::uint64_t frames_offset;
    std::uint64_t audio_offset;

    // Frames: a lock-free triple buffer. The runner owns a BACK slot, the hub
    // owns a FRONT slot, and `middle` holds the third. The runner fills back
    // (pixels and slot[back]), then swaps it into middle with kFresh set; the
    // hub, seeing kFresh, swaps its front for middle. Each index is only ever
    // touched by its owner, so neither side can write what the other reads,
    // and neither ever waits. Initially front = 0, middle = 1, back = 2.
    std::atomic<std::uint32_t> middle;
    FrameInfo slot[kFrameSlots];

    // Audio: single producer (runner), single consumer (hub). Indices count
    // stereo frames and only grow; position = index % capacity.
    std::atomic<std::uint64_t> audio_write;
    std::atomic<std::uint64_t> audio_read;
    std::atomic<std::uint32_t> audio_rate;
    std::uint32_t audio_capacity;
    std::atomic<std::uint64_t> audio_dropped; // frames the runner could not fit
};

constexpr std::size_t shared_frames_offset() {
    return (sizeof(SharedHeader) + 4095) & ~std::size_t(4095);
}
constexpr std::size_t shared_audio_offset() {
    return shared_frames_offset() + kFrameSlots * kFrameSlotBytes;
}
constexpr std::size_t shared_total_size() {
    return shared_audio_offset() + std::size_t(kAudioCapacity) * 2 * sizeof(std::int16_t);
}

// ---- control messages -------------------------------------------------------

enum class Msg : std::uint32_t {
    // runner -> hub
    Hello = 1,       // identity, after the manifest check
    SaveRegions = 2, // after load(); carries one memfd per region
    Ready = 3,       // saves adopted, state (if any) loaded: grant frames now
    FrameDone = 4,   // the granted frame ran; its picture (if any) is `latest`
    Log = 5,         // a log() line at WARN or above (all lines go to core.log)
    Event = 6,       // a report(): BRIDGE / DISPATCH_MISS / FAULT
    Exiting = 7,     // the runner is stopping on purpose; the reason follows
    // hub -> runner
    SavesFilled = 64, // every region filled from its file; the seats at power-on
    Grant = 65,       // run exactly one frame, with these pads
    Quit = 66,        // unload, deinit, exit 0
};

struct MsgHeader {
    Msg type;
    std::uint32_t size; // whole packet, header included
};

struct HelloMsg {
    MsgHeader h;
    std::uint32_t protocol_version;
    std::uint32_t abi_major;
    std::uint32_t draft_revision;
    std::uint32_t engine_dirty;
    std::uint64_t capabilities;
    char sha256[65];
    char core_id[63];
    char core_version[64];
    char platforms[64];
};

constexpr std::uint32_t kMaxRegions = 16;
struct RegionDesc {
    char id[48];
    std::uint32_t kind, seat, slot, erase_value;
    std::uint64_t size;
};
struct SaveRegionsMsg {
    MsgHeader h;
    std::uint32_t count; // fds arrive in the same order, via SCM_RIGHTS
    std::uint32_t _pad;
    RegionDesc region[kMaxRegions];
};

struct FrameDoneMsg {
    MsgHeader h;
    std::uint64_t frame_number;
    std::int32_t result; // run_frame's rcore_result
    std::uint32_t _pad;
};

struct LogMsg {
    MsgHeader h;
    std::uint32_t level;
    char text[1024]; // truncated; core.log keeps the whole line
};

struct EventMsg {
    MsgHeader h;
    std::uint32_t kind;
    std::uint32_t _pad;
    std::uint64_t frame_number;
    std::uint64_t guest_address;
    char detail[512];
};

struct ExitingMsg {
    MsgHeader h;
    std::int32_t code;
    char reason[512];
};

// The seats as they stand before the first frame. A core may read input
// outside a frame -- while unserialize() restores a state, say -- and a seat's
// connected flag is guest-visible, so the hub states it up front rather than
// leaving the runner to guess.
struct SavesFilledMsg {
    MsgHeader h;
    rcore_pad pads[RCORE_MAX_SEATS];
};

struct GrantMsg {
    MsgHeader h;
    std::uint64_t frame_number; // 1-based, as the headless runner counts
    rcore_pad pads[RCORE_MAX_SEATS];
};

struct EmptyMsg {
    MsgHeader h;
};

constexpr std::size_t kMaxMsgSize = sizeof(SaveRegionsMsg) > sizeof(LogMsg)
                                        ? sizeof(SaveRegionsMsg)
                                        : sizeof(LogMsg);

} // namespace retcomm::corelink
