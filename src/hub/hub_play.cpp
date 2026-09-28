#include "hub/hub_play.hpp"

#include "imgui.h"

#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <optional>

namespace retcomm::hub {

namespace {

constexpr std::uint64_t kNsPerSec = 1000000000ull;
constexpr std::uint64_t kFallbackFrameNs = kNsPerSec / 60;
// Audio pacing: keep this much queued; grant only below it.
constexpr double kTargetQueuedMs = 60.0;
constexpr std::uint64_t kPersistEveryNs = 5 * kNsPerSec;
constexpr std::uint32_t kSeats = 4;
// Turbo grants back to back for this long per hub frame, then draws.
constexpr std::uint64_t kTurboBudgetNs = 10 * 1000000ull;
// Turbo with sound, at the game's own pitch: the sound is kept in stretches --
// queued from when the queue is below the low mark until it passes the high
// mark -- and what turbo makes in between is skipped. Each stretch fades in and
// out over kTurboFadeMs so the splices do not click.
constexpr double kTurboSoundLowMs = 40.0;
constexpr double kTurboSoundHighMs = 100.0;
constexpr double kTurboFadeMs = 4.0;
constexpr int kVolumeStep = 10;

// Every connected gamepad, read by position (SOUTH is the bottom face button
// whatever it is labelled), OR-ed together, for the save-state browser --
// never through the player's bindings, so a remap cannot strand the menu.
// `stick_y` is the left stick furthest from centre, rcore sign (up = +).
std::uint32_t physical_pads(std::int16_t& stick_y) {
    struct Map {
        SDL_GamepadButton b;
        std::uint32_t bit;
    };
    static constexpr Map kMap[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, RCORE_PAD_SOUTH},
        {SDL_GAMEPAD_BUTTON_EAST, RCORE_PAD_EAST},
        {SDL_GAMEPAD_BUTTON_WEST, RCORE_PAD_WEST},
        {SDL_GAMEPAD_BUTTON_NORTH, RCORE_PAD_NORTH},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, RCORE_PAD_L1},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, RCORE_PAD_R1},
        {SDL_GAMEPAD_BUTTON_START, RCORE_PAD_START},
        {SDL_GAMEPAD_BUTTON_BACK, RCORE_PAD_SELECT},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, RCORE_PAD_DPAD_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, RCORE_PAD_DPAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, RCORE_PAD_DPAD_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, RCORE_PAD_DPAD_RIGHT},
    };
    std::uint32_t buttons = 0;
    int best = 0;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
        if (!g) continue;
        for (const Map& m : kMap) {
            if (SDL_GetGamepadButton(g, m.b)) buttons |= m.bit;
        }
        const int y = -int(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
        if (std::abs(y) > std::abs(best)) best = y;
    }
    SDL_free(ids);
    stick_y = static_cast<std::int16_t>(std::clamp(best, -32768, 32767));
    return buttons;
}

// The last `n` lines of a text file, for the fault screen.
std::vector<std::string> tail_lines(const fs::path& p, std::size_t n) {
    std::ifstream in(p);
    std::deque<std::string> lines;
    std::string l;
    while (std::getline(in, l)) {
        lines.push_back(l);
        if (lines.size() > n) lines.pop_front();
    }
    return {lines.begin(), lines.end()};
}

} // namespace

PlaySession::~PlaySession() { shutdown(); }

