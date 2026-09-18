#include "hub/hub_osk.hpp"

#include <SDL3/SDL.h>

// Added in SDL 3.4.12; build hosts on an older SDL3 have every other symbol
// here (all 3.2.0 baseline) but not this one. Hints are looked up by name at
// runtime, so declaring it ourselves keeps the Steam path compiled in and it
// starts working the moment the binary meets a runtime new enough to read it —
// on an older runtime setting an unknown hint is a no-op.
#ifndef SDL_HINT_ENABLE_STEAM_SCREEN_KEYBOARD
#define SDL_HINT_ENABLE_STEAM_SCREEN_KEYBOARD "SDL_ENABLE_STEAM_SCREEN_KEYBOARD"
#endif

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace retcomm::hub {
namespace {

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

// Steam sets these for anything it launches; SteamDeck/SteamOS mark the device
// itself. Any of them means Steam's keyboard is the right one to ask for.
bool running_under_steam() {
    for (const char* n : {"SteamDeck", "SteamOS", "SteamGameId", "SteamAppId",
                          "SteamOverlayGameId", "SteamClientLaunch", "SteamEnv"})
        if (env_set(n)) return true;
#if !defined(_WIN32) && !defined(__APPLE__)
    std::ifstream os_release("/etc/os-release");
    for (std::string line; std::getline(os_release, line);)
        if (line == "ID=steamos" || line == "ID=\"steamos\"") return true;
#endif
    return false;
}

#if !defined(_WIN32) && !defined(__APPLE__)
// Desktop Linux has no OSK of its own, so we start one. Alex-supplied first:
// RETCOMM_OSK_COMMAND names an executable to run while a field is open.
const char* const kLinuxOskCandidates[] = {
    "wvkbd-mobintl",     // wlroots layer-shell keyboard
    "squeekboard",       // GNOME/Phosh
    "onboard",           // GTK, X11 and XWayland
    "matchbox-keyboard",
    "florence",
    "corekeyboard",
    "xvkbd",
};

fs::path which_in_path(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path) return {};
    std::string p(path);
    size_t start = 0;
    while (start <= p.size()) {
        const size_t sep = p.find(':', start);
        const std::string dir = p.substr(start, sep == std::string::npos ? std::string::npos
                                                                         : sep - start);
        if (!dir.empty()) {
            const fs::path cand = fs::path(dir) / name;
            std::error_code ec;
            if (fs::is_regular_file(cand, ec) && ::access(cand.c_str(), X_OK) == 0) return cand;
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return {};
}
#endif  // !_WIN32 && !__APPLE__

#if !defined(_WIN32)
pid_t g_osk_pid = 0;

void spawn_osk(const fs::path& exe) {
    const pid_t pid = ::fork();
    if (pid < 0) return;
    if (pid == 0) {
        // Own session, so closing the hub cannot leave the keyboard attached to
        // our terminal, but still our child so hide() can stop it.
        ::setsid();
        ::execl(exe.c_str(), exe.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    g_osk_pid = pid;
}

void kill_osk() {
    if (g_osk_pid <= 0) return;
    // setsid() made it a group leader, so signal the group: a keyboard started
    // through a wrapper script leaves the real process behind otherwise.
    if (::kill(-g_osk_pid, SIGTERM) != 0) ::kill(g_osk_pid, SIGTERM);
    int status = 0;
    ::waitpid(g_osk_pid, &status, 0);
    g_osk_pid = 0;
}
#endif  // !_WIN32

struct State {
    bool probed = false;
    OskBackend backend = OskBackend::None;
    std::string reason;
    fs::path spawn_exe;   // the OSK program we would start, when backend==Spawned
    bool armed = false;   // a pad opened the field, so a keyboard is wanted
    bool shown = false;   // want_text_input was true last sync
    bool warned = false;  // "no keyboard available" said once, not every field
};

State& state() {
    static State s;
    return s;
}

void probe() {
    State& s = state();
    if (s.probed) return;
    s.probed = true;

    // SDL raises Steam's keyboard and the platform's own touch keyboard itself,
    // from inside SDL_StartTextInput() — there is nothing for us to launch.
    if (SDL_HasScreenKeyboardSupport()) {
        s.backend = running_under_steam() ? OskBackend::Steam : OskBackend::Platform;
        return;
    }
#if defined(_WIN32)
    // Pre-Windows-8 hosts, or a build of SDL without the input pane: the touch
    // keyboard still ships as TabTip.exe.
    wchar_t common[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"CommonProgramFiles", common, MAX_PATH)) {
        const fs::path tab = fs::path(common) / L"microsoft shared" / L"ink" / L"TabTip.exe";
        std::error_code ec;
        if (fs::is_regular_file(tab, ec)) {
            s.backend = OskBackend::Spawned;
            s.spawn_exe = tab;
            return;
        }
    }
    s.reason = "Windows touch keyboard (TabTip.exe) not found";
#elif defined(__APPLE__)
    s.reason = "macOS has no on-screen keyboard an app can raise";
#else
    if (const char* custom = std::getenv("RETCOMM_OSK_COMMAND"); custom && *custom) {
        fs::path exe = fs::path(custom).is_absolute() ? fs::path(custom) : which_in_path(custom);
        std::error_code ec;
        if (!exe.empty() && fs::is_regular_file(exe, ec)) {
            s.backend = OskBackend::Spawned;
            s.spawn_exe = exe;
            return;
        }
        s.reason = std::string("RETCOMM_OSK_COMMAND=") + custom + " is not an executable";
        return;
    }
    for (const char* cand : kLinuxOskCandidates) {
        const fs::path exe = which_in_path(cand);
        if (!exe.empty()) {
            s.backend = OskBackend::Spawned;
            s.spawn_exe = exe;
            return;
        }
    }
    s.reason =
        "no on-screen keyboard installed (tried wvkbd-mobintl, squeekboard, onboard, "
        "matchbox-keyboard, florence, corekeyboard, xvkbd); set RETCOMM_OSK_COMMAND to name one";
#endif
}

}  // namespace

const char* osk_backend_name(OskBackend b) {
    switch (b) {
        case OskBackend::Steam: return "Steam";
        case OskBackend::Platform: return "platform";
        case OskBackend::Spawned: return "spawned";
        case OskBackend::None: break;
    }
    return "none";
}

void osk_configure_hints() {
    // Steam sets this itself for Big Picture launches; setting it on a Deck or
    // SteamOS session that started us some other way costs nothing and is the
    // only way SDL will use the steam:// keyboard.
    if (running_under_steam() && !std::getenv("SDL_ENABLE_STEAM_SCREEN_KEYBOARD"))
        SDL_SetHint(SDL_HINT_ENABLE_STEAM_SCREEN_KEYBOARD, "1");
}

OskBackend osk_backend() {
    probe();
    return state().backend;
}

const std::string& osk_unavailable_reason() {
    probe();
    return state().reason;
}

void osk_arm_for_pad() {
    probe();
    State& s = state();
    s.armed = true;
    // SDL's default is "auto" — show only when no physical keyboard is attached.
    // A pad in hand is the same situation even with a keyboard on the desk, and
    // the hint is only read inside SDL_StartTextInput(), which ImGui calls at
    // the end of the frame the field activates on, i.e. after this.
    SDL_SetHintWithPriority(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1", SDL_HINT_OVERRIDE);
}

std::string osk_sync(SDL_Window* window, bool want_text_input) {
    probe();
    State& s = state();
    if (want_text_input == s.shown) return {};
    s.shown = want_text_input;

    if (!want_text_input) {
#if !defined(_WIN32)
        kill_osk();
#endif
        s.armed = false;
        SDL_ResetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD);
        return {};
    }
    if (!s.armed) return {};  // opened with a mouse or a real keyboard

    switch (s.backend) {
        case OskBackend::Steam:
        case OskBackend::Platform:
            // SDL_StartTextInput() already raised it. Report only if it didn't.
            if (window && !SDL_ScreenKeyboardShown(window))
                return std::string("On-screen keyboard (") + osk_backend_name(s.backend) +
                       ") did not open: " + SDL_GetError();
            return {};
        case OskBackend::Spawned:
#if defined(_WIN32)
            ShellExecuteW(nullptr, L"open", s.spawn_exe.wstring().c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
#else
            spawn_osk(s.spawn_exe);
#endif
            return {};
        case OskBackend::None:
            if (s.warned) return {};
            s.warned = true;
            return "No on-screen keyboard available: " + s.reason;
    }
    return {};
}

}  // namespace retcomm::hub
