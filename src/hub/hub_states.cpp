#include "hub/hub_states.hpp"

#include "hub/hub_gl.hpp"
#include "state_envelope.hpp" // Retro-Runtime: retro_state

#include <vector>

namespace retcomm::hub {

namespace state = ::retro::state;

void StatesBrowser::configure(const fs::path& dir, const std::string& core_id,
                              const std::string& core_sha256) {
    dir_ = dir;
    core_id_ = core_id;
    core_sha_ = core_sha256;
}

fs::path StatesBrowser::slot_path(int slot) const {
    char name[24];
    std::snprintf(name, sizeof name, "slot%02d.rstate", slot + 1);
    return dir_ / name;
}

void StatesBrowser::release() {
    for (Slot& s : slots_) {
        if (s.tex) glDeleteTextures(1, &s.tex);
        s = Slot{};
    }
}

void StatesBrowser::scan_slot(int i) {
    Slot& s = slots_[i];
    if (s.tex) glDeleteTextures(1, &s.tex);
    s = Slot{};
    const fs::path p = slot_path(i);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return;
    s.exists = true;
    state::StateHeader h;
    std::vector<std::uint8_t> thumb;
    std::string err;
    if (!state::read_state_header(p, h, &thumb, &err)) {
        s.mark = "UNREADABLE";
        return;
    }
    s.readable = true;
    s.saved_unix = h.saved_unix;
    s.frame = h.frame_number;
    if (!core_id_.empty() && h.identity.core_id != core_id_) s.mark = "OTHER CORE";
    else if (!h.identity.state_compat_id && !core_sha_.empty() &&
             h.identity.core_sha256 != core_sha_)
        s.mark = "OTHER BUILD";
    if (h.thumb_w == state::kThumbWidth && h.thumb_h == state::kThumbHeight &&
        thumb.size() == std::size_t(state::kThumbWidth) * state::kThumbHeight * 4) {
        glGenTextures(1, &s.tex);
        glBindTexture(GL_TEXTURE_2D, s.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLsizei(state::kThumbWidth),
                     GLsizei(state::kThumbHeight), 0, GL_RGBA, GL_UNSIGNED_BYTE, thumb.data());
    }
}

void StatesBrowser::refresh() {
    for (int i = 0; i < kSlots; ++i) scan_slot(i);
}

void StatesBrowser::ask(Request::Kind kind, int slot) {
    if (outstanding_ || kind == Request::None || slot < 0 || slot >= kSlots) return;
    ready_.kind = kind;
    ready_.slot = slot;
    ready_.path = slot_path(slot);
    outstanding_ = true;
    outstanding_slot_ = slot;
    clear_status();
}

StatesBrowser::Request StatesBrowser::take_request() {
    Request r = ready_;
    ready_ = Request{};
    return r;
}

void StatesBrowser::finish(bool ok, bool was_save, const std::string& detail) {
    const int slot = outstanding_slot_;
    outstanding_ = false;
    outstanding_slot_ = -1;
    ready_ = Request{};
    if (ok) {
        status_ = std::string("Slot ") + std::to_string(slot + 1) + (was_save ? " saved" : " loaded");
        status_bad_ = false;
        if (was_save && slot >= 0) scan_slot(slot);
        if (!was_save) loaded_ = true;
    } else {
        status_ = detail.empty() ? std::string(was_save ? "The save failed" : "The load failed") : detail;
        status_bad_ = true;
    }
}

bool StatesBrowser::take_loaded() {
    const bool l = loaded_;
    loaded_ = false;
    return l;
}

bool StatesBrowser::remove(int slot) {
    if (slot < 0 || slot >= kSlots || outstanding_) return false;
    std::error_code ec;
    fs::remove(slot_path(slot), ec);
    scan_slot(slot);
    status_ = ec ? "Could not delete the slot" : std::string("Slot ") + std::to_string(slot + 1) + " deleted";
    status_bad_ = bool(ec);
    return !ec;
}

} // namespace retcomm::hub