bool PlaySession::start(const PlayArgs& args, const fs::path& runner, const fs::path& session_dir,
                        const fs::path& save_dir, std::string* error) {
    args_ = args;
    prefs_ = args.prefs;
    osd_.set_fps_visible(prefs_.show_fps);
    // A title's save states live with its saves: <save_dir>/states/slotNN.rstate.
    states_dir_ = save_dir / "states";
    {
        // The browser's header: how to open and close it, as bound.
        std::string hint = shortcut_text(HostAction::SaveStates);
        for (char& c : hint) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        states_.set_hint(hint);
    }
    corelink::LaunchSpec spec;
    spec.runner = runner;
    spec.core = args.core;
    spec.rom = args.rom;
    if (!args.package.empty()) spec.package = corelink::path_utf8(args.package);
    // A packaged title's game.toml sits beside its shim, not beside the
    // generic core every packaged title shares.
    const fs::path& owner = args.package.empty() ? args.core : args.package;
    spec.title_dir = args.title_dir.empty() ? owner.parent_path() : args.title_dir;
    spec.session_dir = session_dir;
    spec.save_dir = save_dir;
    spec.gl = args.gl;
    spec.options = args.options;
    spec.tpak_roms = args.tpak_rom;
    spec.env = args.env;
    for (std::size_t seat = 0; seat < args.tpak_save.size(); ++seat)
        if (!args.tpak_rom[seat].empty() && !args.tpak_save[seat].empty())
            spec.save_files["tpak" + std::to_string(seat + 1)] = args.tpak_save[seat];
    // Port 1 holds a controller from power-on, as on the console; the other
    // seats follow the gamepads actually present at the first grant.
    spec.initial_pads[0].connected = 1;
    runner_log_ = session_dir / "runner.log";

    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        // No audio device is not fatal: pacing falls back to 60 Hz.
        SDL_Log("retro-hub: no audio (%s); the game runs silent", SDL_GetError());
    }
    return link_.start(spec, error);
}

bool PlaySession::handle_event(const SDL_Event& e) {
    // A KEY_UP lost to a focus change must not leave turbo stuck on.
    if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) turbo_key_ = turbo_ = false;
    if (link_.state() != corelink::LinkState::Ready) return false;
    // The browser, while open, has the keyboard and the pads to itself.
    if (states_.is_open()) return handle_states_event(e);

    // The keyboard shortcuts as bound (PlayPrefs::hotkeys, the settings
    // page's Hotkeys panel); F1 and keypad +/- always work too. Controller
    // shortcuts are read each frame in poll_host_combos().
    const HostHotkeys& hk = prefs_.hotkeys;
    auto is = [&](HostAction a) {
        const SDL_Scancode k = hk.key[static_cast<size_t>(a)];
        return k != SDL_SCANCODE_UNKNOWN && e.key.scancode == k;
    };
    // No Guide: Steam (and the OS) keep it for themselves on most setups; the
    // pad's way in is L3 + R3 or the Function combo (poll_host_combos).
    const bool toggle = e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                        (is(HostAction::Menu) || e.key.key == SDLK_F1);
    if (toggle) {
        set_paused(!menu_open_);
        return true;
    }
    if (e.type != SDL_EVENT_KEY_DOWN && e.type != SDL_EVENT_KEY_UP) return false;
    const bool down = e.type == SDL_EVENT_KEY_DOWN;
    if (is(HostAction::ShowFps)) {
        if (down && !e.key.repeat) set_show_fps(!prefs_.show_fps);
        return true;
    }
    if (is(HostAction::SaveStates)) {
        if (down && !e.key.repeat && !menu_open_) open_states();
        return true;
    }
    if (is(HostAction::Turbo)) { // held
        if (menu_open_) return false;
        if (!e.key.repeat) turbo_key_ = down;
        return true;
    }
    if (is(HostAction::VolumeUp) || e.key.key == SDLK_KP_PLUS) {
        if (down) set_volume(prefs_.volume + kVolumeStep);
        return true;
    }
    if (is(HostAction::VolumeDown) || e.key.key == SDLK_KP_MINUS) {
        if (down) set_volume(prefs_.volume - kVolumeStep);
        return true;
    }
    return false;
}

std::string PlaySession::shortcut_text(HostAction a) const {
    const SDL_Scancode k = prefs_.hotkeys.key[static_cast<size_t>(a)];
    const std::string combo = pad_source_label(prefs_.hotkeys.combo[static_cast<size_t>(a)]);
    std::string out = k == SDL_SCANCODE_UNKNOWN ? std::string() : SDL_GetScancodeName(k);
    if (!combo.empty()) out += (out.empty() ? "" : " or ") + std::string("Function+") + combo;
    return out;
}

