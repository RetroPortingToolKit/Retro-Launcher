#include "host_session.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace retcomm::runner {

HostSession::HostSession(const LoadedCore& core, Sink& sink) : core_(core), sink_(sink) {
    host_.struct_size = sizeof(rcore_host_api);
    host_.abi_major = RCORE_ABI_MAJOR;
    host_.abi_minor = RCORE_ABI_MINOR;
    host_.host_ctx = this;
    host_.log = h_log;
    host_.report = h_report;
    host_.video_submit = h_video;
    host_.audio_push = h_audio;
    host_.set_audio_rate = h_rate;
    host_.input_get = h_input;
    host_.rumble = h_rumble;
    host_.option_get = h_option;
    host_.options_changed = h_options_changed;
    host_.save_memory = h_save;
    host_.frame_boundary = nullptr; // RUN_FRAME cores only, for now
    host_.gl_get_proc_address = nullptr; // set per session by lend_gl()
    host_.wall_clock_us = h_wall_clock;
}

bool HostSession::set_options(const std::map<std::string, std::string>& overrides,
                              std::string* error) {
    options_.clear();
    std::uint32_t n = 0;
    const rcore_option* decl = core_.api->options ? core_.api->options(&n) : nullptr;
    for (std::uint32_t i = 0; i < n; ++i) {
        const rcore_option& o = decl[i];
        options_[o.key] = o.default_value ? std::optional<std::string>(o.default_value)
                                          : std::nullopt;
    }
    for (const auto& [k, v] : overrides) {
        auto it = options_.find(k);
        if (it == options_.end()) {
            if (error) *error = "option '" + k + "': the core declares no such option";
            return false;
        }
        it->second = v;
    }
    return true;
}

void HostSession::adopt_save_regions(const rcore_save_region* regions, std::uint32_t count,
                                     const std::map<std::string, fs::path>& files) {
    regions_.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        const rcore_save_region& r = regions[i];
        SaveRegion s;
        s.id = r.id;
        s.kind = r.kind;
        s.seat = r.seat;
        s.slot = r.slot;
        // A region never saved is filled with its erase value (rev 3); the
        // core never learns whether it is fresh.
        s.erase_value = RCORE_HAS(&r, rcore_save_region, erase_value) ? r.erase_value : 0;
        s.owned.assign(static_cast<size_t>(r.size), std::uint8_t(s.erase_value));
        s.data = s.owned.data();
        s.size = s.owned.size();
        if (auto f = files.find(s.id); f != files.end()) {
            s.file = f->second;
            std::ifstream in(f->second, std::ios::binary);
            if (in) {
                in.read(reinterpret_cast<char*>(s.data), static_cast<std::streamsize>(s.size));
            }
        }
        regions_.push_back(std::move(s));
    }
}

void HostSession::adopt_external_save_regions(const rcore_save_region* regions,
                                              std::uint32_t count,
                                              const std::vector<std::uint8_t*>& memory) {
    regions_.clear();
    for (std::uint32_t i = 0; i < count && i < memory.size(); ++i) {
        const rcore_save_region& r = regions[i];
        SaveRegion s;
        s.id = r.id;
        s.kind = r.kind;
        s.seat = r.seat;
        s.slot = r.slot;
        s.erase_value = RCORE_HAS(&r, rcore_save_region, erase_value) ? r.erase_value : 0;
        s.data = memory[i];
        s.size = static_cast<size_t>(r.size);
        regions_.push_back(std::move(s));
    }
}

void HostSession::persist_save_regions() const {
    for (const SaveRegion& r : regions_) {
        if (!r.file) continue;
        std::ofstream out(*r.file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(r.data), static_cast<std::streamsize>(r.size));
    }
}

void HostSession::h_log(void* ctx, std::uint32_t level, const char* msg) {
    self(ctx)->sink_.log(level, msg ? msg : "");
}

void HostSession::h_report(void* ctx, const rcore_event* e) {
    if (e) self(ctx)->sink_.event(*e);
}

void HostSession::h_video(void* ctx, const rcore_frame* f) {
    if (f) self(ctx)->sink_.frame(*f);
}

void HostSession::h_audio(void* ctx, const std::int16_t* s, std::uint32_t n) {
    self(ctx)->sink_.audio(s, n);
}

void HostSession::h_rate(void* ctx, std::uint32_t hz) { self(ctx)->sink_.audio_rate(hz); }

void HostSession::h_input(void* ctx, std::uint32_t seat, rcore_pad* out) {
    if (!out) return;
    rcore_pad pad{};
    pad.struct_size = sizeof(rcore_pad);
    if (seat < RCORE_MAX_SEATS) self(ctx)->sink_.input(seat, pad);
    *out = pad;
}

void HostSession::h_rumble(void* ctx, std::uint32_t seat, std::uint16_t lo, std::uint16_t hi) {
    self(ctx)->sink_.rumble(seat, lo, hi);
}

const char* HostSession::h_option(void* ctx, const char* key) {
    HostSession* s = self(ctx);
    auto it = s->options_.find(key ? key : "");
    if (it == s->options_.end()) {
        // Rev 3: an undeclared key is a contract violation, and NULL must only
        // ever mean "unset" -- so the session ends here, naming the key.
        std::fprintf(stderr,
                     "retcomm-core-runner: FAULT: option_get(\"%s\"): the core asked for a key "
                     "it did not declare\n",
                     key ? key : "(null)");
        std::fflush(stderr);
        std::exit(3);
    }
    return it->second ? it->second->c_str() : nullptr;
}

std::uint32_t HostSession::h_options_changed(void*) { return 0; }

void* HostSession::h_save(void* ctx, const char* id) {
    for (SaveRegion& r : self(ctx)->regions_) {
        if (id && r.id == id) return r.data;
    }
    return nullptr;
}

void* HostSession::h_gl_proc(void* ctx, const char* name) {
    HostSession* s = self(ctx);
    return s->gl_proc_ ? s->gl_proc_(name) : nullptr;
}

std::uint64_t HostSession::h_wall_clock(void*) {
    // Offline: real time. A netplay or replay session will return a pure
    // function of the agreed epoch and the frame number instead (rev 4).
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

} // namespace retcomm::runner
