#pragma once

// The hub's side of accessory data (Retro-Runtime link 2.1, rcore rev 7;
// docs/CORE_ABI.md "Accessory data"): one whole message at a time, hub ->
// runner as AccessoryData (69) for the core's accessory_poll, runner -> hub as
// AccessoryNotify (9) from the core's accessory_notify. The VRU's messages are
// JSON lines (hub_vru.hpp).
//
// A seam, so the VRU code is the same whether the vendored Retro-Runtime
// carries the link or not: RETCOMM_LINK_ACCESSORY_DATA is set by CMake when
// third_party/Retro-Runtime's link_protocol.hpp has AccessoryData. Without
// it nothing is sent or invented -- supported() is false and why() says the
// vendored link predates accessory data, which the panel and the session
// show in those words.

#include "core_link.hpp" // Retro-Runtime: retro_corelink

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace retcomm::hub {

struct AccessoryMessage {
    std::uint32_t seat = 0, slot = 0;
    std::string bytes;
};

class AccessoryLink {
public:
    explicit AccessoryLink(::retro::corelink::CoreLink& link) : link_(link) {}

    // The session can carry accessory data: a 2.1 runner and a core with
    // CAP_ACCESSORY_DATA, once the link is Ready.
    bool supported() const {
#if defined(RETCOMM_LINK_ACCESSORY_DATA)
        return link_.accessory_data_supported();
#else
        return false;
#endif
    }
    // Why not, in one sentence, for the player.
    static const char* why_unsupported() {
#if defined(RETCOMM_LINK_ACCESSORY_DATA)
        return "the runner or the core does not carry accessory data (Retro-Runtime link 2.1 "
               "and a core with CAP_ACCESSORY_DATA are needed)";
#else
        return "the vendored Retro-Runtime link predates accessory data (link 2.1 is needed)";
#endif
    }
    // Whether this hub was built with the link at all.
    static constexpr bool compiled_in() {
#if defined(RETCOMM_LINK_ACCESSORY_DATA)
        return true;
#else
        return false;
#endif
    }

    bool send(std::uint32_t seat, std::uint32_t slot, const std::string& bytes) {
#if defined(RETCOMM_LINK_ACCESSORY_DATA)
        return link_.send_accessory(seat, slot, bytes.data(), bytes.size());
#else
        (void)seat;
        (void)slot;
        (void)bytes;
        return false;
#endif
    }

    std::optional<AccessoryMessage> poll() {
#if defined(RETCOMM_LINK_ACCESSORY_DATA)
        if (auto n = link_.poll_accessory_notify()) {
            AccessoryMessage m;
            m.seat = n->seat;
            m.slot = n->slot;
            m.bytes.assign(n->bytes.begin(), n->bytes.end());
            return m;
        }
#endif
        return std::nullopt;
    }

private:
    ::retro::corelink::CoreLink& link_;
};

// The seats a LaunchSpec is told hold the VRU (--vruN). On a link without
// accessory data there is no such field: false, and the caller says so.
inline bool spec_set_vru_seats(::retro::corelink::LaunchSpec& spec, const std::array<bool, 4>& seats) {
#if defined(RETCOMM_LINK_ACCESSORY_DATA)
    spec.vru_seats = seats;
    return true;
#else
    (void)spec;
    (void)seats;
    return false;
#endif
}

} // namespace retcomm::hub