// The controller shortcuts, on every seat's pad: its Function input held plus
// the action's button (edges, except Turbo, which is held), and L3 + R3
// together for the menu. A seat holding Function sends the game no buttons
// (fill_pads_from_input), so a shortcut never presses one.
void PlaySession::poll_host_combos() {
    const HostHotkeys& hk = prefs_.hotkeys;
    std::array<bool, kHostActionCount> held{};
    bool l3r3 = false;
    for (const SeatPadState& s : seat_pad_states(args_.input)) {
        if (!s.pad) continue;
        if (SDL_GetGamepadButton(s.pad, SDL_GAMEPAD_BUTTON_LEFT_STICK) &&
            SDL_GetGamepadButton(s.pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK))
            l3r3 = true;
        if (!s.function) continue;
        for (int a = 0; a < kHostActionCount; ++a)
            if (pad_source_down(s.pad, hk.combo[static_cast<size_t>(a)])) held[static_cast<size_t>(a)] = true;
    }
    auto edge = [&](HostAction a) {
        return held[static_cast<size_t>(a)] && !combo_prev_[static_cast<size_t>(a)];
    };
    const bool menu = (l3r3 && !l3r3_prev_) || edge(HostAction::Menu);
    l3r3_prev_ = l3r3;
    if (menu) {
        if (states_.is_open()) states_.close();
        else set_paused(!menu_open_);
    }
    if (edge(HostAction::SaveStates)) {
        if (states_.is_open()) states_.close();
        else if (!menu_open_) open_states();
    }
    if (edge(HostAction::ShowFps)) set_show_fps(!prefs_.show_fps);
    if (edge(HostAction::VolumeUp)) set_volume(prefs_.volume + kVolumeStep);
    if (edge(HostAction::VolumeDown)) set_volume(prefs_.volume - kVolumeStep);
    // The combo toggles turbo (the key is held instead).
    if (edge(HostAction::Turbo) && !menu_open_) turbo_pad_ = !turbo_pad_;
    combo_prev_ = held;
}

bool PlaySession::handle_states_event(const SDL_Event& e) {
    using Key = retro::overlay::SavestateMenu::Key;
    if (e.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode k = e.key.key;
        if (k == SDLK_UP || k == SDLK_LEFT) states_.key(Key::Up);
        else if (k == SDLK_DOWN || k == SDLK_RIGHT) states_.key(Key::Down);
        else if (e.key.repeat) {
        } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) states_.key(Key::Load);
        else if (k == SDLK_S) states_.key(Key::Save);
        else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE || k == SDLK_F7) states_.key(Key::Back);
        else if (k >= SDLK_1 && k <= SDLK_9) states_.jump(int(k - SDLK_1));
        else if (k == SDLK_0) states_.jump(9);
        else if (k == SDLK_MINUS) states_.jump(10);
        else if (k == SDLK_EQUALS) states_.jump(11);
        else if (k == SDLK_F3) set_show_fps(!prefs_.show_fps);
        return true;
    }
    // Pads are read in tick(); none of it reaches ImGui behind the browser.
    return e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
           e.type == SDL_EVENT_GAMEPAD_BUTTON_UP || e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
}

void PlaySession::set_paused(bool paused) {
    menu_open_ = paused;
    sync_pause();
}

// Offline the quick menu and the save-state browser pause (HOST_LIFECYCLE.md):
// no grants, no sound.
void PlaySession::sync_pause() {
    const bool want = paused();
    if (want) next_grant_ns_ = 0;
    if (was_paused_ && !want) osd_.restart_fps(); // the pause is not a slow frame
    was_paused_ = want;
    if (!audio_ || want == audio_paused_) return;
    if (want) SDL_PauseAudioStreamDevice(audio_);
    else SDL_ResumeAudioStreamDevice(audio_);
    audio_paused_ = want;
}

void PlaySession::set_volume(int percent) {
    prefs_.volume = std::clamp(percent, 0, 100);
    if (audio_) SDL_SetAudioStreamGain(audio_, float(prefs_.volume) / 100.0f);
    osd_.show_volume(prefs_.volume, SDL_GetTicksNS());
    save_prefs();
}

void PlaySession::set_show_fps(bool on) {
    prefs_.show_fps = on;
    osd_.set_fps_visible(on);
    save_prefs();
}

