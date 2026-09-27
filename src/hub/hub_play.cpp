#include "hub/hub_play.hpp"

#include "imgui.h"

#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <deque>
#include <fstream>

namespace retcomm::hub {

namespace {

constexpr std::uint64_t kNsPerSec = 1000000000ull;
constexpr std::uint64_t kFallbackFrameNs = kNsPerSec / 60;
// Audio pacing: keep this much queued; grant only below it.
constexpr double kTargetQueuedMs = 60.0;
constexpr std::uint64_t kPersistEveryNs = 5 * kNsPerSec;
constexpr std::uint32_t kSeats = 4;

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
    spec.tpak_rom = args.tpak_rom;
    if (!args.tpak_save.empty()) spec.save_files["tpak1"] = args.tpak_save;
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
    const bool toggle =
        (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) ||
        (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
         (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_F1));
    if (!toggle || link_.state() != corelink::LinkState::Ready) return false;
    set_paused(!menu_open_);
    return true;
}

void PlaySession::set_paused(bool paused) {
    menu_open_ = paused;
    // Offline the quick menu pauses (HOST_LIFECYCLE.md): no grants, no sound.
    if (audio_) {
        if (paused) SDL_PauseAudioStreamDevice(audio_);
        else SDL_ResumeAudioStreamDevice(audio_);
    }
    next_grant_ns_ = 0;
}

void PlaySession::fill_pads(rcore_pad pads[RCORE_MAX_SEATS]) const {
    // The platform's seats: which device each port reads, and its map.
    fill_pads_from_input(args_.input, pads, kSeats);
}

void PlaySession::grant_if_due() {
    if (menu_open_ || !link_.can_grant()) return;
    const std::uint64_t now = SDL_GetTicksNS();
    bool due;
    if (audio_ && audio_hz_) {
        // The core's audio clock paces it: grant while the queue is short.
        const int queued = SDL_GetAudioStreamQueued(audio_);
        const double queued_ms = queued / (double(audio_hz_) * 4.0) * 1000.0;
        due = queued_ms < kTargetQueuedMs;
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
    rcore_pad pads[RCORE_MAX_SEATS];
    fill_pads(pads);
    link_.grant(pads);
}

void PlaySession::pump_audio() {
    const std::uint32_t hz = link_.audio_rate();
    if (hz && hz != audio_hz_ && SDL_WasInit(SDL_INIT_AUDIO)) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(hz)};
        if (!audio_) {
            audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr,
                                               nullptr);
            if (audio_ && !menu_open_) SDL_ResumeAudioStreamDevice(audio_);
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
        if (audio_) {
            SDL_PutAudioStreamData(audio_, audio_buf_.data(), static_cast<int>(n * 4));
        }
    }
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
    if (link_.state() == corelink::LinkState::Ended) {
        if (audio_) SDL_PauseAudioStreamDevice(audio_);
        return;
    }
    pump_audio();
    upload_frame();
    grant_if_due();
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
        const ImU32 tint = menu_open_ ? IM_COL32(110, 110, 110, 255) : IM_COL32_WHITE;
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
            if (menu_open_) draw_menu();
            break;
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
    if (ImGui::Button("Close game", ImVec2(220, 0))) {
        user_quit_ = true;
        link_.stop();
    }
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
    if (link_.state() != corelink::LinkState::Idle) link_.stop();
    if (audio_) {
        SDL_DestroyAudioStream(audio_);
        audio_ = nullptr;
    }
    if (tex_) {
        glDeleteTextures(1, &tex_);
        tex_ = 0;
    }
}

} // namespace retcomm::hub
