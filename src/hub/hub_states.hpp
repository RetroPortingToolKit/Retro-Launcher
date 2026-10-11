#pragma once

// The save-state browser's data: twelve slots in <saves>/states/slotNN.rstate,
// read through Retro-Runtime's state envelope (header and thumbnail only), plus
// the one save or load the player asked for. The page that shows it is the hub's
// own overlay UI (hub_play.cpp, draw_states_page), not the runtime's pixel
// browser: one look for every menu, and the buttons pressed in it never reach
// the game.
//
// The runner does the saving and loading (CORE_LINK.md, link 1.1); this only
// lists, asks and reports.

#include <cstdint>
#include <filesystem>
#include <string>

namespace retcomm::hub {

namespace fs = std::filesystem;

class StatesBrowser {
public:
    static constexpr int kSlots = 12;

    struct Slot {
        bool exists = false;
        bool readable = false;
        std::int64_t saved_unix = 0;
        std::uint64_t frame = 0;
        std::string mark;      // "OTHER CORE", "OTHER BUILD", "UNREADABLE": a hint; the runner judges
        unsigned tex = 0;      // GL texture of the 160x120 thumbnail, 0 = none
    };

    struct Request {
        enum Kind { None, Save, Load } kind = None;
        int slot = -1;
        fs::path path;
    };

    ~StatesBrowser() { release(); }

    // The slots live in `dir`; `core_id` / `core_sha256` are the running core's,
    // for the "other core" / "other build" marks.
    void configure(const fs::path& dir, const std::string& core_id, const std::string& core_sha256);
    bool configured() const { return !dir_.empty(); }
    fs::path slot_path(int slot) const;

    // Re-read every slot. Needs the GL context (thumbnails become textures).
    void refresh();
    void release();
    const Slot& slot(int i) const { return slots_[i]; }

    int selected = 0;

    // Ask for a save or load of `slot`; ignored while one is outstanding.
    void ask(Request::Kind kind, int slot);
    // For the session's pump: what was asked since the last call.
    Request take_request();
    bool busy() const { return outstanding_; }
    // The runner's answer. A save re-reads its slot; a load that worked is
    // reported once through take_loaded().
    void finish(bool ok, bool was_save, const std::string& detail);
    bool take_loaded();

    // Delete a slot's file.
    bool remove(int slot);

    // The last answer or refusal, for the page ("Slot 3 saved").
    const std::string& status() const { return status_; }
    bool status_bad() const { return status_bad_; }
    void clear_status() { status_.clear(); status_bad_ = false; }

private:
    void scan_slot(int i);

    fs::path dir_;
    std::string core_id_, core_sha_;
    Slot slots_[kSlots];
    Request ready_;
    bool outstanding_ = false;
    int outstanding_slot_ = -1;
    bool loaded_ = false;
    std::string status_;
    bool status_bad_ = false;
};

} // namespace retcomm::hub