void PlaySession::save_prefs() {
    if (args_.data_dir.empty()) return;
    std::string err;
    if (!save_play_prefs(args_.data_dir, prefs_, &err)) {
        SDL_Log("retro-hub: play settings not saved: %s", err.c_str());
    }
}

// The hotkey's way in; the pad chord opens it in poll_states_pad(). A core
// without savestates, or a runner from before link 1.1, gets a toast instead.
void PlaySession::open_states() {
    const corelink::CoreIdentity& id = link_.identity();
    if (!link_.states_supported()) {
        const std::string why =
            id.protocol_minor < 1 ? "Save states need a newer retro-core-runner"
                                  : id.core_id + " has no save states";
        osd_.toast(why, SDL_GetTicksNS(), 3000);
        states_.close();
        return;
    }
    if (!states_configured_) {
        states_.configure(states_dir_, id.core_id, id.sha256);
        states_configured_ = true;
    }
    states_.open();
}

void PlaySession::poll_states_pad(std::uint64_t now) {
    std::int16_t stick_y = 0;
    // Select is Function's by default, and the browser's own Select + R1
    // chord would open it behind poll_host_combos()'s back: keep it out.
    const std::uint32_t buttons = physical_pads(stick_y) & ~std::uint32_t(RCORE_PAD_SELECT);
    if (menu_open_) return; // the quick menu has the pads
    if (states_.poll_pad(buttons, stick_y, now / 1000000ull)) {
        states_.close();
        open_states();
    }
}

void PlaySession::service_states() {
    const std::uint64_t now = SDL_GetTicksNS();
    // Closing: whatever is held now stays out of the game until released.
    const bool open = states_.is_open();
    if (states_were_open_ && !open) {
        rcore_pad pads[RCORE_MAX_SEATS];
        fill_pads(pads);
        guard_.arm(pads, kSeats);
    }
    states_were_open_ = open;
    if (std::string t = states_.take_toast(); !t.empty()) osd_.toast(t, now);

    using Req = retro::overlay::SavestateMenu::Request;
    if (state_request_.kind == Req::None) state_request_ = states_.take_request();
    if (state_request_.kind != Req::None && !link_.state_pending()) {
        // The frame in flight finishes first: a state is taken between frames.
        if (link_.state() != corelink::LinkState::Ready) {
            states_.finish(false, "the game stopped");
            state_request_ = Req{};
        } else if (link_.can_request_state()) {
            const bool sent = state_request_.kind == Req::Save
                                  ? link_.request_save_state(state_request_.path)
                                  : link_.request_load_state(state_request_.path);
            if (!sent) states_.finish(false, "the link could not send the request");
            state_request_ = Req{};
        }
    }
    if (std::optional<corelink::StateResult> r = link_.take_state_result()) {
        SDL_Log("retro-hub: state %s %s: %s", r->save ? "save" : "load",
                r->path.string().c_str(), r->ok ? "ok" : r->detail.c_str());
        states_.finish(r->ok, r->detail);
        if (r->ok && !r->save) next_grant_ns_ = 0;
    }
}

void PlaySession::fill_pads(rcore_pad pads[RCORE_MAX_SEATS]) {
    // The platform's seats: which device each port reads, and its map.
    fill_pads_from_input(args_.input, pads, kSeats);
}

void PlaySession::grant(std::uint64_t) {
    rcore_pad pads[RCORE_MAX_SEATS];
    fill_pads(pads);
    guard_.apply(pads, kSeats);
    link_.grant(pads);
}

// Turbo: grant again as soon as a frame is done, for most of a hub frame, and
// throw the sound away (pump_audio). The machine runs exactly the frames it
// always would; only the host stops holding it back.
void PlaySession::run_turbo() {
    const std::uint64_t until = SDL_GetTicksNS() + kTurboBudgetNs;
    while (link_.state() == corelink::LinkState::Ready && !paused() && !link_.state_pending()) {
        if (link_.can_grant()) grant(SDL_GetTicksNS());
        link_.pump(1);
        note_frames(SDL_GetTicksNS());
        pump_audio();
        if (SDL_GetTicksNS() >= until) break;
    }
}

// Vsync goes off with turbo and comes back after it. A present that waits for
// the display leaves one grant window per refresh, so a core that takes longer
// than kTurboBudgetNs a frame finished each frame inside the wait and got the
// next grant only on the next refresh: 60 fps on a 60 Hz screen, whatever the
// core could do (n64lle, about 12.5 ms a frame, ran exactly that).
void PlaySession::set_turbo_running(bool on) {
    turbo_running_ = on;
    // Turbo's sound (keep_turbo_sound) starts a stretch afresh; when turbo
    // ends, the stretch in progress fades out before the game's own sound.
    if (!on) end_turbo_stretch();
    turbo_keeping_ = false;
    turbo_tail_.clear();
    if (on) {
        if (audio_) SDL_ClearAudioStream(audio_); // no stale sound
        int interval = 0;
        if (SDL_GL_GetSwapInterval(&interval) && interval != 0 && SDL_GL_SetSwapInterval(0))
            saved_swap_interval_ = interval;
    } else if (saved_swap_interval_) {
        SDL_GL_SetSwapInterval(*saved_swap_interval_);
        saved_swap_interval_.reset();
    }
}

// Every frame the core finished since the last call, for the FPS readout,
// spread evenly over the time since (a hub frame can collect several).
void PlaySession::note_frames(std::uint64_t now) {
    const std::uint64_t done = link_.frames_done();
    if (done <= noted_frames_) return;
    const std::uint64_t n = done - noted_frames_;
    if (noted_ns_ && now > noted_ns_) {
        for (std::uint64_t i = 1; i < n; ++i) osd_.note_frame(noted_ns_ + (now - noted_ns_) * i / n);
    }
    osd_.note_frame(now);
    noted_frames_ = done;
    noted_ns_ = now;
}

double PlaySession::audio_queued_ms() const {
    if (!audio_ || !audio_hz_) return 0.0;
    return SDL_GetAudioStreamQueued(audio_) / (double(audio_hz_) * 4.0) * 1000.0;
}

bool PlaySession::audio_queue_full() const {
    return audio_ && audio_hz_ && audio_queued_ms() >= kTargetQueuedMs;
}

// A frame still running when the hub frame starts would get its successor's
// grant only on the next refresh: one grant window per present, so a core
// whose frames alternate either side of the refresh period (n64lle's Pokemon
// Stadium flyover: about 14 / 18.5 ms a field against 16.7) lost a whole
// refresh on every long one and read 50 fps while averaging above 60. Wait
// for it here, up to half a frame period, so it is shown and the next one is
// granted before this hub frame draws; the present still makes the same
// vsync. Only when the next grant would be due: a full audio queue means
// nothing is gained by waiting.
void PlaySession::await_late_frame() {
    if (paused() || link_.state_pending() || audio_queue_full()) return;
    std::uint32_t num = 0, den = 0;
    const std::uint64_t period =
        link_.frame_rate(num, den) ? std::uint64_t(den) * kNsPerSec / num : kFallbackFrameNs;
    const std::uint64_t until = SDL_GetTicksNS() + period / 2;
    while (link_.state() == corelink::LinkState::Ready &&
           link_.frames_granted() > link_.frames_done()) {
        const std::uint64_t now = SDL_GetTicksNS();
        if (now >= until) break;
        // pump() returns at the first message of any kind; loop to FrameDone.
        link_.pump(int((until - now + 999999) / 1000000));
    }
}

void PlaySession::grant_if_due() {
    if (paused() || !link_.can_grant()) return;
    const std::uint64_t now = SDL_GetTicksNS();
    bool due;
    if (audio_ && audio_hz_) {
        // The core's audio clock paces it: grant while the queue is short.
        due = !audio_queue_full();
    } else {
        // No audio to follow: the core's stated frame rate, else 60 Hz.
        std::uint32_t num = 0, den = 0;
        const std::uint64_t period =
            link_.frame_rate(num, den) ? std::uint64_t(den) * kNsPerSec / num : kFallbackFrameNs;
        if (next_grant_ns_ == 0 || now > next_grant_ns_ + 100000000ull) next_grant_ns_ = now;
        due = now >= next_grant_ns_;
        if (due) next_grant_ns_ += period;
    }
    if (!due) return;
    grant(now);
}

void PlaySession::pump_audio() {
    const std::uint32_t hz = link_.audio_rate();
    if (hz && hz != audio_hz_ && SDL_WasInit(SDL_INIT_AUDIO)) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(hz)};
        if (!audio_) {
            audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr,
                                               nullptr);
            if (audio_) {
                SDL_SetAudioStreamGain(audio_, float(prefs_.volume) / 100.0f);
                audio_paused_ = true; // opened paused
                sync_pause();
            }
        } else {
            // A guest may change its DAC rate mid-run; SDL resamples.
            SDL_SetAudioStreamFormat(audio_, &spec, nullptr);
        }
        audio_hz_ = hz;
    }
    audio_buf_.resize(4096 * 2);
    for (;;) {
        const std::size_t n = link_.drain_audio(audio_buf_.data(), 4096);
        if (!n) break;
        if (!audio_) continue;
        if (!turbo_) SDL_PutAudioStreamData(audio_, audio_buf_.data(), static_cast<int>(n * 4));
        else if (prefs_.turbo_sound) keep_turbo_sound(audio_buf_.data(), n);
    }
}

// Turbo makes sound faster than it can be played. Speeding the playback up
// raises its pitch and distorts it; instead the sound stays at its own pitch
// and is kept in stretches, the rest skipped, the way emulators fast-forward.
// A stretch starts (fading in) once the queue is below kTurboSoundLowMs and
// ends (its held-back tail fading out) once it is past kTurboSoundHighMs.
void PlaySession::keep_turbo_sound(std::int16_t* s, std::size_t frames) {
    const double q = audio_queued_ms();
    const std::size_t fade = std::max<std::size_t>(1, static_cast<std::size_t>(audio_hz_ * kTurboFadeMs / 1000.0));
    if (!turbo_keeping_) {
        if (q > kTurboSoundLowMs) return; // still playing the last stretch: skip
        turbo_keeping_ = true;
        const std::size_t in = std::min(fade, frames);
        for (std::size_t i = 0; i < in; ++i) {
            const float g = float(i) / float(in);
            s[2 * i] = static_cast<std::int16_t>(s[2 * i] * g);
            s[2 * i + 1] = static_cast<std::int16_t>(s[2 * i + 1] * g);
        }
    } else if (q > kTurboSoundHighMs) {
        end_turbo_stretch(); // this chunk is skipped
        return;
    }
    // Carry on: the last chunk's tail, then this one less its own tail.
    if (!turbo_tail_.empty())
        SDL_PutAudioStreamData(audio_, turbo_tail_.data(), static_cast<int>(turbo_tail_.size() * 2));
    const std::size_t keep = frames > fade ? frames - fade : 0;
    SDL_PutAudioStreamData(audio_, s, static_cast<int>(keep * 4));
    turbo_tail_.assign(s + keep * 2, s + frames * 2);
}

// The stretch in progress ends: its held-back tail fades out and is queued.
void PlaySession::end_turbo_stretch() {
    if (audio_ && !turbo_tail_.empty()) {
        const std::size_t n = turbo_tail_.size() / 2;
        for (std::size_t i = 0; i < n; ++i) {
            const float g = 1.f - float(i + 1) / float(n);
            turbo_tail_[2 * i] = static_cast<std::int16_t>(turbo_tail_[2 * i] * g);
            turbo_tail_[2 * i + 1] = static_cast<std::int16_t>(turbo_tail_[2 * i + 1] * g);
        }
        SDL_PutAudioStreamData(audio_, turbo_tail_.data(), static_cast<int>(turbo_tail_.size() * 2));
    }
    turbo_tail_.clear();
    turbo_keeping_ = false;
}

void PlaySession::upload_frame() {
    if (!link_.take_frame()) return;
    const corelink::FrameInfo* f = link_.frame_info();
    if (!f || !f->width || !f->height) return;
    if (!tex_) {
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (f->width != tex_w_ || f->height != tex_h_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f->width, f->height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, link_.frame_pixels());
        tex_w_ = f->width;
        tex_h_ = f->height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->width, f->height, GL_RGBA, GL_UNSIGNED_BYTE,
                        link_.frame_pixels());
    }
    aspect_num_ = f->aspect_num;
    aspect_den_ = f->aspect_den;
}

void PlaySession::tick() {
    link_.pump(0);
    const bool ready = link_.state() == corelink::LinkState::Ready;
    if (ready) poll_host_combos();
    // Turbo while its key or combo is held, and never behind a menu. Worked
    // out before the Ended check, so a game that stops mid-turbo gets its
    // vsync back (set_turbo_running).
    turbo_ = (turbo_key_ || turbo_pad_) && ready && !paused();
    if (turbo_ != turbo_running_) set_turbo_running(turbo_);
    osd_.set_turbo(turbo_);
    if (link_.state() == corelink::LinkState::Ended) {
        if (audio_) SDL_PauseAudioStreamDevice(audio_);
        if (states_.pending()) states_.finish(false, "the game stopped");
        return;
    }
    if (ready) {
        poll_states_pad(SDL_GetTicksNS());
        service_states();
    }
    sync_pause();
    if (!turbo_) await_late_frame();
    pump_audio();
    note_frames(SDL_GetTicksNS());
    upload_frame();
    if (turbo_) run_turbo();
    else grant_if_due();
    const std::uint64_t now = SDL_GetTicksNS();
    if (now - last_persist_ns_ > kPersistEveryNs) {
        link_.persist_saves();
        last_persist_ns_ = now;
    }
}

void PlaySession::draw() {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, 255));
    if (tex_) {
        // Letterbox to the core's stated aspect, or square pixels.
        const float aspect = (aspect_num_ && aspect_den_)
                                 ? float(aspect_num_) / float(aspect_den_)
                                 : float(tex_w_) / float(tex_h_);
        float w = disp.x, h = disp.x / aspect;
        if (h > disp.y) {
            h = disp.y;
            w = disp.y * aspect;
        }
        const ImVec2 p0((disp.x - w) * 0.5f, (disp.y - h) * 0.5f);
        const ImU32 tint = paused() ? IM_COL32(110, 110, 110, 255) : IM_COL32_WHITE;
        bg->AddImage((ImTextureID)(intptr_t)tex_, p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0),
                     ImVec2(1, 1), tint);
    }
    switch (link_.state()) {
        case corelink::LinkState::Starting:
        case corelink::LinkState::Idle:
            draw_loading();
            break;
        case corelink::LinkState::Ended:
            if (user_quit_) finished_ = true;
            else draw_fault();
            break;
        case corelink::LinkState::Ready:
            draw_overlay();
            if (menu_open_) draw_menu();
            break;
    }
}

// Retro-Runtime's overlay, as it hands it over: one texture per layer,
// re-uploaded when the layer's pixels change, drawn where place() says, over
// the picture and under the hub's own windows.
void PlaySession::draw_overlay() {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    std::vector<retro::overlay::Layer> layers = osd_.layers(SDL_GetTicksNS());
    if (const retro::overlay::Layer* panel = states_.layer()) layers.push_back(*panel);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (const retro::overlay::Layer& l : layers) {
        const retro::overlay::Image& img = *l.image;
        LayerTex& t = layer_tex_[l.id];
        if (!t.tex) {
            glGenTextures(1, &t.tex);
            glBindTexture(GL_TEXTURE_2D, t.tex);
            // Nearest: the overlay is pixel type, drawn at whole multiples.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            t.rev = ~l.revision;
        }
        if (t.rev != l.revision || t.w != img.width || t.h != img.height) {
            glBindTexture(GL_TEXTURE_2D, t.tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLsizei(img.width), GLsizei(img.height), 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
            t.rev = l.revision;
            t.w = img.width;
            t.h = img.height;
        }
        const retro::overlay::Rect r = retro::overlay::place(l, disp.x, disp.y);
        dl->AddImage((ImTextureID)(intptr_t)t.tex, ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h));
    }
}

void PlaySession::draw_loading() {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::Begin("##play_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    const std::string& id = link_.identity().core_id;
    const fs::path& what = args_.package.empty() ? args_.core : args_.package;
    ImGui::Text("Loading %s…", id.empty() ? what.filename().string().c_str() : id.c_str());
    ImGui::End();
}

void PlaySession::draw_menu() {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::Begin("Paused", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
    const corelink::CoreIdentity& id = link_.identity();
    ImGui::Text("%s %s%s", id.core_id.c_str(), id.core_version.c_str(),
                id.engine_dirty ? "  (dev build)" : "");
    ImGui::TextDisabled("frame %llu", static_cast<unsigned long long>(link_.frames_done()));
    ImGui::Separator();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::Button("Resume", ImVec2(220, 0))) set_paused(false);
    ImGui::BeginDisabled(!link_.states_supported());
    if (ImGui::Button("Save states", ImVec2(220, 0))) {
        set_paused(false);
        open_states();
    }
    ImGui::EndDisabled();
    bool fps = prefs_.show_fps;
    if (ImGui::Checkbox("Show FPS", &fps)) set_show_fps(fps);
    int vol = prefs_.volume;
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("##volume", &vol, 0, 100, "Volume %d%%")) {
        prefs_.volume = vol;
        if (audio_) SDL_SetAudioStreamGain(audio_, float(vol) / 100.0f);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) save_prefs();
    if (ImGui::Button("Close game", ImVec2(220, 0))) {
        user_quit_ = true;
        link_.stop();
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s: FPS  \xC2\xB7  %s: turbo", shortcut_text(HostAction::ShowFps).c_str(),
                        shortcut_text(HostAction::Turbo).c_str());
    ImGui::TextDisabled("%s / %s: volume", shortcut_text(HostAction::VolumeUp).c_str(),
                        shortcut_text(HostAction::VolumeDown).c_str());
    ImGui::TextDisabled("%s: save states", shortcut_text(HostAction::SaveStates).c_str());
    ImGui::TextDisabled("%s or L3+R3: this menu", shortcut_text(HostAction::Menu).c_str());
    ImGui::End();
}

void PlaySession::draw_fault() {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(disp.x * 0.8f, disp.y * 0.7f), ImGuiCond_Always);
    ImGui::Begin("The game stopped", nullptr,
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoMove);
    ImGui::TextWrapped("Exit code %d. %s", link_.exit_code(),
                       link_.exit_reason().empty() ? "The runner gave no reason."
                                                   : link_.exit_reason().c_str());
    ImGui::TextDisabled("Saves were written before this screen appeared.");
    for (const corelink::LinkEvent& e : link_.events) {
        if (e.kind == RCORE_EVENT_FAULT || e.kind == RCORE_EVENT_DISPATCH_MISS) {
            ImGui::TextWrapped("%s at frame %llu, 0x%08llx: %s",
                               e.kind == RCORE_EVENT_FAULT ? "FAULT" : "DISPATCH_MISS",
                               static_cast<unsigned long long>(e.frame_number),
                               static_cast<unsigned long long>(e.guest_address), e.detail.c_str());
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", runner_log_.string().c_str());
    ImGui::BeginChild("##runner_log", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.5f),
                      ImGuiChildFlags_Borders);
    if (!fault_log_loaded_) {
        fault_log_ = tail_lines(runner_log_, 40);
        fault_log_loaded_ = true;
    }
    for (const std::string& l : fault_log_) ImGui::TextUnformatted(l.c_str());
    ImGui::EndChild();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::Button("Close", ImVec2(220, 0))) finished_ = true;
    ImGui::End();
}

void PlaySession::shutdown() {
    turbo_ = turbo_key_ = turbo_pad_ = false;
    if (turbo_running_) set_turbo_running(false);
    if (link_.state() != corelink::LinkState::Idle) link_.stop();
    if (audio_) {
        SDL_DestroyAudioStream(audio_);
        audio_ = nullptr;
    }
    if (tex_) {
        glDeleteTextures(1, &tex_);
        tex_ = 0;
    }
    for (auto& [id, t] : layer_tex_) {
        if (t.tex) glDeleteTextures(1, &t.tex);
    }
    layer_tex_.clear();
}

} // namespace retcomm::hub
