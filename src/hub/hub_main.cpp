#include "hub/hub_boxart.hpp"
#include "hub/hub_model.hpp"
#include "hub/hub_osk.hpp"
#if defined(RETCOMM_HUB_HAVE_PLAY)
#include "hub/hub_play.hpp"
#endif
#include "hub/hub_theme.hpp"

#include "retcomm/catalog_sync.hpp"
#include "retcomm/config.hpp"
#include "retcomm/http.hpp"
#include "retcomm/paths.hpp"
#include "retcomm/platform_settings.hpp"
#include "retcomm/psx_input_profiles.hpp"
#include "retcomm/snes_platform_settings.hpp"
#include "retcomm/romm_saves.hpp"
#include "retcomm/mods.hpp"
#include "retcomm/self_update.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#if defined(IMGUI_ENABLE_FREETYPE)
#include "misc/freetype/imgui_freetype.h"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_opengl.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <set>
#include <unordered_set>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Resolve hub assets (fonts, platform icons). Prefer packaged locations, then
// walk up from the app base path for a source-tree assets/<kind> (local builds).
fs::path find_hub_asset_file(const char* kind, const char* filename) {
    std::error_code ec;
    auto try_file = [&](const fs::path& p) -> fs::path {
        if (p.empty()) return {};
        const fs::path c = fs::weakly_canonical(p, ec);
        const fs::path& use = (!ec && !c.empty()) ? c : p;
        if (fs::is_regular_file(use, ec)) return use;
        return {};
    };

    std::vector<fs::path> dirs;
    // AppImage runtime sets APPDIR to the mounted squashfs root.
    if (const char* appdir = std::getenv("APPDIR")) {
        const fs::path ad(appdir);
        dirs.push_back(ad / "usr" / "share" / "retcomm" / kind);
        dirs.push_back(ad / "usr" / "bin" / kind);
        dirs.push_back(ad / kind);
    }
    // SDL3: cached string — do not free. Often …/usr/bin/ inside an AppImage.
    if (const char* base = SDL_GetBasePath()) {
        fs::path b(base);
        dirs.push_back(b);
        dirs.push_back(b / kind);
        dirs.push_back(b / ".." / "share" / "retcomm" / kind);
        dirs.push_back(b / ".." / "Resources" / kind);
        dirs.push_back(b / ".." / ".." / "Resources" / kind);
        // Local cmake: build/ → ../assets/<kind>
        fs::path walk = b;
        for (int i = 0; i < 6 && !walk.empty(); ++i) {
            dirs.push_back(walk / "assets" / kind);
            dirs.push_back(walk / kind);
            dirs.push_back(walk / "share" / "retcomm" / kind);
            walk = walk.parent_path();
        }
    }

    for (const auto& dir : dirs) {
        if (auto hit = try_file(dir / filename); !hit.empty()) return hit;
    }
    return {};
}

fs::path find_hub_font_file(const char* filename) {
    return find_hub_asset_file("fonts", filename);
}

// Fill a fixed-size UI text buffer, always NUL-terminated.
void copy_buf(char* dest, size_t dest_n, const std::string& src) {
    if (!dest || dest_n == 0) return;
    const size_t n = std::min(src.size(), dest_n - 1);
    std::memcpy(dest, src.data(), n);
    dest[n] = '\0';
}

std::string human_bytes(std::uintmax_t n) {
    const char* unit[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(n);
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        ++i;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), (i == 0 ? "%.0f %s" : "%.1f %s"), v, unit[i]);
    return buf;
}

// Catalog platform slug → human-readable label for library cards.
const char* platform_display_name(const std::string& slug) {
    if (slug.empty()) return "Library";
    if (slug == "psx" || slug == "ps1" || slug == "ps") return "PlayStation";
    if (slug == "snes") return "Super Nintendo";
    if (slug == "gba") return "Game Boy Advance";
    if (slug == "n64") return "Nintendo 64";
    if (slug == "genesis" || slug == "md" || slug == "megadrive") return "Genesis / Mega Drive";
    if (slug == "gb" || slug == "dmg") return "Game Boy";
    if (slug == "gbc") return "Game Boy Color";
    if (slug == "nds") return "Nintendo DS";
    if (slug == "psp") return "PlayStation Portable";
    return slug.c_str();
}

// Resolve assets/platforms/<slug>.png (falls back to all.png).
fs::path platform_icon_path(const std::string& slug) {
    std::string key = slug.empty() ? "all" : slug;
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    fs::path hit = find_hub_asset_file("platforms", (key + ".png").c_str());
    if (!hit.empty()) return hit;
    if (key == "ps1" || key == "ps") {
        hit = find_hub_asset_file("platforms", "psx.png");
        if (!hit.empty()) return hit;
    }
    if (key == "md" || key == "megadrive") {
        hit = find_hub_asset_file("platforms", "genesis.png");
        if (!hit.empty()) return hit;
    }
    return find_hub_asset_file("platforms", "all.png");
}

// Load Lato like recomp-ui (18px body, oversample 2). Falls back to ImGui default.
// Merge symbols + (when FreeType is enabled) CBDT color emoji (Noto Color Emoji).
// `density` is the display scale (see UiScale below): glyphs are rasterized that
// much larger while keeping their 18-unit logical size, so text stays sharp on a
// scaled display instead of being a magnified 18px bitmap.
void load_hub_fonts(float density) {
    ImGuiIO& io = ImGui::GetIO();
#if defined(IMGUI_ENABLE_FREETYPE)
    // Color-layered glyphs (CBDT/CBLC) for Noto Color Emoji / Segoe UI Emoji.
    io.Fonts->FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_LoadColor;
#endif
    const fs::path regular = find_hub_font_file("LatoLatin-Regular.ttf");
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.RasterizerDensity = density;
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin-1
        0x2010, 0x2027, // dashes, curly quotes, ellipsis
        0,
    };
    constexpr float kBody = 18.0f;
    bool loaded = false;
    if (!regular.empty()) {
        loaded = io.Fonts->AddFontFromFileTTF(regular.string().c_str(), kBody, &cfg, kRanges) !=
                 nullptr;
        if (loaded) {
            std::fprintf(stderr, "retro-hub: loaded UI font %s\n", regular.string().c_str());
        } else {
            std::fprintf(stderr, "retro-hub: failed to load UI font %s\n",
                         regular.string().c_str());
        }
    }
    if (!loaded) {
        const char* appdir = std::getenv("APPDIR");
        const char* base = SDL_GetBasePath();
        std::fprintf(stderr,
                     "retro-hub: using ImGui default font (LatoLatin-Regular.ttf not found; "
                     "APPDIR=%s SDL_GetBasePath=%s)\n",
                     appdir ? appdir : "(null)", base ? base : "(null)");
        cfg.SizePixels = kBody;
        io.Fonts->AddFontDefault(&cfg);
    }

    auto merge_font_if_present = [&](const char* path, const ImWchar* ranges,
                                     bool color_emoji) -> bool {
        if (!path || !*path || !ranges) return false;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return false;
        ImFontConfig merge_cfg;
        merge_cfg.MergeMode = true;
        merge_cfg.PixelSnapH = true;
        // Color emoji bitmaps ignore oversampling; keep outline fonts crisp.
        merge_cfg.OversampleH = color_emoji ? 1 : 2;
        merge_cfg.OversampleV = color_emoji ? 1 : 2;
        merge_cfg.RasterizerDensity = density;
#if defined(IMGUI_ENABLE_FREETYPE)
        if (color_emoji) merge_cfg.FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_LoadColor;
#else
        if (color_emoji) return false;
#endif
        if (!io.Fonts->AddFontFromFileTTF(path, kBody, &merge_cfg, ranges)) return false;
        std::fprintf(stderr, "retro-hub: merged %s font %s\n",
                     color_emoji ? "color-emoji" : "symbol/emoji", path);
        return true;
    };
    static const ImWchar kSymbolRanges[] = {
        0x2000, 0x206F, // General Punctuation
        0x2190, 0x21FF, // Arrows
        0x2300, 0x23FF, // Misc Technical
        0x2460, 0x24FF, // Enclosed Alphanumerics
        0x25A0, 0x25FF, // Geometric Shapes
        0x2600, 0x26FF, // Misc Symbols (⚠)
        0x2700, 0x27BF, // Dingbats
        0x2B00, 0x2BFF, // Misc Symbols and Arrows
        0xFE00, 0xFE0F, // Variation Selectors
        0,
    };
    const char* kSymbolCandidates[] = {
        "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansSymbols-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\seguisym.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Apple Symbols.ttf",
        "/System/Library/Fonts/Supplemental/Apple Symbols.ttf",
#endif
        nullptr,
    };
    bool merged_symbols = false;
    for (int i = 0; kSymbolCandidates[i]; ++i) {
        if (merge_font_if_present(kSymbolCandidates[i], kSymbolRanges, false)) {
            merged_symbols = true;
            break;
        }
    }
#ifdef IMGUI_USE_WCHAR32
    static const ImWchar kEmojiRanges[] = {
        0x2600, 0x26FF,   // Misc Symbols (color ⚠ when available)
        0x2700, 0x27BF,   // Dingbats
        0x1F300, 0x1F5FF, // Misc Symbols and Pictographs
        0x1F600, 0x1F64F, // Emoticons
        0x1F680, 0x1F6FF, // Transport and Map
        0x1F900, 0x1F9FF, // Supplemental Symbols and Pictographs
        0,
    };
    const char* kColorEmojiCandidates[] = {
#if defined(IMGUI_ENABLE_FREETYPE)
        "/usr/share/fonts/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto-color-emoji/NotoColorEmoji.ttf",
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\seguiemj.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Apple Color Emoji.ttc",
#endif
#endif
        nullptr,
    };
    const char* kOutlineEmojiCandidates[] = {
        "/usr/share/fonts/noto/NotoEmoji-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf",
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\seguiemj.ttf",
#endif
        nullptr,
    };
    bool merged_emoji = false;
    for (int i = 0; kColorEmojiCandidates[i]; ++i) {
        if (merge_font_if_present(kColorEmojiCandidates[i], kEmojiRanges, true)) {
            merged_emoji = true;
            break;
        }
    }
    if (!merged_emoji) {
        for (int i = 0; kOutlineEmojiCandidates[i]; ++i) {
            if (merge_font_if_present(kOutlineEmojiCandidates[i], kEmojiRanges, false)) {
                merged_emoji = true;
                break;
            }
        }
    }
    if (!merged_emoji) {
#if defined(IMGUI_ENABLE_FREETYPE)
        std::fprintf(stderr, "retro-hub: no color/outline emoji font found\n");
#else
        std::fprintf(stderr,
                     "retro-hub: no outline emoji font found (rebuild with FreeType for "
                     "color emoji)\n");
#endif
    }
#endif
    if (!merged_symbols) {
        std::fprintf(stderr,
                     "retro-hub: no symbol fallback font found (⚠ may not render)\n");
    }

    io.FontGlobalScale = 1.0f;
}

// ---------------------------------------------------------------------------
// Display scaling
// ---------------------------------------------------------------------------
// SDL3 hands us a window measured in *pixels* on Windows: with "Scale and
// layout" at 150% a 1280x800 window is still 1280x800 physical pixels, so the
// 18px body font and every hard-coded padding in this file came out a third
// smaller than the rest of the desktop.
//
// Rather than multiply ~600 layout constants, the UI keeps drawing in logical
// units and the frame is stretched instead: io.DisplaySize becomes the pixel
// size divided by the scale, io.DisplayFramebufferScale carries the factor into
// the GL backend's viewport/scissor, and the atlas is rasterized at that density
// so text is genuinely sharper rather than a magnified bitmap. At scale 1.0
// nothing below changes a single value.
struct UiScale {
    // Pixels per logical unit — what the framebuffer and the font atlas use.
    float px = 1.f;
    // SDL screen coordinates per logical unit — what mouse events and window
    // sizes use. Equals `px` on Windows (screen coords are pixels there) and
    // 1.0 where SDL reports the density through the pixel size instead
    // (macOS retina, Wayland with SDL_WINDOW_HIGH_PIXEL_DENSITY).
    float coords = 1.f;

    bool identity() const { return px == 1.f && coords == 1.f; }
};

// Config / env preference, else the display's own scale. Auto never shrinks:
// a display reporting under 100% still draws 1:1.
float resolve_ui_scale_px(SDL_Window* window, float pref) {
    float s = 0.f;
    if (const char* env = std::getenv("RETCOMM_UI_SCALE")) s = std::strtof(env, nullptr);
    if (!(s > 0.f)) s = pref;
    if (s > 0.f) return std::clamp(s, 0.5f, 4.f);
    if (window) s = SDL_GetWindowDisplayScale(window);
    if (!(s > 0.f)) s = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (!(s > 0.f)) return 1.f;
    return std::clamp(s, 1.f, 4.f);
}

UiScale resolve_ui_scale(SDL_Window* window, float pref) {
    UiScale out;
    out.px = resolve_ui_scale_px(window, pref);
    // display scale = pixel density * content scale, so the pixel density is
    // exactly the part SDL already applied to the window's pixel size.
    float density = window ? SDL_GetWindowPixelDensity(window) : 1.f;
    if (!(density > 0.f)) density = 1.f;
    out.coords = std::max(out.px / density, 0.05f);
    return out;
}

// Re-rasterize the atlas after the scale changed (monitor swap, settings edit).
// Only safe between frames.
void rebuild_hub_fonts(float density) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    io.Fonts->Clear();
    load_hub_fonts(density);
    io.Fonts->Build();
    ImGui_ImplOpenGL3_CreateFontsTexture();
}

// Mouse input reaches ImGui in window coordinates; move it into logical units.
// The backend reads only the fields touched here, and the hub's own handlers
// (gamepad/key bind capture) never look at mouse coordinates.
void scale_mouse_event(SDL_Event& e, float coords) {
    if (coords == 1.f) return;
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
        e.motion.x /= coords;
        e.motion.y /= coords;
        e.motion.xrel /= coords;
        e.motion.yrel /= coords;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        e.button.x /= coords;
        e.button.y /= coords;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        e.wheel.mouse_x /= coords;
        e.wheel.mouse_y /= coords;
        break;
    default:
        break;
    }
}

// True where ImGui_ImplSDL3 re-asserts the cursor from the global mouse state
// every frame (same driver whitelist it uses). Elsewhere — Wayland — there is
// no global cursor and motion events are the only source.
bool sdl_has_global_mouse() {
    const char* drv = SDL_GetCurrentVideoDriver();
    if (!drv) return false;
    static const char* kWhitelist[] = {"windows", "cocoa", "x11", "DIVE", "VMAN"};
    for (const char* w : kWhitelist) {
        if (std::strncmp(drv, w, std::strlen(w)) == 0) return true;
    }
    return false;
}

// Install the logical coordinate space for the frame about to be built. Runs
// after ImGui_ImplSDL3_NewFrame(), which fills io.DisplaySize with the window's
// pixel size and queues an unscaled cursor position; ours is queued last, so it
// is the one ImGui::NewFrame() ends up applying.
void apply_ui_scale_frame(SDL_Window* window, const UiScale& s) {
    if (s.identity()) return;
    ImGuiIO& io = ImGui::GetIO();
    int px_w = 0, px_h = 0;
    SDL_GetWindowSizeInPixels(window, &px_w, &px_h);
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) px_w = px_h = 0;
    io.DisplaySize = ImVec2(static_cast<float>(px_w) / s.px, static_cast<float>(px_h) / s.px);
    io.DisplayFramebufferScale = ImVec2(s.px, s.px);

    if (sdl_has_global_mouse() && SDL_GetKeyboardFocus() == window) {
        float gx = 0.f, gy = 0.f;
        int wx = 0, wy = 0;
        SDL_GetGlobalMouseState(&gx, &gy);
        SDL_GetWindowPosition(window, &wx, &wy);
        io.AddMousePosEvent((gx - static_cast<float>(wx)) / s.coords,
                            (gy - static_cast<float>(wy)) / s.coords);
    }
}

// Size and place the window for the resolved scale, keeping every part the user
// has to grab — title bar, borders — inside the desktop's work area.
//
// The 1280x800 this UI is written against is a LOGICAL size, so at 150% it asks
// for 1920x1200 pixels and at 300% for 3840x2400: on most monitors that does not
// fit. The old code clamped those numbers to the usable bounds and centred the
// result, which failed twice over. SDL sizes and positions the CLIENT area, so
// the caption lives above y and was never paid for; and SDL_WINDOWPOS_CENTERED
// centres on the DISPLAY bounds, not the usable bounds the size was clamped to,
// so a window as tall as the work area was pushed up by half the taskbar's
// height and its title bar ended up off the top of the screen — unreachable,
// with no grabbable edges either, because it already spanned the work area.
void fit_window_to_display(SDL_Window* window, const UiScale& s) {
    const int want_w = static_cast<int>(std::lround(1280.f * s.coords));
    const int want_h = static_cast<int>(std::lround(800.f * s.coords));

    SDL_Rect usable{};
    const SDL_DisplayID did = SDL_GetDisplayForWindow(window);
    if (!did || !SDL_GetDisplayUsableBounds(did, &usable) || usable.w <= 0 || usable.h <= 0) {
        // No work area to reason about: honour the scale and let the window
        // manager place it, which is what this did before any of the above.
        if (s.coords != 1.f) {
            SDL_SetWindowSize(window, want_w, want_h);
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
        return;
    }

    // Frame extents come from the live window, so they are already in the
    // display's own scale. Platforms without server-side decorations report
    // nothing and leave these at 0, where the margin alone is the slack.
    int frame_t = 0, frame_l = 0, frame_b = 0, frame_r = 0;
    SDL_GetWindowBordersSize(window, &frame_t, &frame_l, &frame_b, &frame_r);
    const int margin = static_cast<int>(std::lround(16.f * s.coords));

    // Centre the whole FRAME in the work area, never letting the caption cross
    // the top edge. Runs before a maximize too, so Restore lands somewhere sane.
    auto place = [&](int w, int h) {
        SDL_SetWindowSize(window, w, h);
        int x = usable.x + frame_l + (usable.w - (w + frame_l + frame_r)) / 2;
        int y = usable.y + frame_t + (usable.h - (h + frame_t + frame_b)) / 2;
        if (x < usable.x + frame_l) x = usable.x + frame_l;
        if (y < usable.y + frame_t) y = usable.y + frame_t;
        SDL_SetWindowPosition(window, x, y);
    };

    const int max_w = usable.w - frame_l - frame_r - margin * 2;
    const int max_h = usable.h - frame_t - frame_b - margin * 2;
    if (max_w <= 0 || max_h <= 0) {   // a work area smaller than its own chrome
        place(want_w, want_h);
        return;
    }

    if (want_w > max_w || want_h > max_h) {
        // Too big to place by hand. Maximizing is the honest version of what the
        // clamp was reaching for: the OS owns the geometry, the title bar is
        // always reachable, snapping works, and Restore gives back the size set
        // here. This is also the 100% path on a small laptop panel, where an
        // 800-tall window never fit a 768-tall screen and nothing clamped it.
        place(std::min(want_w, max_w), std::min(want_h, max_h));
        SDL_MaximizeWindow(window);
        return;
    }
    place(want_w, want_h);
}

using retcomm::hub::BoxartCache;
using retcomm::hub::BoxartTexture;
using retcomm::hub::FolderPickTarget;
using retcomm::hub::SetupPath;
using retcomm::hub::HubJob;
using retcomm::hub::HubModel;
using retcomm::hub::Theme;
using retcomm::hub::TitleRow;

void SDLCALL on_folder_dialog(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* hub = static_cast<HubModel*>(userdata);
    if (!hub) return;
    std::lock_guard<std::mutex> lock(hub->folder_pick_mu);
    hub->folder_pick_busy = false;
    if (!filelist) {
        hub->folder_pick_target = FolderPickTarget::None;
        hub->folder_pick_path.clear();
        return; // error
    }
    if (!filelist[0]) {
        hub->folder_pick_target = FolderPickTarget::None;
        hub->folder_pick_path.clear();
        return; // canceled
    }
    hub->folder_pick_path = filelist[0];
}

void begin_folder_pick(HubModel& hub, SDL_Window* window, FolderPickTarget target,
                       const char* current_path) {
    {
        std::lock_guard<std::mutex> lock(hub.folder_pick_mu);
        if (hub.folder_pick_busy) return;
        hub.folder_pick_busy = true;
        hub.folder_pick_target = target;
        hub.folder_pick_path.clear();
    }
    const char* start = nullptr;
    if (current_path && current_path[0] != '\0') start = current_path;
    SDL_ShowOpenFolderDialog(on_folder_dialog, &hub, window, start, false);
}

void SDLCALL on_file_dialog(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* hub = static_cast<HubModel*>(userdata);
    if (!hub) return;
    std::lock_guard<std::mutex> lock(hub->file_pick_mu);
    hub->file_pick_busy = false;
    hub->file_pick_paths.clear();
    if (!filelist || !filelist[0]) {
        hub->file_pick_kind = retcomm::hub::FilePickKind::None;
        hub->file_pick_platform.clear();
        hub->file_pick_title_id.clear();
        return;
    }
    for (const char* const* p = filelist; *p; ++p) hub->file_pick_paths.emplace_back(*p);
}

std::string strip_dot_ext(std::string e) {
    if (!e.empty() && e.front() == '.') e.erase(e.begin());
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string join_ext_pattern(const std::set<std::string>& exts) {
    std::string pat;
    for (const auto& e : exts) {
        if (e.empty() || e == "*") continue;
        if (!pat.empty()) pat.push_back(';');
        pat += e;
    }
    return pat;
}

std::set<std::string> rom_exts_for_platform(const retcomm::Catalog& catalog,
                                            const std::string& platform) {
    const bool is_psx = platform == "psx" || platform == "ps1" || platform == "ps";
    std::set<std::string> exts;
    for (const auto& t : catalog.titles) {
        const bool t_is_psx =
            t.platform == "psx" || t.platform == "ps1" || t.platform == "ps";
        if (is_psx ? !t_is_psx : t.platform != platform) continue;
        for (const auto& e : t.rom_extensions) {
            const std::string s = strip_dot_ext(e);
            if (s.empty()) continue;
            // PSX: no cooked ISO / CHD ever (no reliable multi-track TOC).
            if (is_psx && (s == "iso" || s == "chd")) continue;
            exts.insert(s);
        }
    }
    if (is_psx) {
        // Library baseline: cue sheets + bin tracks; catalog titles may add
        // self-contained official images (e.g. Tomba!'s Steam .car payload).
        exts.insert("cue");
        exts.insert("bin");
        return exts;
    }
    if (!exts.empty()) return exts;
    // Fallbacks aligned with Retro-Catalog platform-defaults.json
    if (platform == "snes") return {"sfc", "smc", "fig", "swc"};
    if (platform == "gba") return {"gba"};
    if (platform == "n64") return {"z64", "n64", "v64"};
    if (platform == "genesis" || platform == "md" || platform == "megadrive")
        return {"bin", "md", "gen", "smd"};
    return {"bin", "rom", "iso", "cue"};
}

std::set<std::string> save_exts_for_platform(const retcomm::Catalog& catalog,
                                             const std::string& platform) {
    if (platform == "psx" || platform == "ps1" || platform == "ps") return {"mcd"};
    std::set<std::string> exts;
    auto add_glob = [&](const std::string& g) {
        const auto dot = g.find_last_of('.');
        if (dot == std::string::npos) return;
        const std::string s = strip_dot_ext(g.substr(dot));
        if (!s.empty()) exts.insert(s);
    };
    for (const auto& t : catalog.titles) {
        if (t.platform != platform) continue;
        for (const auto& g : t.saves_memcard_glob) add_glob(g);
        for (const auto& g : t.saves_sram_glob) add_glob(g);
    }
    if (!exts.empty()) return exts;
    if (platform == "gba") return {"sav"};
    return {"srm", "sav", "mcd", "mcr"};
}

std::set<std::string> bios_exts_for_platform(const std::string& platform) {
    if (platform == "psx" || platform == "ps1" || platform == "ps") return {"bin"};
    return {"bin", "rom", "bios", "img"};
}

void begin_file_pick(HubModel& hub, SDL_Window* window, retcomm::hub::FilePickKind kind,
                     const std::string& platform, const std::string& filter_name,
                     const std::set<std::string>& exts, bool allow_many,
                     const std::string& title_id = {}) {
    const std::string pattern = join_ext_pattern(exts);
    if (pattern.empty()) return;
    {
        std::lock_guard<std::mutex> lock(hub.file_pick_mu);
        if (hub.file_pick_busy) return;
        hub.file_pick_busy = true;
        hub.file_pick_kind = kind;
        hub.file_pick_platform = platform;
        hub.file_pick_title_id = title_id;
        hub.file_pick_paths.clear();
        hub.file_pick_filter_name = filter_name;
        hub.file_pick_filter_pattern = pattern;
    }
    // Pointers must remain valid until the callback runs.
    static SDL_DialogFileFilter filters[1];
    filters[0].name = hub.file_pick_filter_name.c_str();
    filters[0].pattern = hub.file_pick_filter_pattern.c_str();
    SDL_ShowOpenFileDialog(on_file_dialog, &hub, window, filters, 1, nullptr, allow_many);
}

void begin_export_activity_log(HubModel& hub, SDL_Window* window) {
    {
        std::lock_guard<std::mutex> lock(hub.file_pick_mu);
        if (hub.file_pick_busy) return;
        hub.file_pick_busy = true;
        hub.file_pick_kind = retcomm::hub::FilePickKind::ExportActivityLog;
        hub.file_pick_platform.clear();
        hub.file_pick_title_id.clear();
        hub.file_pick_paths.clear();
        hub.file_pick_filter_name = "Log files";
        hub.file_pick_filter_pattern = "log";
        hub.file_pick_default_location =
            (retcomm::user_home_dir() / "retcomm-launcher.log").string();
    }
    static SDL_DialogFileFilter filters[1];
    filters[0].name = hub.file_pick_filter_name.c_str();
    filters[0].pattern = hub.file_pick_filter_pattern.c_str();
    SDL_ShowSaveFileDialog(on_file_dialog, &hub, window, filters, 1,
                           hub.file_pick_default_location.c_str());
}

// ImGui opens a text field for editing only when the activation carries
// ImGuiActivateFlags_PreferInput. A pad produces that on the north face button
// alone: A is a "tweak", and tweaking a text field does nothing, so on a
// controller the field simply refuses to open. A is the button a player will
// press on a controller-first shell, so when the ring is on a text field and A
// goes down, re-issue the activation as an input one and tell the on-screen
// keyboard it is wanted. Must be called immediately after the widget, while it
// is still the last item.
void pad_a_opens_text_field() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.NavInputSource != ImGuiInputSource_Gamepad) return;
    const ImGuiID id = ImGui::GetItemID();
    if (id == 0 || g.ActiveId == id || !ImGui::IsItemFocused()) return;
    if (!ImGui::IsKeyPressed(ImGuiKey_NavGamepadActivate, false)) return;
    // Queued rather than applied: NavUpdate has already run for this frame, so
    // the field picks it up at the top of the next one.
    g.NavNextActivateId = id;
    g.NavNextActivateFlags =
        ImGuiActivateFlags_PreferInput | ImGuiActivateFlags_TryToPreserveState;
    retcomm::hub::osk_arm_for_pad();
}

// Every text field in the hub goes through these so the pad rule above holds
// everywhere, rather than on whichever fields someone remembered.
bool hub_input_text(const char* id, char* buf, size_t buf_n, ImGuiInputTextFlags flags = 0) {
    const bool changed = ImGui::InputText(id, buf, buf_n, flags);
    pad_a_opens_text_field();
    return changed;
}

bool hub_input_text_hint(const char* id, const char* hint, char* buf, size_t buf_n,
                         ImGuiInputTextFlags flags = 0) {
    const bool changed = ImGui::InputTextWithHint(id, hint, buf, buf_n, flags);
    pad_a_opens_text_field();
    return changed;
}

// Path field + native Browse button. Returns true if the text field changed.
bool path_field_with_browse(const char* label, const char* input_id, char* buf, size_t buf_n,
                            HubModel& hub, SDL_Window* window, FolderPickTarget target,
                            const Theme& th) {
    ImGui::TextColored(th.text_muted, "%s", label);
    const float browse_w = 96.f;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float input_w = ImGui::GetContentRegionAvail().x - browse_w - gap;
    bool changed = false;
    if (input_w > 80.f) ImGui::SetNextItemWidth(input_w);
    changed = hub_input_text(input_id, buf, buf_n);
    ImGui::SameLine();
    bool busy = false;
    {
        std::lock_guard<std::mutex> lock(hub.folder_pick_mu);
        busy = hub.folder_pick_busy;
    }
    ImGui::BeginDisabled(busy);
    ImGui::PushID(input_id);
    if (ImGui::Button("Browse", ImVec2(browse_w, 0)))
        begin_folder_pick(hub, window, target, buf);
    ImGui::PopID();
    ImGui::EndDisabled();
    return changed;
}

bool title_has_rom_source(const TitleRow& r) { return r.has_rom || r.has_romm; }

// Filter Unsupported Titles: keep rows with a ROM source, or anything already installed.
bool title_passes_library_filter(const TitleRow& r, const HubModel& hub) {
    if (!hub.cfg.filter_unsupported_titles) return true;
    if (title_has_rom_source(r)) return true;
    if (r.installed || r.install_dir_present || r.has_preserved_state) return true;
    return false;
}

const char* chip_label(const TitleRow& r) {
    if (r.update_available) return "UPDATE";
    if (r.installed && r.runtime == "wine") return "WINE";
    if (r.installed) return "INSTALLED";
    if (r.install_dir_present) return "NEEDS SETUP";
    // Keep-saves uninstall: catalog/ROM state, not a broken install.
    if (r.has_rom) return "ROM READY";
    if (r.has_romm) return "ON ROMM";
    if (r.has_preserved_state) return "SAVES KEPT";
    return "CATALOG";
}

ImVec4 chip_color(const TitleRow& r, const Theme& th) {
    if (r.update_available) return th.warn;
    if (r.installed && r.runtime == "wine") return th.good;
    if (r.installed) return th.good;
    if (r.install_dir_present) return th.warn;
    if (r.has_rom) return th.focus;
    if (r.has_romm) return th.accent;
    if (r.has_preserved_state) return th.focus;
    return th.text_muted;
}

// Corner glyph on library boxart (drawn inside the status badge).
enum class TileStatusIcon : int {
    None = 0,
    Catalog,    // installable / catalog-only (no ROM gate)
    NoRom,      // local build needs a ROM; none matched
    RomReady,   // verified ROM on disk — ready to install
    OnRomm,     // RomM match (download) — same download glyph as RomReady
    Installed,  // checkmark
    NeedsSetup, // partial install
    Update,     // small up-chevron (center update overlay still shown)
    Queued,     // pause mark — waiting in Install/Update backlog
};

TileStatusIcon tile_status_icon(const TitleRow& r) {
    if (r.update_available) return TileStatusIcon::Update;
    if (r.installed) return TileStatusIcon::Installed;
    if (r.install_dir_present) return TileStatusIcon::NeedsSetup;
    if (r.has_rom) return TileStatusIcon::RomReady;
    if (r.has_romm) return TileStatusIcon::OnRomm;
    // Default Install builds whenever a local recipe exists (even if a zip is
    // also advertised) — surface the missing-ROM badge for those titles.
    if (r.supports_local_build) return TileStatusIcon::NoRom;
    return TileStatusIcon::Catalog;
}

// Glyph ink on the badge fill (dark on bright accents, light on muted).
ImU32 status_glyph_ink(const ImVec4& badge_col, const Theme& th) {
    const float lum = 0.2126f * badge_col.x + 0.7152f * badge_col.y + 0.0722f * badge_col.z;
    if (lum > 0.55f) return ImGui::ColorConvertFloat4ToU32(th.background);
    return IM_COL32(245, 247, 252, 255);
}

void draw_status_badge_glyph(ImDrawList* dl, const ImVec2& c, float rad, TileStatusIcon icon,
                             ImU32 ink) {
    switch (icon) {
    case TileStatusIcon::None:
        break;
    case TileStatusIcon::Installed: {
        // Checkmark.
        const ImVec2 a(c.x - rad * 0.45f, c.y + rad * 0.02f);
        const ImVec2 b(c.x - rad * 0.08f, c.y + rad * 0.38f);
        const ImVec2 d(c.x + rad * 0.48f, c.y - rad * 0.36f);
        dl->AddLine(a, b, ink, 2.1f);
        dl->AddLine(b, d, ink, 2.1f);
        break;
    }
    case TileStatusIcon::NeedsSetup: {
        // Exclamation.
        dl->AddLine(ImVec2(c.x, c.y - rad * 0.42f), ImVec2(c.x, c.y + rad * 0.08f), ink, 2.2f);
        dl->AddCircleFilled(ImVec2(c.x, c.y + rad * 0.38f), rad * 0.14f, ink, 8);
        break;
    }
    case TileStatusIcon::Update: {
        // Up chevron.
        const float h = rad * 0.42f;
        const float w = rad * 0.40f;
        dl->AddTriangleFilled(ImVec2(c.x, c.y - h), ImVec2(c.x - w, c.y + h * 0.35f),
                              ImVec2(c.x + w, c.y + h * 0.35f), ink);
        break;
    }
    case TileStatusIcon::Queued: {
        // Pause bars (download waiting in queue).
        const float bw = rad * 0.22f;
        const float bh = rad * 0.55f;
        const float gap = rad * 0.18f;
        dl->AddRectFilled(ImVec2(c.x - gap - bw, c.y - bh * 0.5f),
                          ImVec2(c.x - gap, c.y + bh * 0.5f), ink, 1.4f);
        dl->AddRectFilled(ImVec2(c.x + gap, c.y - bh * 0.5f),
                          ImVec2(c.x + gap + bw, c.y + bh * 0.5f), ink, 1.4f);
        break;
    }
    case TileStatusIcon::RomReady: {
        // Check in a circle — local ROM verified / ready to install.
        dl->AddCircle(c, rad * 0.55f, ink, 16, 1.7f);
        const ImVec2 a(c.x - rad * 0.32f, c.y + rad * 0.02f);
        const ImVec2 b(c.x - rad * 0.06f, c.y + rad * 0.30f);
        const ImVec2 d(c.x + rad * 0.38f, c.y - rad * 0.28f);
        dl->AddLine(a, b, ink, 1.9f);
        dl->AddLine(b, d, ink, 1.9f);
        break;
    }
    case TileStatusIcon::OnRomm: {
        // Download: arrow into tray.
        const float h = rad * 0.36f;
        const float w = rad * 0.34f;
        const ImVec2 tip(c.x, c.y + h * 0.55f);
        dl->AddTriangleFilled(tip, ImVec2(c.x - w, c.y - h * 0.15f),
                              ImVec2(c.x + w, c.y - h * 0.15f), ink);
        const float stem_w = w * 0.38f;
        dl->AddRectFilled(ImVec2(c.x - stem_w * 0.5f, c.y - h * 0.70f),
                          ImVec2(c.x + stem_w * 0.5f, c.y - h * 0.05f), ink, 1.2f);
        dl->AddLine(ImVec2(c.x - rad * 0.48f, c.y + rad * 0.55f),
                    ImVec2(c.x + rad * 0.48f, c.y + rad * 0.55f), ink, 1.8f);
        break;
    }
    case TileStatusIcon::NoRom: {
        // Slash-circle (unavailable).
        dl->AddCircle(c, rad * 0.55f, ink, 16, 1.8f);
        dl->AddLine(ImVec2(c.x - rad * 0.38f, c.y + rad * 0.38f),
                    ImVec2(c.x + rad * 0.38f, c.y - rad * 0.38f), ink, 1.8f);
        break;
    }
    case TileStatusIcon::Catalog: {
        // Soft disc / catalog mark.
        dl->AddCircle(c, rad * 0.48f, ink, 16, 1.7f);
        dl->AddCircleFilled(c, rad * 0.14f, ink, 10);
        break;
    }
    }
}

bool accent_button(const char* label, const Theme& th, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, th.accent_button);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accent_button_hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, th.accent_button_active);
    ImGui::PushStyleColor(ImGuiCol_Text, th.accent_text);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

// Play / success actions — muted green fill; bright th.good stays for status text.
bool good_button(const char* label, const Theme& th, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, th.good_button);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.good_button_hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, th.good_button_active);
    ImGui::PushStyleColor(ImGuiCol_Text, th.good_button_text);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

// Destructive actions — muted red fill (mirrors good_button contrast).
bool danger_button(const char* label, const Theme& /*th*/, const ImVec2& size = ImVec2(0, 0)) {
    const ImVec4 btn(0.561f, 0.165f, 0.200f, 1.f);       // #8F2A33
    const ImVec4 hovered(0.655f, 0.220f, 0.255f, 1.f);   // #A73841
    const ImVec4 active(0.455f, 0.130f, 0.165f, 1.f);    // #74212A
    const ImVec4 text(0.980f, 0.920f, 0.925f, 1.f);      // #FAEBEB
    ImGui::PushStyleColor(ImGuiCol_Button, btn);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

// RomM brand purple (docs.romm.app brand guidelines: #553e98 / #371f69).
// When inside BeginDisabled (e.g. RomM not configured), keep default grey chrome.
bool romm_button(const char* label, const Theme& /*th*/, const ImVec2& size = ImVec2(0, 0)) {
    const bool disabled =
        (GImGui != nullptr) && ((GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0);
    if (disabled) return ImGui::Button(label, size);

    const ImVec4 btn(0.333f, 0.243f, 0.596f, 1.f);         // #553E98
    const ImVec4 hovered(0.420f, 0.322f, 0.690f, 1.f);     // #6B52B0
    const ImVec4 active(0.216f, 0.122f, 0.412f, 1.f);      // #371F69
    const ImVec4 text(0.929f, 0.898f, 0.973f, 1.f);        // #EDE5F8
    ImGui::PushStyleColor(ImGuiCol_Button, btn);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

// Settings pages fill the window, so their Save / Cancel appear twice: in the
// page footer and in the header. One implementation of each keeps the two from
// ever disagreeing about what Cancel throws away.
void cancel_psx_bind_capture(HubModel& hub);
void cancel_snes_bind_capture(HubModel& hub);
void cancel_snes_gesture_capture(HubModel& hub);

bool any_settings_page_open(const HubModel& hub) {
    return hub.show_settings || hub.show_romm_settings || hub.show_psx_settings ||
           hub.show_snes_settings;
}

bool active_settings_dirty(const HubModel& hub) {
    if (hub.show_settings) return hub.settings.dirty;
    if (hub.show_romm_settings) return hub.romm_settings.dirty;
    if (hub.show_psx_settings) return hub.psx_settings.dirty;
    if (hub.show_snes_settings) return hub.snes_settings.dirty;
    return false;
}

// Write the open page to disk. False on failure — the caller must leave the
// page open then, or the edits vanish with no way back to them.
bool save_active_settings(HubModel& hub) {
    std::string err;
    const char* what = nullptr;
    bool ok = false;
    if (hub.show_settings) {
        what = "settings";
        ok = hub.save_settings(&err);
    } else if (hub.show_romm_settings) {
        what = "RomM settings";
        ok = hub.save_romm_settings(&err);
    } else if (hub.show_psx_settings) {
        what = "PlayStation settings";
        ok = hub.save_psx_settings(&err);
    } else if (hub.show_snes_settings) {
        what = "Super Nintendo settings";
        ok = hub.save_snes_settings(&err);
    } else {
        return false;
    }
    if (!ok) {
        hub.append_log(std::string(what) + " save failed: " + err);
        hub.set_status("Save failed");
        return false;
    }
    hub.show_toast("Saved!");
    return true;
}

// Hand the nav ring back to whichever body page is in front. Each page owns a
// separate child window, so the flag has to name the page rather than "content".
void request_page_focus(HubModel& hub) {
    switch (hub.library_nav) {
        case retcomm::hub::LibraryNav::Platforms: hub.home_focus_pending = true; break;
        case retcomm::hub::LibraryNav::Titles: hub.grid_focus_pending = true; break;
        case retcomm::hub::LibraryNav::Detail: hub.detail_focus_pending = true; break;
    }
}

// Home = the platform cards, the page the hub starts on.
void go_home(HubModel& hub) {
    hub.library_nav = retcomm::hub::LibraryNav::Platforms;
    hub.library_platform.clear();
    hub.home_focus_pending = true;
}

// Leave a title page for the grid it was opened from.
void go_back_to_titles(HubModel& hub) {
    hub.library_nav = retcomm::hub::LibraryNav::Titles;
    hub.grid_focus_pending = true;
}

// Leave every settings page and drop the drafts. Also ends any in-progress key
// or pad capture — walking away from the page must not leave the next keypress
// bound to a button the user can no longer see.
void close_settings_pages(HubModel& hub) {
    request_page_focus(hub);
    hub.show_mods_page = false;
    hub.show_library_panel = false;
    hub.show_settings = false;
    hub.show_romm_settings = false;
    hub.show_psx_settings = false;
    hub.show_snes_settings = false;
    hub.settings.dirty = false;
    hub.romm_settings.dirty = false;
    hub.psx_settings.dirty = false;
    hub.psx_settings.capturing_hotkey = -1;
    hub.psx_settings.configuring_player = -1;
    hub.psx_settings.gamepads_tab = false;
    cancel_psx_bind_capture(hub);
    hub.snes_settings.dirty = false;
    hub.snes_settings.capturing_hotkey = -1;
    hub.snes_settings.configuring_player = -1;
    cancel_snes_bind_capture(hub);
    cancel_snes_gesture_capture(hub);
}

// Panel width. Shared by the slide, the dim and the click-outside test.
float nav_drawer_width() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    return std::min(360.f, vp->WorkSize.x * 0.85f);
}

// While the drawer is over the page, the wheel must not scroll what is behind
// it. BeginDisabled covers hover, clicks and nav, but not scrolling, so the
// page's scrolling children opt out of the wheel for the duration.
ImGuiWindowFlags page_wheel_flags(const HubModel& hub) {
    return hub.drawer_t > 0.f ? ImGuiWindowFlags_NoScrollWithMouse : 0;
}

void open_nav_drawer(HubModel& hub) {
    hub.drawer_open = true;
    hub.drawer_focus_pending = true;
}

void close_nav_drawer(HubModel& hub) {
    if (!hub.drawer_open) return;
    hub.drawer_open = false;
    hub.hub_refocus_pending = true;
    // Send the ring back to the page content. ImGui's nav init would otherwise
    // put it on the window's first item — the hamburger — so the next A press
    // reopened the drawer that was just closed.
    request_page_focus(hub);
}

void toggle_nav_drawer(HubModel& hub) {
    if (hub.drawer_open) close_nav_drawer(hub);
    else open_nav_drawer(hub);
}

// Advance the drawer slide and return the eased position (0 = gone, 1 = open).
// Ticked before the page is drawn, so the page can be made inert by the same
// value that places the panel.
float tick_nav_drawer(HubModel& hub) {
    if (hub.pending_open_menu) {
        hub.pending_open_menu = false;
        open_nav_drawer(hub);
    }
    const float target = hub.drawer_open ? 1.f : 0.f;
    constexpr float kSlideSeconds = 0.16f;
    const float dt = ImGui::GetIO().DeltaTime;
    const float step = dt > 0.f ? dt / kSlideSeconds : 1.f;
    if (hub.drawer_t < target) hub.drawer_t = std::min(target, hub.drawer_t + step);
    else if (hub.drawer_t > target) hub.drawer_t = std::max(target, hub.drawer_t - step);
    const float inv = 1.f - hub.drawer_t;
    return 1.f - inv * inv * inv;  // ease-out cubic
}

void draw_marquee(HubModel& hub, const Theme& th, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = 72.f;
    dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + width, p0.y + h),
                                ImGui::ColorConvertFloat4ToU32(th.background2),
                                ImGui::ColorConvertFloat4ToU32(th.background2),
                                ImGui::ColorConvertFloat4ToU32(th.background),
                                ImGui::ColorConvertFloat4ToU32(th.background));
    // Neon underline: CRT violet → icon green.
    dl->AddRectFilledMultiColor(ImVec2(p0.x, p0.y + h - 3), ImVec2(p0.x + width, p0.y + h),
                                ImGui::ColorConvertFloat4ToU32(th.accent),
                                ImGui::ColorConvertFloat4ToU32(th.good),
                                ImGui::ColorConvertFloat4ToU32(th.good),
                                ImGui::ColorConvertFloat4ToU32(th.accent));

    ImGui::Dummy(ImVec2(width, h));
    const bool in_settings = any_settings_page_open(hub);

    // Hamburger, top-left: the one place every page keeps. Disabled while a
    // settings page holds edits, because switching pages would drop them.
    constexpr float kHamburger = 40.f;
    constexpr float kBrandX = 16.f + kHamburger + 16.f;
    {
        const float ham_y = p0.y + (h - kHamburger) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 16.f, ham_y));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
        ImGui::BeginDisabled(in_settings);
        if (ImGui::Button("##hamburger", ImVec2(kHamburger, kHamburger))) toggle_nav_drawer(hub);
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip(in_settings ? "Save or cancel the open settings page first"
                                          : "Menu  (Start on a controller)");
        const ImVec2 c(p0.x + 16.f + kHamburger * 0.5f, ham_y + kHamburger * 0.5f);
        const ImU32 bar = ImGui::ColorConvertFloat4ToU32(in_settings ? th.text_muted : th.text);
        for (int i = -1; i <= 1; ++i) {
            const float yy = c.y + static_cast<float>(i) * 6.f;
            dl->AddLine(ImVec2(c.x - 9.f, yy), ImVec2(c.x + 9.f, yy), bar, 2.f);
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(p0.x + kBrandX, p0.y + 14.f));
    ImGui::PushStyleColor(ImGuiCol_Text, th.good);
    ImGui::TextUnformatted("Retro Launcher");
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + kBrandX, p0.y + 40.f));
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("Retro Compilation Manager");
    ImGui::PopStyleColor();

    // Status chip (left of top-right actions).
    std::string chip = "Ready";
    ImVec4 chip_col = th.good;
    {
        std::string st;
        bool game_upd = false;
        {
            std::lock_guard<std::mutex> lock(hub.mu);
            st = hub.status;
            for (const auto& r : hub.rows) {
                if (r.update_available) {
                    game_upd = true;
                    break;
                }
            }
        }
        if (hub.job_running.load()) {
            chip = st.empty() ? "Working…" : st;
            if (chip.size() > 42) chip = chip.substr(0, 39) + "…";
            chip_col = th.accent;
        } else if (hub.launch_running.load()) {
            chip = (!st.empty() && st != "Ready") ? st : "Launching…";
            if (chip.size() > 42) chip = chip.substr(0, 39) + "…";
            chip_col = th.accent;
        } else if (hub.launcher_update_prompt_pending.load() ||
                   hub.toolchain_prompt_pending.load() || hub.toolchain_update_available ||
                   game_upd) {
            chip = "Update available";
            chip_col = th.warn;
        } else if (!st.empty() && st != "Ready") {
            // Keep last notable status briefly as chip when idle.
            if (st == "Up to date" || st.find("complete") != std::string::npos) {
                chip = st.size() > 42 ? st.substr(0, 39) + "…" : st;
                chip_col = th.text_muted;
            }
        }
    }
    {
        const ImVec2 chip_sz = ImGui::CalcTextSize(chip.c_str());
        const float chip_pad_x = 10.f;
        const float chip_pad_y = 5.f;
        const float chip_w = chip_sz.x + chip_pad_x * 2.f;
        const float chip_h = chip_sz.y + chip_pad_y * 2.f;
        // Clear of both brand lines (title + subtitle), not just the name.
        const float brand_w =
            std::max(ImGui::CalcTextSize("Retro Launcher").x,
                     ImGui::CalcTextSize("Retro Compilation Manager").x);
        const float chip_x = p0.x + kBrandX + brand_w + 28.f;
        const float chip_y = p0.y + (h - chip_h) * 0.5f;
        const ImVec2 c0(chip_x, chip_y);
        const ImVec2 c1(chip_x + chip_w, chip_y + chip_h);
        dl->AddRectFilled(c0, c1, ImGui::ColorConvertFloat4ToU32(th.background2), 6.f);
        dl->AddRect(c0, c1, ImGui::ColorConvertFloat4ToU32(chip_col), 6.f, 0, 1.5f);
        dl->AddText(ImVec2(chip_x + chip_pad_x, chip_y + chip_pad_y),
                    ImGui::ColorConvertFloat4ToU32(chip_col), chip.c_str());

        // Second chip: waiting Install/Update queue (only when non-empty).
        const std::size_t qn = hub.queued_job_count();
        if (qn > 0) {
            char qbuf[48];
            if (qn == 1)
                std::snprintf(qbuf, sizeof(qbuf), "1 queued");
            else
                std::snprintf(qbuf, sizeof(qbuf), "%zu queued", qn);
            const ImVec2 qsz = ImGui::CalcTextSize(qbuf);
            const float qw = qsz.x + chip_pad_x * 2.f;
            const float qh = qsz.y + chip_pad_y * 2.f;
            const float qx = chip_x + chip_w + 8.f;
            const float qy = p0.y + (h - qh) * 0.5f;
            const ImVec2 q0(qx, qy);
            const ImVec2 q1(qx + qw, qy + qh);
            dl->AddRectFilled(q0, q1, ImGui::ColorConvertFloat4ToU32(th.background2), 6.f);
            dl->AddRect(q0, q1, ImGui::ColorConvertFloat4ToU32(th.focus), 6.f, 0, 1.5f);
            dl->AddText(ImVec2(qx + chip_pad_x, qy + chip_pad_y),
                        ImGui::ColorConvertFloat4ToU32(th.focus), qbuf);
        }
    }

    // Top-right: Add/Scan Files + Check for Updates + Menu, or Save / Cancel
    // while a settings page is open — the header is where the eye lands, so it
    // carries the commit/discard choice rather than a bare way out.
    constexpr float kMenuH = 36.f;
    constexpr float kBtnGap = 8.f;
    const float btn_y = p0.y + (h - kMenuH) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.f, 8.f));
    const float frame_pad_x = ImGui::GetStyle().FramePadding.x;

    if (hub.show_library_panel) {
        // Add/Scan Files has nothing to commit, so it gets a way out rather
        // than Save/Cancel — and Check for Updates would be the wrong action to
        // leave under the cursor on a page about importing files.
        const char* cancel_label = "Cancel";
        const float cancel_w =
            std::max(96.f, ImGui::CalcTextSize(cancel_label).x + frame_pad_x * 2.f);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + width - 16.f - cancel_w, btn_y));
        if (ImGui::Button(cancel_label, ImVec2(cancel_w, kMenuH)))
            hub.show_library_panel = false;
    } else if (in_settings) {
        const char* save_label = "Save";
        const char* cancel_label = "Cancel";
        const float save_w =
            std::max(96.f, ImGui::CalcTextSize(save_label).x + frame_pad_x * 2.f);
        const float cancel_w =
            std::max(96.f, ImGui::CalcTextSize(cancel_label).x + frame_pad_x * 2.f);
        const float cancel_x = p0.x + width - 16.f - cancel_w;
        const float save_x = cancel_x - kBtnGap - save_w;

        // Say plainly that edits are pending, so Cancel never reads as "done".
        if (active_settings_dirty(hub)) {
            const char* warn_text = "unsaved changes";
            const ImVec2 ws = ImGui::CalcTextSize(warn_text);
            dl->AddText(ImVec2(save_x - 12.f - ws.x, p0.y + (h - ws.y) * 0.5f),
                        ImGui::ColorConvertFloat4ToU32(th.warn), warn_text);
        }

        ImGui::SetCursorScreenPos(ImVec2(save_x, btn_y));
        // Only leave on a save that landed; a failed write must keep the page
        // (and the edits) in front of the user.
        if (accent_button(save_label, th, ImVec2(save_w, kMenuH)) && save_active_settings(hub))
            close_settings_pages(hub);
        ImGui::SetCursorScreenPos(ImVec2(cancel_x, btn_y));
        if (ImGui::Button(cancel_label, ImVec2(cancel_w, kMenuH))) close_settings_pages(hub);
    } else {
        // Add/Scan Files and the old Menu button moved into the drawer; the
        // header keeps only the one action people reach for between sessions —
        // plus Back, which belongs beside it in the corner rather than down in
        // the page's own header band.
        float right_x = p0.x + width - 16.f;
        const bool show_back =
            hub.show_mods_page || hub.library_nav != retcomm::hub::LibraryNav::Platforms;
        if (show_back) {
            const char* back_label = "Back";
            const float back_w =
                std::max(96.f, ImGui::CalcTextSize(back_label).x + frame_pad_x * 2.f);
            right_x -= back_w;
            ImGui::SetCursorScreenPos(ImVec2(right_x, btn_y));
            if (ImGui::Button(back_label, ImVec2(back_w, kMenuH))) {
                if (hub.show_mods_page) hub.show_mods_page = false;
                else if (hub.library_nav == retcomm::hub::LibraryNav::Detail)
                    go_back_to_titles(hub);
                else go_home(hub);
            }
            right_x -= kBtnGap;
        }

        const char* updates_label = "Check for Updates";
        const float updates_w =
            std::max(120.f, ImGui::CalcTextSize(updates_label).x + frame_pad_x * 2.f);
        ImGui::SetCursorScreenPos(ImVec2(right_x - updates_w, btn_y));
        ImGui::BeginDisabled(hub.job_running.load());
        if (ImGui::Button(updates_label, ImVec2(updates_w, kMenuH)))
            hub.start_job(HubJob::CheckUpdates);
        ImGui::EndDisabled();
    }
    ImGui::PopStyleVar();

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + 8.f));
}

// Typical Named_Boxarts width/height per platform (placeholder when art missing).
// Real textures always size with their own aspect — never stretched to this.
float platform_boxart_aspect(const std::string& slug) {
    if (slug == "psx" || slug == "ps1" || slug == "ps" || slug == "psp") return 1.0f;
    if (slug == "gba" || slug == "gb" || slug == "gbc" || slug == "dmg") return 0.62f;
    if (slug == "nds") return 0.90f;
    // Cartridge / NA retail boxes tend to be portrait.
    if (slug == "snes" || slug == "n64" || slug == "genesis" || slug == "md" ||
        slug == "megadrive")
        return 0.72f;
    return 0.75f;
}

// Fit image into max box without cropping or stretching (letterbox unused space).
ImVec2 contain_size(float src_w, float src_h, float max_w, float max_h) {
    if (src_w <= 0.f || src_h <= 0.f) return ImVec2(max_w, max_h);
    const float scale = std::min(max_w / src_w, max_h / src_h);
    return ImVec2(src_w * scale, src_h * scale);
}

void draw_ellipsized_centered(const char* text, float max_w, const ImVec4& col) {
    if (!text || !text[0] || max_w <= 4.f) return;
    std::string shown = text;
    ImVec2 sz = ImGui::CalcTextSize(shown.c_str());
    if (sz.x > max_w) {
        const char* ell = "...";
        while (shown.size() > 1 &&
               ImGui::CalcTextSize((shown + ell).c_str()).x > max_w)
            shown.pop_back();
        shown += ell;
        sz = ImGui::CalcTextSize(shown.c_str());
    }
    const float x = ImGui::GetCursorScreenPos().x;
    const float y = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(x + std::max(0.f, (max_w - sz.x) * 0.5f), y));
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(shown.c_str());
    ImGui::PopStyleColor();
}

// Slightly smaller title under cover art: up to two centered lines, then ellipsis.
// Returns pixel height used (always reserves room for two lines when drawing).
float draw_wrapped_title_centered(const char* text, float max_w, const ImVec4& col,
                                  float font_scale = 0.84f) {
    ImGui::SetWindowFontScale(font_scale);
    const float line_h = ImGui::GetTextLineHeight();
    const float block_h = line_h * 2.f;
    if (!text || !text[0] || max_w <= 4.f) {
        ImGui::SetWindowFontScale(1.f);
        return block_h;
    }

    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const float scale = (font && font->FontSize > 0.f) ? (font_size / font->FontSize) : 1.f;
    const char* end = text + std::strlen(text);
    const char* mid =
        font ? font->CalcWordWrapPositionA(scale, text, end, max_w) : end;

    std::string l1(text, mid);
    while (!l1.empty() && (l1.back() == ' ' || l1.back() == '\t')) l1.pop_back();

    std::string l2;
    if (mid < end) {
        while (mid < end && (*mid == ' ' || *mid == '\t')) ++mid;
        l2.assign(mid, end);
        if (ImGui::CalcTextSize(l2.c_str()).x > max_w) {
            const char* ell = "...";
            while (l2.size() > 1 && ImGui::CalcTextSize((l2 + ell).c_str()).x > max_w)
                l2.pop_back();
            l2 += ell;
        }
    }

    const ImVec2 base = ImGui::GetCursorScreenPos();
    // Vertically center one-line titles inside the reserved two-line block.
    const float used_h = l2.empty() ? line_h : (line_h * 2.f);
    const float y0 = base.y + std::max(0.f, (block_h - used_h) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    auto draw_line = [&](const std::string& s, float y) {
        if (s.empty()) return;
        const ImVec2 sz = ImGui::CalcTextSize(s.c_str());
        ImGui::SetCursorScreenPos(ImVec2(base.x + std::max(0.f, (max_w - sz.x) * 0.5f), y));
        ImGui::TextUnformatted(s.c_str());
    };
    draw_line(l1, y0);
    if (!l2.empty()) draw_line(l2, y0 + line_h);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.f);
    return block_h;
}

// Portrait/platform grid tile: art on top (aspect preserved), labels below.
// Content is clipped inside the frame; colored border is drawn last on top.
// Returns total tile height used.
void draw_art_dim(ImDrawList* dl, const ImVec2& art0, const ImVec2& art1, float radius) {
    dl->AddRectFilled(art0, art1, IM_COL32(8, 10, 18, 155), radius, ImDrawFlags_RoundCornersTop);
}

void draw_busy_spinner(ImDrawList* dl, const ImVec2& art0, const ImVec2& art1, const Theme& th) {
    const ImVec2 c((art0.x + art1.x) * 0.5f, (art0.y + art1.y) * 0.5f);
    const float rad = std::min(art1.x - art0.x, art1.y - art0.y) * 0.16f;
    const float t = static_cast<float>(ImGui::GetTime()) * 4.8f;
    constexpr int kSeg = 11;
    for (int i = 0; i < kSeg; ++i) {
        const float a0 = t + (static_cast<float>(i) / static_cast<float>(kSeg)) * 6.2831853f;
        const float a1 = a0 + 0.5f;
        ImVec4 col = th.accent;
        col.w = 0.22f + 0.78f * (static_cast<float>(i + 1) / static_cast<float>(kSeg));
        dl->PathClear();
        dl->PathArcTo(c, rad, a0, a1, 10);
        dl->PathStroke(ImGui::ColorConvertFloat4ToU32(col), 0, 3.0f);
    }
}

void draw_queued_overlay(ImDrawList* dl, const ImVec2& art0, const ImVec2& art1, const Theme& th) {
    // Soft “paused download” disc — waiting in the Install/Update backlog.
    const ImVec2 c((art0.x + art1.x) * 0.5f, (art0.y + art1.y) * 0.5f);
    const float s = std::min(art1.x - art0.x, art1.y - art0.y);
    const float rad = s * 0.20f;
    dl->AddCircleFilled(c, rad, IM_COL32(12, 16, 28, 230), 36);
    dl->AddCircle(c, rad, ImGui::ColorConvertFloat4ToU32(th.focus), 36, 2.2f);
    const ImU32 ink = ImGui::ColorConvertFloat4ToU32(th.focus);
    // Pause bars.
    const float bw = rad * 0.20f;
    const float bh = rad * 0.52f;
    const float gap = rad * 0.16f;
    dl->AddRectFilled(ImVec2(c.x - gap - bw, c.y - bh * 0.55f),
                      ImVec2(c.x - gap, c.y + bh * 0.35f), ink, 2.f);
    dl->AddRectFilled(ImVec2(c.x + gap, c.y - bh * 0.55f),
                      ImVec2(c.x + gap + bw, c.y + bh * 0.35f), ink, 2.f);
    // Slim download tray under the pause — “download held”.
    dl->AddLine(ImVec2(c.x - rad * 0.42f, c.y + rad * 0.52f),
                ImVec2(c.x + rad * 0.42f, c.y + rad * 0.52f), ink, 2.0f);
}

void draw_rom_ready_overlay(ImDrawList* dl, const ImVec2& art0, const ImVec2& art1,
                            const Theme& th) {
    // Soft circle + check — verified local ROM ready to install.
    const ImVec2 c((art0.x + art1.x) * 0.5f, (art0.y + art1.y) * 0.5f);
    const float s = std::min(art1.x - art0.x, art1.y - art0.y);
    const float rad = s * 0.20f;
    dl->AddCircleFilled(c, rad, IM_COL32(12, 16, 28, 220), 36);
    dl->AddCircle(c, rad, ImGui::ColorConvertFloat4ToU32(th.focus), 36, 2.2f);
    const ImU32 ink = ImGui::ColorConvertFloat4ToU32(th.focus);
    const ImVec2 a(c.x - rad * 0.38f, c.y + rad * 0.02f);
    const ImVec2 b(c.x - rad * 0.06f, c.y + rad * 0.36f);
    const ImVec2 d(c.x + rad * 0.46f, c.y - rad * 0.34f);
    dl->AddLine(a, b, ink, 2.6f);
    dl->AddLine(b, d, ink, 2.6f);
}

// ---- Card lift -------------------------------------------------------------
// A focused or hovered card grows a few percent over ~an eighth of a second.
// The grid slot never changes size, so neighbours stay put; only the paint and
// the focus ring expand around the slot's centre.
constexpr float kTileGrow = 0.06f;   // fraction of the card's size at full lift
constexpr float kTileRise = 0.11f;   // seconds, rest → lifted
constexpr float kTileFall = 0.15f;   // seconds, lifted → rest

struct TilePop {
    float t = 0.f;        // 0 = resting, 1 = fully lifted
    bool active = false;  // focus/hover as of the frame that last drew this card
    int frame = -1;
};
std::unordered_map<std::string, TilePop> g_tile_pop;

// Advance this card's lift and report the scale to paint it at. The driving
// focus/hover is only known *after* the card's button is submitted, so the
// state read here is the one tile_pop_end() stored last frame — a frame of lag
// no one can see in a 110 ms ramp.
float tile_pop_begin(const std::string& key) {
    auto it = g_tile_pop.find(key);
    if (it == g_tile_pop.end()) return 1.f;  // first sight: rest, created by _end
    TilePop& e = it->second;
    const int frame = ImGui::GetFrameCount();
    // Off-screen since before last frame (another page was in front): a card
    // must come back resting, not still lifted from whenever it left.
    if (e.frame < frame - 1) {
        e.t = 0.f;
        e.active = false;
    }
    const float dt = ImGui::GetIO().DeltaTime > 0.f ? ImGui::GetIO().DeltaTime : 0.f;
    e.t = e.active ? std::min(1.f, e.t + dt / kTileRise)
                   : std::max(0.f, e.t - dt / kTileFall);
    e.frame = frame;
    const float ease = e.t * e.t * (3.f - 2.f * e.t);  // smoothstep
    return 1.f + kTileGrow * ease;
}

void tile_pop_end(const std::string& key, bool active) {
    const int frame = ImGui::GetFrameCount();
    TilePop& e = g_tile_pop[key];
    e.active = active;
    e.frame = frame;
    // The working set is whatever grid is on screen; drop the rest rather than
    // letting a big library leave an entry per title behind forever.
    if (g_tile_pop.size() > 1024) {
        for (auto it = g_tile_pop.begin(); it != g_tile_pop.end();)
            it = (it->second.frame < frame - 4) ? g_tile_pop.erase(it) : std::next(it);
    }
}

// The rectangle a card actually paints into once `pop` has lifted it: the slot
// grown about its own centre.
void tile_pop_rect(const ImVec2& tile_min, float tile_w, float tile_h, float pop,
                   ImVec2* out_min, ImVec2* out_max) {
    const float w = tile_w * pop;
    const float h = tile_h * pop;
    out_min->x = tile_min.x - (w - tile_w) * 0.5f;
    out_min->y = tile_min.y - (h - tile_h) * 0.5f;
    out_max->x = out_min->x + w;
    out_max->y = out_min->y + h;
}

float draw_grid_tile(const ImVec2& tile_min, float tile_w, bool selected, const Theme& th,
                     const BoxartTexture* tex, float art_aspect_wh, const char* title,
                     const char* subtitle, const ImVec4* badge_col, bool busy_spinner = false,
                     bool dim_art = false, bool update_overlay = false,
                     TileStatusIcon status_icon = TileStatusIcon::None,
                     bool queued_overlay = false, bool rom_ready_overlay = false,
                     float pop = 1.f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float kArtPad = 8.f;
    constexpr float kLabelGap = 6.f;
    constexpr float kFrameInset = 2.f; // keep art/badge inside the border stroke
    constexpr float kTitleScale = 0.84f;
    ImGui::SetWindowFontScale(kTitleScale);
    const float title_line_h = ImGui::GetTextLineHeight();
    ImGui::SetWindowFontScale(1.f);
    const float line_h = ImGui::GetTextLineHeight();
    // Always reserve two title lines so long names wrap instead of ellipsizing early.
    const float title_block_h = title_line_h * 2.f + 2.f;
    const float label_h =
        title_block_h + (subtitle && subtitle[0] ? line_h + 2.f : 0.f) + 8.f;

    // Art frame height follows platform/boxart aspect (width fixed by grid column).
    // `tile_h` is the resting height — what the grid lays out with. Painting
    // happens inside the lifted rect, which grows about the slot's centre; the
    // label keeps its type size, so only the art absorbs the extra height.
    const float rest_art_h = tile_w / std::max(0.35f, art_aspect_wh);
    const float tile_h = rest_art_h + kLabelGap + label_h;
    ImVec2 box_min, tile_max;
    tile_pop_rect(tile_min, tile_w, tile_h, pop, &box_min, &tile_max);
    const float art_box_w = tile_max.x - box_min.x;
    const float art_box_h = std::max(8.f, (tile_max.y - box_min.y) - kLabelGap - label_h);

    const ImU32 bg = ImGui::ColorConvertFloat4ToU32(selected ? th.panel_hovered : th.panel);
    dl->AddRectFilled(box_min, tile_max, bg, th.radius_sm);

    // Clip all inner content so it cannot paint over the frame edge.
    dl->PushClipRect(ImVec2(box_min.x + kFrameInset, box_min.y + kFrameInset),
                     ImVec2(tile_max.x - kFrameInset, tile_max.y - kFrameInset), true);

    const ImVec2 art0(box_min.x, box_min.y);
    const ImVec2 art1(box_min.x + art_box_w, box_min.y + art_box_h);
    dl->AddRectFilled(art0, art1, ImGui::ColorConvertFloat4ToU32(th.control), th.radius_sm,
                      ImDrawFlags_RoundCornersTop);

    if (tex && tex->gl_id && tex->width > 0 && tex->height > 0) {
        const ImVec2 fit =
            contain_size(static_cast<float>(tex->width), static_cast<float>(tex->height),
                         art_box_w - kArtPad * 2.f, art_box_h - kArtPad * 2.f);
        const float ix = art0.x + (art_box_w - fit.x) * 0.5f;
        const float iy = art0.y + (art_box_h - fit.y) * 0.5f;
        dl->AddImage((ImTextureID)(intptr_t)tex->gl_id, ImVec2(ix, iy),
                     ImVec2(ix + fit.x, iy + fit.y));
    }

    if (dim_art || busy_spinner || queued_overlay || rom_ready_overlay)
        draw_art_dim(dl, art0, art1, th.radius_sm);
    if (busy_spinner) draw_busy_spinner(dl, art0, art1, th);

    // Queued Install/Update: pause-download disc (wins over update / ROM ready).
    if (queued_overlay && !busy_spinner) {
        draw_queued_overlay(dl, art0, art1, th);
    } else if (update_overlay && !busy_spinner) {
        // Update-available: centered soft disc + up arrow over dimmed cover.
        const ImVec2 c((art0.x + art1.x) * 0.5f, (art0.y + art1.y) * 0.5f);
        const float s = std::min(art1.x - art0.x, art1.y - art0.y);
        const float rad = s * 0.20f;
        dl->AddCircleFilled(c, rad, IM_COL32(12, 16, 28, 220), 36);
        dl->AddCircle(c, rad, ImGui::ColorConvertFloat4ToU32(th.good), 36, 2.2f);
        const ImU32 arrow = ImGui::ColorConvertFloat4ToU32(th.good);
        const float h = rad * 0.58f;
        const float w = rad * 0.48f;
        const ImVec2 tip(c.x, c.y - h * 0.72f);
        const ImVec2 bl(c.x - w, c.y + h * 0.08f);
        const ImVec2 br(c.x + w, c.y + h * 0.08f);
        dl->AddTriangleFilled(tip, bl, br, arrow);
        const float stem_w = w * 0.42f;
        dl->AddRectFilled(ImVec2(c.x - stem_w * 0.5f, c.y - h * 0.08f),
                          ImVec2(c.x + stem_w * 0.5f, c.y + h * 0.72f), arrow, 2.f);
    } else if (rom_ready_overlay && !busy_spinner) {
        draw_rom_ready_overlay(dl, art0, art1, th);
    }

    if (badge_col) {
        // Slightly larger when carrying a glyph so the mark stays legible.
        const float r = (status_icon != TileStatusIcon::None) ? 10.f : 7.f;
        const ImVec2 c(art1.x - (12.f + (r - 7.f)), art0.y + (12.f + (r - 7.f)));
        dl->AddCircleFilled(c, r, ImGui::ColorConvertFloat4ToU32(*badge_col), 20);
        dl->AddCircle(c, r, ImGui::ColorConvertFloat4ToU32(th.background), 20, 1.5f);
        if (status_icon != TileStatusIcon::None)
            draw_status_badge_glyph(dl, c, r, status_icon, status_glyph_ink(*badge_col, th));
    }

    ImGui::SetCursorScreenPos(ImVec2(box_min.x + 4.f, box_min.y + art_box_h + kLabelGap));
    draw_wrapped_title_centered(title, art_box_w - 8.f, th.text, kTitleScale);
    if (subtitle && subtitle[0]) {
        ImGui::SetCursorScreenPos(
            ImVec2(box_min.x + 4.f, box_min.y + art_box_h + kLabelGap + title_block_h));
        draw_ellipsized_centered(subtitle, art_box_w - 8.f, th.text_muted);
    }

    dl->PopClipRect();

    // Frame stroke last so selection/border always sits above clipped content.
    const float border_t = selected ? 2.5f : 1.5f;
    dl->AddRect(box_min, tile_max,
                ImGui::ColorConvertFloat4ToU32(selected ? th.accent : th.border), th.radius_sm,
                0, border_t);
    return tile_h;
}

void select_first_visible_title(HubModel& hub) {
    hub.selected = 0;
    for (int i = 0; i < static_cast<int>(hub.rows.size()); ++i) {
        const TitleRow& cand = hub.rows[static_cast<size_t>(i)];
        if (!title_passes_library_filter(cand, hub)) continue;
        if (hub.library_platform.empty() || cand.platform == hub.library_platform) {
            hub.selected = i;
            break;
        }
    }
}

void enter_platform_titles(HubModel& hub, const char* platform_id) {
    hub.grid_focus_pending = true;
    if (!platform_id || !platform_id[0]) return;
    hub.library_nav = retcomm::hub::LibraryNav::Titles;
    hub.library_platform = platform_id;
    bool ok = hub.selected >= 0 && hub.selected < static_cast<int>(hub.rows.size());
    if (ok) {
        const TitleRow& sel = hub.rows[static_cast<size_t>(hub.selected)];
        if (!title_passes_library_filter(sel, hub)) ok = false;
        if (ok && sel.platform != hub.library_platform) ok = false;
    }
    if (!ok) select_first_visible_title(hub);
}

// A card press opens that title's own page; the grid no longer shares the
// window with a detail column, so selecting and opening are one gesture.
void open_title_page(HubModel& hub, int index) {
    hub.selected = index;
    hub.library_nav = retcomm::hub::LibraryNav::Detail;
    hub.detail_scroll_top = true;
    hub.detail_focus_pending = true;
    // Steam may have been edited from Steam itself since the last look.
    hub.steam_state_valid = false;
}

// The band at the top of a body page: page name on the left, the platform's
// Configure button and Back on the right.
//
// One function, not one per page: the grid and the title page sit either side
// of a single press, so any difference in band height or type size shows up as
// the header jumping. They were two copies of the same block, and the title
// page's copy had already lost the Configure button.
//
// `config_platform` empty → no Configure button. Back is not here: it lives in
// the top bar beside Check for Updates, where the eye already goes for the
// window's own controls.
enum class PageHeaderAction { None, Configure };

PageHeaderAction draw_page_header(const Theme& th, const char* header,
                                  const std::string& config_platform) {
    PageHeaderAction action = PageHeaderAction::None;

    constexpr float kHeaderScale = 1.45f;
    constexpr float kBtnPadX = 14.f;
    constexpr float kBtnPadY = 6.f;
    // Height of a padded button in this band, measured off a line of text.
    const float btn_h = ImGui::GetTextLineHeight() + kBtnPadY * 2.f;

    ImGui::SetWindowFontScale(kHeaderScale);
    const float title_h = ImGui::GetTextLineHeight();
    ImGui::SetWindowFontScale(1.f);
    // Same band on every page — tall enough for the padded controls.
    const float row_h = std::max(title_h, btn_h);

    const ImVec2 row0 = ImGui::GetCursorScreenPos();
    const float content_right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, row_h));

    const bool show_configure =
        !config_platform.empty() && retcomm::platform_has_config_section(config_platform);
    const char* cfg_label =
        show_configure ? retcomm::platform_config_button_label(config_platform) : "";
    const float cfg_w =
        show_configure ? ImGui::CalcTextSize(cfg_label).x + kBtnPadX * 2.f : 0.f;

    // Clip the name to whatever Configure leaves: at this type size a long
    // platform name would otherwise run under it on a narrow window.
    ImGui::SetWindowFontScale(kHeaderScale);
    const ImVec2 title_sz = ImGui::CalcTextSize(header);
    const float name_right = content_right - cfg_w - 12.f;
    ImGui::SetCursorScreenPos(ImVec2(row0.x, row0.y + (row_h - title_sz.y) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::PushClipRect(ImVec2(row0.x, row0.y),
                        ImVec2((std::max)(row0.x + 16.f, name_right), row0.y + row_h), true);
    ImGui::TextUnformatted(header);
    ImGui::PopClipRect();
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.f);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kBtnPadX, kBtnPadY));
    if (show_configure) {
        const bool snes = retcomm::is_snes_platform(config_platform);
        ImGui::SetCursorScreenPos(
            ImVec2(content_right - cfg_w, row0.y + (row_h - btn_h) * 0.5f));
        if (accent_button(cfg_label, th, ImVec2(cfg_w, btn_h)))
            action = PageHeaderAction::Configure;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "Global %s settings (Display, Audio, Input, Hotkeys).\n"
                "Applied to titles on install, update, and launch unless excluded.",
                snes ? "Super Nintendo" : "PlayStation");
        }
    }
    ImGui::PopStyleVar();

    ImGui::SetCursorScreenPos(ImVec2(row0.x, row0.y + row_h + 4.f));
    ImGui::Separator();
    return action;
}

void draw_library(HubModel& hub, BoxartCache& boxart, const Theme& th) {
    const bool job_busy = hub.job_running.load();
    const bool install_busy = job_busy && retcomm::hub::hub_job_is_install(hub.job);
    std::string busy_title_id;
    if (install_busy) busy_title_id = hub.job_title_id;
    ImGui::BeginChild("library", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      page_wheel_flags(hub));

    // Header: HOME on the platform picker; the platform's own name on its grid.
    {
        const bool on_titles = hub.library_nav == retcomm::hub::LibraryNav::Titles;
        const char* header = "HOME";
        if (on_titles && !hub.library_platform.empty())
            header = platform_display_name(hub.library_platform);
        const std::string config_platform = on_titles ? hub.library_platform : std::string();
        switch (draw_page_header(th, header, config_platform)) {
            case PageHeaderAction::Configure:
                if (retcomm::is_snes_platform(hub.library_platform)) hub.open_snes_settings();
                else hub.open_psx_settings();
                break;
            case PageHeaderAction::None:
                break;
        }
    }

    std::lock_guard<std::mutex> lock(hub.mu);

    // Snapshot while mu is held — do not call is_title_queued() here (it locks mu
    // again and deadlocks on a non-recursive mutex when entering a title grid).
    std::unordered_set<std::string> queued_titles;
    for (const auto& q : hub.job_queue) {
        if (!q.title_id.empty() && retcomm::hub::hub_job_is_queueable(q.job))
            queued_titles.insert(q.title_id);
    }

    // Cards sit at their intended size and spend the leftover width on the gap
    // between them rather than stretching to fill the row — a page of covers
    // reads better with air around each one, and the lift needs somewhere to go.
    constexpr float kGapMin = 20.f;
    constexpr float kGapMax = 36.f;
    // Slack kept at the grid's edges so a lifted card in the first/last column
    // is not clipped by the child window.
    const float pop_pad = 10.f;
    const float avail_w = std::max(80.f, ImGui::GetContentRegionAvail().x - pop_pad * 2.f);

    // Wrapping grid: tile width capped at the preferred size, gap takes the rest.
    struct GridMetrics {
        float tile_w;
        float gap;
        int cols;
    };
    auto begin_grid = [&](float prefer_tile_w, int min_cols, int max_cols) -> GridMetrics {
        int cols =
            (std::max)(min_cols, static_cast<int>((avail_w + kGapMin) / (prefer_tile_w + kGapMin)));
        cols = (std::clamp)(cols, min_cols, max_cols);
        const float fcols = static_cast<float>(cols);
        float tile_w = (avail_w - kGapMin * (fcols - 1.f)) / fcols;
        float gap = kGapMin;
        if (tile_w > prefer_tile_w) {
            tile_w = prefer_tile_w;
            if (cols > 1)
                gap = (std::clamp)((avail_w - tile_w * fcols) / (fcols - 1.f), kGapMin, kGapMax);
        }
        return {(std::max)(72.f, tile_w), gap, cols};
    };

    if (hub.library_nav == retcomm::hub::LibraryNav::Platforms) {
        std::map<std::string, int> counts;
        for (const auto& r : hub.rows) {
            if (!title_passes_library_filter(r, hub)) continue;
            if (!r.platform.empty()) counts[r.platform]++;
        }
        std::vector<std::string> platforms;
        platforms.reserve(counts.size());
        for (const auto& [plat, _] : counts) platforms.push_back(plat);
        std::sort(platforms.begin(), platforms.end());

        struct PlatCard {
            std::string id;
            std::string title;
            std::string subtitle;
        };
        std::vector<PlatCard> cards;
        for (const auto& plat : platforms) {
            const int n = counts[plat];
            cards.push_back({plat, platform_display_name(plat),
                             std::to_string(n) + (n == 1 ? " title" : " titles")});
        }

        // Home tiles: portrait cards (~ES style), larger than the title grid
        // and centred on both axes so the page reads as a dashboard from a
        // couch, not a list hugging the top-left corner.
        const GridMetrics g = begin_grid(176.f, 1, 6);
        const float tile_w = g.tile_w;
        const float gap = g.gap;
        const int cols = g.cols;
        const int n = static_cast<int>(cards.size());
        const int rows = n > 0 ? (n + cols - 1) / cols : 0;
        const int used_cols = std::min(cols, std::max(n, 1));
        const float grid_w =
            static_cast<float>(used_cols) * tile_w + static_cast<float>(used_cols - 1) * gap;
        // Tile height comes out of draw_grid_tile; use last frame's measure to
        // centre vertically (one frame top-aligned on first draw is invisible).
        static float s_home_tile_h = 0.f;
        const float grid_h =
            rows > 0 ? static_cast<float>(rows) * s_home_tile_h + static_cast<float>(rows - 1) * gap
                     : 0.f;
        const float avail_h = ImGui::GetContentRegionAvail().y;
        const float start_x =
            ImGui::GetCursorPosX() + pop_pad + std::max(0.f, (avail_w - grid_w) * 0.5f);
        const float start_y = ImGui::GetCursorPosY() + std::max(0.f, (avail_h - grid_h) * 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 focus_col = ImGui::ColorConvertFloat4ToU32(th.focus);
        const ImU32 hover_col = ImGui::ColorConvertFloat4ToU32(th.accent);
        float row_h = 0.f;
        float y = start_y;

        for (int i = 0; i < n; ++i) {
            const auto& card = cards[static_cast<size_t>(i)];
            const int col = i % cols;
            if (i > 0 && col == 0) {
                y += row_h + gap;
                row_h = 0.f;
            }
            ImGui::PushID(card.id.c_str());
            ImGui::SetCursorPos(ImVec2(start_x + static_cast<float>(col) * (tile_w + gap), y));
            const ImVec2 tile_min = ImGui::GetCursorScreenPos();

            const std::string pop_key = "platform:" + card.id;
            const float pop = tile_pop_begin(pop_key);

            const fs::path icon = platform_icon_path(card.id);
            const BoxartTexture* tex =
                icon.empty() ? nullptr : boxart.get(std::string("platform:") + card.id, icon);
            // Controller icons are landscape; tile frame is portrait — contain, no stretch.
            constexpr float kPlatAspect = 0.78f;
            const float tile_h = draw_grid_tile(
                tile_min, tile_w, false, th, tex, kPlatAspect, card.title.c_str(),
                card.subtitle.c_str(), nullptr, false, false, false, TileStatusIcon::None, false,
                false, pop);

            ImGui::SetCursorScreenPos(tile_min);
            // First card is where the focus ring starts, so A on a pad goes
            // straight into the library. SetItemDefaultFocus only works on an
            // appearing window and the rows load after it appeared, so hand
            // the focus over explicitly: a tabbing request onto the next item,
            // and the cursor made visible because activation is gated on it.
            if (i == 0 && hub.home_focus_pending &&
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                hub.home_focus_pending = false;
                ImGui::SetKeyboardFocusHere();
                ImGui::SetNavCursorVisible(true);
            }
            // InvisibleButton opts out of nav unless told otherwise (1.90+);
            // without EnableNav a controller could never reach a card.
            if (ImGui::InvisibleButton("##plat", ImVec2(tile_w, tile_h),
                                       ImGuiButtonFlags_EnableNav))
                enter_platform_titles(hub, card.id.c_str());
            // InvisibleButton draws no nav cursor of its own; ring the card so
            // a controller always knows where it is. Hover gets a softer ring.
            // The ring follows the lifted rect, not the slot.
            const bool lifted = ImGui::IsItemFocused() || ImGui::IsItemHovered();
            tile_pop_end(pop_key, lifted);
            ImVec2 ring_min, ring_max;
            tile_pop_rect(tile_min, tile_w, tile_h, pop, &ring_min, &ring_max);
            if (ImGui::IsItemFocused())
                dl->AddRect(ring_min, ring_max, focus_col, th.radius_sm, 0, 3.f);
            else if (ImGui::IsItemHovered())
                dl->AddRect(ring_min, ring_max, hover_col, th.radius_sm, 0, 2.f);

            row_h = (std::max)(row_h, tile_h);
            s_home_tile_h = tile_h;
            ImGui::PopID();
        }
        ImGui::SetCursorPos(ImVec2(start_x, y + row_h + gap));
        ImGui::Dummy(ImVec2(0, 0));
    } else {
        // Title cover grid for one platform (no mixed "All" view).
        const std::string& plat_filter = hub.library_platform;
        const float filter_aspect = platform_boxart_aspect(plat_filter);
        const float prefer_w = (filter_aspect >= 0.95f) ? 208.f : 174.f;
        const GridMetrics g = begin_grid(prefer_w, 1, 7);
        const float tile_w = g.tile_w;
        const float gap = g.gap;
        const int cols = g.cols;

        // The ring is handed to the selected card, not to card zero: Back from a
        // title page has to return to the card it was opened from. Pin the
        // selection to something this grid actually draws first, or the hand-off
        // would never find its target and the ring would be left behind.
        {
            bool sel_drawn = false;
            int first_drawn = -1;
            for (int i = 0; i < static_cast<int>(hub.rows.size()); ++i) {
                const TitleRow& r = hub.rows[static_cast<size_t>(i)];
                if (r.platform != plat_filter) continue;
                if (!title_passes_library_filter(r, hub)) continue;
                if (first_drawn < 0) first_drawn = i;
                if (i == hub.selected) sel_drawn = true;
            }
            if (!sel_drawn) hub.selected = first_drawn >= 0 ? first_drawn : 0;
        }

        // Centre the block of columns and leave the lift room at the top, the
        // same way Home does — a grid pinned to the left edge reads as a list.
        const float grid_w = static_cast<float>(cols) * tile_w + static_cast<float>(cols - 1) * gap;
        const float start_x =
            ImGui::GetCursorPosX() + pop_pad + std::max(0.f, (avail_w - grid_w) * 0.5f);
        int col = 0;
        float y = ImGui::GetCursorPosY() + pop_pad;
        float row_h = 0.f;

        for (int i = 0; i < static_cast<int>(hub.rows.size()); ++i) {
            const TitleRow& r = hub.rows[static_cast<size_t>(i)];
            if (r.platform != plat_filter) continue;
            if (!title_passes_library_filter(r, hub)) continue;

            const float aspect = filter_aspect;

            if (col >= cols) {
                col = 0;
                y += row_h + gap;
                row_h = 0.f;
            }

            ImGui::PushID(r.id.c_str());
            ImGui::SetCursorPos(ImVec2(start_x + static_cast<float>(col) * (tile_w + gap), y));
            const ImVec2 tile_min = ImGui::GetCursorScreenPos();
            const bool selected = (hub.selected == i);
            const bool title_busy = install_busy && r.id == busy_title_id;
            const bool title_queued = !title_busy && queued_titles.count(r.id) > 0;
            const bool needs_update = r.update_available;
            const bool rom_ready =
                r.has_rom && !r.installed && !r.install_dir_present && !needs_update;
            const bool dim_art = !r.installed || needs_update || title_queued;
            const BoxartTexture* tex =
                r.boxart_path.empty() ? nullptr : boxart.get(r.id, r.boxart_path);
            ImVec4 badge = chip_color(r, th);
            TileStatusIcon status = tile_status_icon(r);
            if (title_queued) {
                badge = th.focus;
                status = TileStatusIcon::Queued;
            }
            const float pop = tile_pop_begin(r.id);
            const float tile_h = draw_grid_tile(
                tile_min, tile_w, selected, th, tex, aspect, r.name.c_str(), nullptr, &badge,
                title_busy, dim_art, needs_update && !title_queued, status, title_queued,
                rom_ready && !title_queued, pop);

            ImGui::SetCursorScreenPos(tile_min);
            if (hub.grid_focus_pending && selected &&
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                hub.grid_focus_pending = false;
                ImGui::SetKeyboardFocusHere();
                ImGui::SetNavCursorVisible(true);
            }
            if (ImGui::InvisibleButton("##row", ImVec2(tile_w, tile_h),
                                       ImGuiButtonFlags_EnableNav))
                open_title_page(hub, i);
            tile_pop_end(r.id, ImGui::IsItemFocused() || ImGui::IsItemHovered());
            if (ImGui::IsItemFocused()) {
                ImVec2 ring_min, ring_max;
                tile_pop_rect(tile_min, tile_w, tile_h, pop, &ring_min, &ring_max);
                ImGui::GetWindowDrawList()->AddRect(
                    ring_min, ring_max, ImGui::ColorConvertFloat4ToU32(th.focus), th.radius_sm, 0,
                    3.f);
                // Keep the selection under the ring: Back from a title page
                // returns the ring to the card the page was opened from.
                if (hub.selected != i) {
                    hub.selected = i;
                    hub.detail_scroll_top = true;
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("%s\n%s · %s\n%s", r.name.c_str(),
                                  platform_display_name(r.platform), r.kind.c_str(),
                                  title_queued ? "QUEUED" : chip_label(r));
            }

            row_h = (std::max)(row_h, tile_h);
            ++col;
            ImGui::PopID();
        }
        ImGui::SetCursorPos(ImVec2(start_x, y + row_h + gap));
        ImGui::Dummy(ImVec2(0, 0));
    }
    // B (or Escape) on a title grid goes back to Home, unless something modal
    // or the drawer is in front and owns that key.
    if (hub.library_nav == retcomm::hub::LibraryNav::Titles && !hub.drawer_open &&
        !hub.log_overlay_open &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        !ImGui::IsAnyItemActive() &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        go_home(hub);
    ImGui::EndChild();
}

void center_modal_next() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}

// Dismiss the topmost modal when the user clicks the dimmed backdrop (not the
// modal itself). Nested confirms stay sticky until they are topmost. Call just
// before EndPopup() inside BeginPopupModal. Skip first-run setup wizard.
void close_modal_on_outside_click() {
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    if (ImGui::GetTopMostPopupModal() != ImGui::GetCurrentWindow()) return;
    const ImVec2 tl = ImGui::GetWindowPos();
    const ImVec2 br(tl.x + ImGui::GetWindowSize().x, tl.y + ImGui::GetWindowSize().y);
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (m.x >= tl.x && m.y >= tl.y && m.x < br.x && m.y < br.y) return;
    ImGui::CloseCurrentPopup();
}

void draw_detail_save_controls(HubModel& hub, const TitleRow& row, const Theme& th, bool busy,
                               SDL_Window* window) {
    struct PickerState {
        std::string title_id;
        int slot = 0;   // 0 = cart/save file, 1 = memcard 1, 2 = memcard 2
        int sel = -2;   // -1 = Empty Card Slot (slot 2 only), >=0 = save index
        bool renaming = false;
        char rename_buf[256]{};
    };
    static PickerState ps;

    auto slot_button_label = [](const TitleRow& r, int index, bool is_card2) -> std::string {
        if (is_card2 && index < 0) return "Empty Card Slot";
        if (index >= 0 && index < static_cast<int>(r.save_labels.size()))
            return r.save_labels[static_cast<size_t>(index)];
        if (!r.save_labels.empty() && !is_card2) return r.save_labels.front();
        return "None";
    };

    auto open_picker = [&](int slot) {
        ps.title_id = row.id;
        ps.slot = slot;
        ps.renaming = false;
        ps.rename_buf[0] = '\0';
        if (slot == 2)
            ps.sel = row.preferred_save_card2_index;
        else if (row.preferred_save_index >= 0)
            ps.sel = row.preferred_save_index;
        else
            ps.sel = row.save_labels.empty() ? -2 : 0;
        ImGui::OpenPopup("###memcard_picker");
    };

    if (row.dual_memcard) {
        ImGui::TextColored(th.text_muted, "Memory card 1 & 2");
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 4.f));
        {
            const std::string lab =
                slot_button_label(row, row.preferred_save_index, false) + "##memcard1_btn";
            if (ImGui::Button(lab.c_str(), ImVec2(-1, 0))) open_picker(1);
        }
        {
            const std::string lab =
                slot_button_label(row, row.preferred_save_card2_index, true) + "##memcard2_btn";
            if (ImGui::Button(lab.c_str(), ImVec2(-1, 0))) open_picker(2);
        }
        ImGui::PopStyleVar();
    } else {
        ImGui::TextColored(th.text_muted, "Save file");
        const std::string lab =
            slot_button_label(row, row.preferred_save_index, false) + "##save_btn";
        if (ImGui::Button(lab.c_str(), ImVec2(-1, 0))) open_picker(0);
    }

    if (!ImGui::IsPopupOpen("###memcard_picker")) return;
    constexpr float kW = 560.f;
    constexpr float kH = 720.f;
    center_modal_next();
    ImGui::SetNextWindowSizeConstraints(ImVec2(kW, kH), ImVec2(kW, kH));
    ImGui::SetNextWindowSize(ImVec2(kW, kH), ImGuiCond_Appearing);

    const char* slot_title = "Save file";
    if (ps.slot == 1) slot_title = "Memory Card 1";
    else if (ps.slot == 2) slot_title = "Memory Card 2";
    char picker_title[96];
    std::snprintf(picker_title, sizeof(picker_title), "%s###memcard_picker", slot_title);
    if (!ImGui::BeginPopupModal(
            picker_title, nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        return;

    const TitleRow* live = nullptr;
    for (const auto& r : hub.rows) {
        if (r.id == ps.title_id) {
            live = &r;
            break;
        }
    }
    if (!live) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextWrapped("%s", live->name.c_str());
    ImGui::Separator();

    const bool allow_empty = (ps.slot == 2);
    // Negative height: fill remaining space after the footer (list scrolls; window does not).
    // Include ItemSpacing after every widget (Dummy / button rows / Close), plus a safety
    // margin — under-reserving clips Close under NoScrollbar.
    constexpr float kPadTop = 12.f;
    constexpr float kPadClose = 14.f;
    constexpr float kFooterSafety = 12.f;
    const float fh = ImGui::GetFrameHeight();
    const float sp = ImGui::GetStyle().ItemSpacing.y;
    const float footer =
        ps.renaming
            // Dummy + InputText + Confirm + Cancel + Dummy + Close (+ spacing each)
            ? (sp + kPadTop + sp + fh + sp + fh + sp + fh + sp + kPadClose + sp + fh +
               kFooterSafety)
            // Dummy + 2 action rows + Dummy + Close (+ spacing each)
            : (sp + kPadTop + sp + fh + sp + fh + sp + kPadClose + sp + fh + kFooterSafety);
    ImGui::BeginChild("##memcard_list", ImVec2(-FLT_MIN, -footer), ImGuiChildFlags_Borders);

    auto apply_selection = [&](int sel) {
        std::string err;
        bool ok = false;
        if (ps.slot == 2) {
            if (sel < 0)
                ok = hub.set_title_preferred_save_card2(ps.title_id, retcomm::kBlankMemcardId,
                                                        &err);
            else if (sel < static_cast<int>(live->save_ids.size()))
                ok = hub.set_title_preferred_save_card2(ps.title_id, live->save_ids[static_cast<size_t>(sel)],
                                                        &err);
        } else if (sel >= 0 && sel < static_cast<int>(live->save_ids.size())) {
            ok = hub.set_title_preferred_save(ps.title_id, live->save_ids[static_cast<size_t>(sel)],
                                              &err);
        }
        if (!ok && !err.empty()) hub.append_log("Could not save preference: " + err);
        return ok;
    };

    // Theme Header matches panel bg, so Selectable selection is invisible without a push.
    auto selectable_row = [&](const char* label, bool sel) -> bool {
        if (sel) {
            ImGui::PushStyleColor(ImGuiCol_Header, th.accent_button);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, th.accent_button_hovered);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, th.accent_button_active);
        }
        const bool clicked =
            ImGui::Selectable(label, sel, ImGuiSelectableFlags_AllowDoubleClick);
        if (sel) {
            ImGui::PopStyleColor(3);
            ImGui::SetItemDefaultFocus();
        }
        return clicked;
    };

    if (allow_empty) {
        const bool sel = (ps.sel == -1);
        if (selectable_row("Empty Card Slot", sel)) {
            ps.sel = -1;
            ps.renaming = false;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (apply_selection(-1)) ImGui::CloseCurrentPopup();
            }
        }
    }

    for (size_t i = 0; i < live->save_labels.size(); ++i) {
        const bool sel = (ps.sel == static_cast<int>(i));
        const std::string item = live->save_labels[i] + "##mc" + std::to_string(i);
        if (selectable_row(item.c_str(), sel)) {
            ps.sel = static_cast<int>(i);
            ps.renaming = false;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (apply_selection(ps.sel)) ImGui::CloseCurrentPopup();
            }
        }
    }
    if (live->save_labels.empty() && !allow_empty) {
        ImGui::TextColored(th.text_muted, "No save files yet.");
    }
    ImGui::EndChild();

    const bool has_file = ps.sel >= 0 && ps.sel < static_cast<int>(live->save_ids.size());
    const bool can_select = has_file || (allow_empty && ps.sel == -1);

    if (ps.renaming && has_file) {
        ImGui::Dummy(ImVec2(0, 12));
        ImGui::SetNextItemWidth(-1);
        hub_input_text("##rename_save", ps.rename_buf, sizeof(ps.rename_buf));
        if (ImGui::Button("Confirm rename", ImVec2(-1, 0))) {
            std::string err;
            const std::string id = live->save_ids[static_cast<size_t>(ps.sel)];
            if (hub.rename_title_save(ps.title_id, id, ps.rename_buf, &err)) {
                ps.renaming = false;
                // Re-resolve selection after refresh.
                for (const auto& r : hub.rows) {
                    if (r.id != ps.title_id) continue;
                    live = &r;
                    break;
                }
                if (live) {
                    const std::string want = std::string("saves/") + ps.rename_buf;
                    ps.sel = -2;
                    for (size_t i = 0; i < live->save_ids.size(); ++i) {
                        if (live->save_ids[i] == want ||
                            live->save_labels[i] == ps.rename_buf) {
                            ps.sel = static_cast<int>(i);
                            break;
                        }
                    }
                    if (ps.sel < 0) {
                        // Extension may have been added by rename.
                        const std::string base = ps.rename_buf;
                        for (size_t i = 0; i < live->save_labels.size(); ++i) {
                            if (live->save_labels[i].rfind(base, 0) == 0) {
                                ps.sel = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                }
            } else {
                hub.append_log("Rename failed: " + err);
            }
        }
        if (ImGui::Button("Cancel rename", ImVec2(-1, 0))) ps.renaming = false;
    } else {
        ImGui::Dummy(ImVec2(0, 12));
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float third = (ImGui::GetContentRegionAvail().x - gap * 2.f) / 3.f;
        ImGui::BeginDisabled(!can_select);
        if (good_button("Select", th, ImVec2(third, 0))) {
            if (apply_selection(ps.sel)) ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, gap);
        ImGui::BeginDisabled(!has_file);
        if (danger_button("Delete", th, ImVec2(third, 0))) {
            std::string err;
            const std::string id = live->save_ids[static_cast<size_t>(ps.sel)];
            if (hub.delete_title_save(ps.title_id, id, &err)) {
                ps.sel = allow_empty ? -1 : -2;
                ps.renaming = false;
            } else {
                hub.append_log("Delete failed: " + err);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, gap);
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Create", ImVec2(third, 0))) {
            std::string err;
            if (hub.create_title_save(ps.title_id, &err, ps.slot == 2)) {
                ps.renaming = false;
                for (const auto& r : hub.rows) {
                    if (r.id != ps.title_id) continue;
                    if (ps.slot == 2)
                        ps.sel = r.preferred_save_card2_index;
                    else
                        ps.sel = r.preferred_save_index;
                    break;
                }
            } else {
                hub.append_log("Create failed: " + err);
            }
        }
        ImGui::EndDisabled();

        bool file_busy = false;
        {
            std::lock_guard<std::mutex> lock(hub.file_pick_mu);
            file_busy = hub.file_pick_busy;
        }
        ImGui::BeginDisabled(!has_file);
        if (ImGui::Button("Rename", ImVec2(third, 0))) {
            const std::string& lab = live->save_labels[static_cast<size_t>(ps.sel)];
            std::snprintf(ps.rename_buf, sizeof(ps.rename_buf), "%s", lab.c_str());
            ps.renaming = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, gap);
        ImGui::BeginDisabled(busy || file_busy || window == nullptr);
        if (ImGui::Button("Import Save File", ImVec2(third, 0))) {
            const auto exts = save_exts_for_platform(hub.catalog, live->platform);
            begin_file_pick(hub, window, retcomm::hub::FilePickKind::ImportSave, live->platform,
                            "Save files", exts, /*allow_many=*/false, ps.title_id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, gap);
        ImGui::BeginDisabled(busy || !live->romm_ready || !live->installed);
        if (romm_button("Sync with RomM", th, ImVec2(third, 0))) {
            hub.start_job(HubJob::SyncRommSaves, ps.title_id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
    }

    ImGui::Dummy(ImVec2(0, 14));
    if (ImGui::Button("Close", ImVec2(-1, 0))) {
        ps.renaming = false;
        ImGui::CloseCurrentPopup();
    }
    close_modal_on_outside_click();
    ImGui::EndPopup();
}

// Per-title HD texture packs: install several, run one. Coverage comes from the
// coverage.json psxrecomp writes at exit, so it reflects what the last session
// actually drew — a pack can be fully installed and still show a low number
// simply because you have not reached those screens yet.
void draw_detail_texture_packs_popup(HubModel& hub, const TitleRow& row, const Theme& th,
                                     bool busy, SDL_Window* window) {
    if (!ImGui::IsPopupOpen("Texture Packs###detail_texture_packs")) return;
    constexpr float kW = 620.f;
    center_modal_next();
    ImGui::SetNextWindowSizeConstraints(ImVec2(kW, 0.f), ImVec2(kW, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(kW, 0.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Texture Packs###detail_texture_packs", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + kW - 40.f);
    ImGui::TextWrapped("%s", row.name.c_str());
    ImGui::TextColored(th.text_muted,
                       "Beetle PSX HW format packs. One pack is active at a time.");
    ImGui::Separator();
    ImGui::BeginDisabled(busy);

    const auto& packs = hub.texpack_list;
    if (packs.empty()) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(th.text_muted, "No texture packs installed for this title.");
        ImGui::Dummy(ImVec2(0, 6));
    } else if (ImGui::BeginTable("texpack_table", 5,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed, 38.f);
        ImGui::TableSetupColumn("Pack", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Textures", ImGuiTableColumnFlags_WidthFixed, 74.f);
        ImGui::TableSetupColumn("Coverage", ImGuiTableColumnFlags_WidthFixed, 116.f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 74.f);
        ImGui::TableHeadersRow();

        for (const auto& p : packs) {
            ImGui::PushID(p.id.c_str());
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            const bool active = (row.active_texture_pack == p.id);
            if (ImGui::RadioButton("##use", active))
                hub.set_texture_pack(row.id, active ? std::string{} : p.id);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip(active ? "Click again to use native textures"
                                         : "Use this pack when the game launches");

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(p.name.c_str());
            if (!p.author.empty() || !p.version.empty()) {
                std::string sub = p.author;
                if (!p.version.empty()) sub += (sub.empty() ? "" : " · ") + p.version;
                ImGui::TextColored(th.text_muted, "%s", sub.c_str());
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !p.description.empty())
                ImGui::SetTooltip("%s", p.description.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", p.texture_count);

            ImGui::TableSetColumnIndex(3);
            if (p.has_coverage && p.coverage_entries > 0) {
                const int pct = (int)((100.0 * p.coverage_matched) / p.coverage_entries + 0.5);
                ImGui::Text("%d / %d (%d%%)", p.coverage_matched, p.coverage_entries, pct);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip(
                        "Textures this pack replaced during the last session (%llds of play).\n"
                        "Play further to raise it — unseen screens cannot count.",
                        (long long)p.coverage_seconds);
            } else {
                ImGui::TextColored(th.text_muted, "—");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("Play the game with this pack to measure coverage.");
            }

            ImGui::TableSetColumnIndex(4);
            if (danger_button("Remove", th, ImVec2(-1, 0)))
                ImGui::OpenPopup("Remove texture pack?");
            if (ImGui::BeginPopupModal("Remove texture pack?", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Delete '%s' from disk?", p.name.c_str());
                ImGui::TextColored(th.text_muted, "The game's own textures are untouched.");
                ImGui::Separator();
                if (danger_button("Remove", th, ImVec2(140, 0))) {
                    hub.remove_texture_pack_now(row.id, p.id);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(140, 0))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (accent_button("Install Pack…", th, ImVec2(180, 0))) {
        begin_file_pick(hub, window, retcomm::hub::FilePickKind::ImportTexturePack,
                        row.platform, "Texture pack (.zip)", {"zip"}, false, row.id);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("A .zip of <hash>-<hash>.png files, as dumped by\n"
                          "Beetle PSX HW or this game's own texture dumper.\n"
                          "A wrapping folder inside the zip is handled for you.");
    ImGui::SameLine();
    if (ImGui::Button("Open Folder", ImVec2(150, 0))) {
        const fs::path dir = retcomm::texture_packs_dir(hub.paths, row.id);
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string err;
        if (!retcomm::open_path_in_file_manager(dir, &err))
            hub.append_log("Open Folder failed: " + err);
    }

    if (!hub.texpack_status.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextWrapped("%s", hub.texpack_status.c_str());
    }

    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}

// Left column of Manage Game Data: everything that acts on the install itself.
void draw_manage_install_column(HubModel& hub, const TitleRow& row, const Theme& th) {

    const bool can_open =
        row.installed || row.install_dir_present || row.has_preserved_state;
    if (can_open && ImGui::Button("Open Folder", ImVec2(-1, 0))) {
        fs::path open_dir;
        if (!row.install_root.empty())
            open_dir = retcomm::resolve_current_release_dir(row.install_root);
        if (open_dir.empty() && !row.binary_path.empty()) {
            fs::path walk = fs::path(row.binary_path).parent_path();
            while (!walk.empty() && walk.has_parent_path()) {
                if (walk.parent_path().filename() == "releases") {
                    open_dir = walk;
                    break;
                }
                const fs::path parent = walk.parent_path();
                if (parent == walk) break;
                walk = parent;
            }
            if (open_dir.empty()) open_dir = fs::path(row.binary_path).parent_path();
        }
        if (open_dir.empty()) open_dir = row.install_root;
        std::string err;
        if (!retcomm::open_path_in_file_manager(open_dir, &err))
            hub.append_log("Open Folder failed: " + err);
    }
    if (can_open) {
        const auto roots = retcomm::effective_install_roots(hub.cfg, hub.paths);
        const bool can_move = roots.size() > 1;
        ImGui::BeginDisabled(!can_move);
        if (ImGui::Button("Move Installation Dir", ImVec2(-1, 0))) {
            hub.begin_move_install(row.id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal |
                                 ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (can_move) {
                ImGui::SetTooltip(
                    "Move this title's install folder (releases, build data, preserved "
                    "saves/config) to another configured install location.");
            } else {
                ImGui::SetTooltip(
                    "Add another install location in Library Settings to enable Move.");
            }
        }
    }

    if (retcomm::platform_has_config_section(row.platform)) {
        ImGui::Dummy(ImVec2(0, 6));
        bool exclude = retcomm::title_excludes_platform_config(hub.app_state, row.id);
        if (ImGui::Checkbox("Exclude from platform config", &exclude)) {
            std::string err;
            if (!hub.set_title_exclude_platform_config(row.id, exclude, &err)) {
                hub.append_log("Exclude toggle failed: " + err);
            } else {
                hub.app_state = retcomm::load_app_state(hub.paths.state_path);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "When checked, Retro will not overwrite this title's settings.toml / "
                "config.ini from global PlayStation Configure on install, update, or "
                "launch.\nExisting files are left as-is.");
        }
    }

    const bool can_uninstall =
        row.installed || row.install_dir_present || row.has_preserved_state;
    // Keep-saves uninstall left only apps/<title>/preserved/ — second action is a purge.
    const bool preserved_only =
        row.has_preserved_state && !row.installed && !row.install_dir_present;
    if (can_uninstall) {
        ImGui::Dummy(ImVec2(0, 6));
        if (preserved_only) {
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Game files are already removed. Preserved saves/config remain under "
                "preserved/ for the next install.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4));
            if (ImGui::Button("Remove Saves & Config", ImVec2(-1, 0))) {
                hub.start_job(HubJob::UninstallPurge, row.id);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "Deletes preserved/ (saves, settings, keybinds stashed from the last "
                    "uninstall).\n"
                    "Library ROMs and managed saves_root files are not deleted.");
            }
        } else {
            if (row.supports_local_build &&
                (row.installed || row.install_dir_present || row.install_method == "build")) {
                ImGui::BeginDisabled(!row.has_cmake_build_data);
                if (ImGui::Button("Delete Build Data", ImVec2(-1, 0))) {
                    hub.start_job(HubJob::DeleteBuildData, row.id);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal |
                                         ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (row.has_cmake_build_data) {
                        ImGui::SetTooltip(
                            "Deletes cmake build intermediates (src/*/build/) to free disk "
                            "space.\n"
                            "The next app update will need a full rebuild instead of an "
                            "incremental one.\n"
                            "Does not remove the installed game, saves, or ROM library.");
                    } else {
                        ImGui::SetTooltip("No cmake build data on disk.");
                    }
                }
                ImGui::Dummy(ImVec2(0, 4));
            }
            static bool keep_saves = true;
            static std::string keep_saves_for_id;
            if (keep_saves_for_id != row.id) {
                keep_saves_for_id = row.id;
                keep_saves = true;
            }
            if (ImGui::Button("Uninstall", ImVec2(-1, 0))) {
                hub.start_job(keep_saves ? HubJob::Uninstall : HubJob::UninstallPurge, row.id);
                ImGui::CloseCurrentPopup();
            }
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::Checkbox("Keep save data", &keep_saves);
        }
    } else {
        ImGui::TextColored(th.text_muted, "No install folder or preserved data yet.");
    }
}

// Right column: the files behind the install — where its ROM comes from, what
// RomM can re-fetch, and the Steam entry that starts it.
void draw_manage_files_column(HubModel& hub, const TitleRow& row, const Theme& th) {

    // Add to Steam. A non-Steam shortcut is another way of pressing Play, so it
    // belongs with the rest of this install's management rather than beside the
    // button itself. The shortcut runs this launcher's CLI, not the game binary,
    // so disc, BIOS, save and memory cards resolve exactly as they do here — and
    // the CLI waits for the game, so Steam's session stays attached to it.
    if (row.installed) {
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::TextColored(th.text_muted, "Steam");
        if (!hub.steam_state_valid.load()) hub.refresh_steam_state();
        const std::string steam_name = hub.steam_shortcut_name(row);
        const bool in_steam = hub.steam_shortcut_ids.count(row.id) > 0;
        const bool steam_busy = hub.steam_busy.load();
        if (!hub.steam_found) {
            ImGui::TextColored(th.text_muted, "%s", hub.steam_hint.c_str());
        } else {
            // Steam keeps its shortcut list in memory and writes it back on exit,
            // so a write made while it runs is silently thrown away. Asked at the
            // moment of the press, not shown as standing text, because it is only
            // ever true or false right now.
            static bool steam_confirm_remove = false;
            ImGui::BeginDisabled(steam_busy);
            const char* add_label = steam_busy ? "Working…"
                                    : in_steam ? "Update Steam Entry"
                                               : "Add to Steam";
            if (good_button(add_label, th, ImVec2(-1, 0))) {
                if (retcomm::steam_is_running()) {
                    steam_confirm_remove = false;
                    ImGui::OpenPopup("Steam is running###steam_running");
                } else {
                    hub.begin_steam_shortcut(row.id, false);
                }
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "Creates a non-Steam shortcut called \"%s\" that starts this game "
                    "through Retro Launcher,\nand fills Steam's library grid, hero and "
                    "icon from this title's cover art.",
                    steam_name.c_str());
            }
            if (in_steam) {
                ImGui::BeginDisabled(steam_busy);
                if (ImGui::Button("Remove from Steam", ImVec2(-1, 0))) {
                    if (retcomm::steam_is_running()) {
                        steam_confirm_remove = true;
                        ImGui::OpenPopup("Steam is running###steam_running");
                    } else {
                        hub.begin_steam_shortcut(row.id, true);
                    }
                }
                ImGui::EndDisabled();
            }
            if (row.boxart_path.empty()) {
                ImGui::TextColored(th.text_muted,
                                   "No cover art for this title yet, so the Steam entry "
                                   "would have none either.");
            }

            if (ImGui::IsPopupOpen("Steam is running###steam_running")) {
                constexpr float kCW = 400.f;
                center_modal_next();
                ImGui::SetNextWindowSizeConstraints(ImVec2(kCW, 0.f), ImVec2(kCW, FLT_MAX));
                if (ImGui::BeginPopupModal("Steam is running###steam_running", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + kCW - 40.f);
                    ImGui::TextWrapped(
                        "Steam holds its shortcut list in memory and writes the whole "
                        "file back when it exits, so anything written now is discarded "
                        "the moment you close Steam.");
                    ImGui::Dummy(ImVec2(0, 4));
                    ImGui::TextWrapped(
                        "Close Steam and try again to be sure, or continue and restart "
                        "Steam straight afterwards so it reloads the file.");
                    ImGui::PopTextWrapPos();
                    ImGui::Dummy(ImVec2(0, 8));
                    const float half = (kCW - 40.f - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                    if (ImGui::Button("Continue anyway", ImVec2(half, 0))) {
                        hub.begin_steam_shortcut(row.id, steam_confirm_remove);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(half, 0))) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }
        }
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(th.text_muted, "ROM / disc");
    if (row.has_rom)
        ImGui::TextWrapped("%s", row.rom_path.c_str());
    else if (row.has_romm) {
        ImGui::TextColored(th.accent, "Available on RomM");
        if (!row.romm_file_name.empty()) ImGui::TextWrapped("%s", row.romm_file_name.c_str());
    } else {
        ImGui::TextColored(th.warn, "No library match");
        if (!row.suggested_rom.empty()) {
            ImGui::TextColored(th.text_muted, "Looking for:");
            ImGui::TextWrapped("%s", row.suggested_rom.c_str());
        }
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(th.text_muted, "RomM");
    if (!row.romm_ready) {
        ImGui::TextColored(th.text_muted, "Not configured — Menu → RomM Sync Settings.");
    } else {
        if (row.has_rom_identity) {
            const char* rom_label = row.has_rom ? "Re-download ROM" : "Download ROM";
            if (romm_button(rom_label, th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::FetchRommRom, row.id);
                ImGui::CloseCurrentPopup();
            }
        }
        if (row.needs_bios) {
            const char* bios_label = row.has_bios ? "Re-download BIOS" : "Download BIOS";
            if (romm_button(bios_label, th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::FetchRommBios, row.id);
                ImGui::CloseCurrentPopup();
            }
        }
        if (!row.has_rom_identity && !row.needs_bios) {
            ImGui::TextColored(th.text_muted, "No RomM download actions for this title.");
        }
    }
}

void draw_detail_manage_game_popup(HubModel& hub, const TitleRow& row, const Theme& th, bool busy) {
    if (!ImGui::IsPopupOpen("Manage Game Data###detail_manage_game")) return;
    // Two columns: the single stack had grown to the height of the window.
    constexpr float kW = 840.f;
    center_modal_next();
    ImGui::SetNextWindowSizeConstraints(ImVec2(kW, 0.f), ImVec2(kW, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(kW, 0.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Manage Game Data###detail_manage_game", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    // Wrap at the content edge rather than a fixed offset: inside a table cell
    // that edge is the column's, so the same push serves both columns.
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextWrapped("%s", row.name.c_str());
    ImGui::Separator();
    ImGui::BeginDisabled(busy);

    // A table rather than two child windows: the row is as tall as its tallest
    // cell, so the columns stay aligned without either being given a height,
    // and the popup still auto-resizes to its content.
    if (ImGui::BeginTable("##manage_cols", 2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(th.text_muted, "Installation");
        draw_manage_install_column(hub, row, th);

        ImGui::TableSetColumnIndex(1);
        draw_manage_files_column(hub, row, th);
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Close", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
    close_modal_on_outside_click();
    ImGui::EndPopup();
}

// `t` is the eased slide from tick_nav_drawer(), already advanced this frame.
void draw_nav_drawer(HubModel& hub, const Theme& th, float t) {
    if (hub.drawer_t <= 0.f) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = nav_drawer_width();
    const ImVec2 vp_max(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);

    // Panel.
    const float x = vp->WorkPos.x - w * (1.f - t);
    ImGui::SetNextWindowPos(ImVec2(x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(w, vp->WorkSize.y));
    if (hub.drawer_focus_pending) ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, th.background2);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.f, 20.f));
    ImGui::Begin("##nav_drawer", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();

        // Shade the page behind the panel. This is painted from the drawer's
        // own draw list — the panel is the front-most window, so it is the one
        // surface guaranteed to land on top of the page. A separate full-screen
        // scrim window used to do this and never worked: a window carrying
        // NoBringToFrontOnFocus sinks below ##hub in ImGui's display order no
        // matter when it is created, so its rect was painted over by the page
        // and its hit box never saw a click. The panel's own strip is left out
        // of the rect rather than drawn over.
        dl->PushClipRectFullScreen();
        dl->AddRectFilled(ImVec2(wp.x + ws.x, vp->WorkPos.y), vp_max,
                          IM_COL32(0, 0, 0, static_cast<int>(165.f * t)));
        dl->PopClipRect();

        // Neon edge, same gradient as the header underline.
        dl->AddRectFilledMultiColor(ImVec2(wp.x + ws.x - 3.f, wp.y), ImVec2(wp.x + ws.x, wp.y + ws.y),
                                    ImGui::ColorConvertFloat4ToU32(th.accent),
                                    ImGui::ColorConvertFloat4ToU32(th.accent),
                                    ImGui::ColorConvertFloat4ToU32(th.good),
                                    ImGui::ColorConvertFloat4ToU32(th.good));

        ImGui::PushStyleColor(ImGuiCol_Text, th.good);
        ImGui::TextUnformatted("Retro Launcher");
        ImGui::PopStyleColor();
        ImGui::SameLine(ws.x - 20.f - 32.f);
        // The close mark is drawn, not typed. As a text label the font's "X"
        // sits off-centre in a square frame (its glyph box is not the frame's)
        // and is too light a stroke at this size to read as a close control.
        {
            constexpr float kClose = 32.f;
            constexpr float kMarkPad = 9.f;    // equal inset on all four sides
            constexpr float kMarkThick = 2.6f;
            if (ImGui::Button("##drawer_close", ImVec2(kClose, kClose))) close_nav_drawer(hub);
            // Span the frame ImGui actually laid out, inset equally, so the
            // mark's box is the button's box minus the same padding on every
            // side — no centre arithmetic to drift.
            const ImVec2 bmin = ImGui::GetItemRectMin();
            const ImVec2 bmax = ImGui::GetItemRectMax();
            const float x0 = bmin.x + kMarkPad, x1 = bmax.x - kMarkPad;
            const float y0 = bmin.y + kMarkPad, y1 = bmax.y - kMarkPad;
            const ImU32 col = ImGui::ColorConvertFloat4ToU32(
                ImGui::IsItemHovered() || ImGui::IsItemFocused() ? th.text : th.text_muted);
            // PathStroke, not AddLine: AddLine adds (+0.5,+0.5) to both
            // endpoints to pixel-align hairlines, which shifted the whole mark
            // down and right inside the frame — the reason it read as
            // off-centre however the endpoints were computed.
            ImDrawList* mark = ImGui::GetWindowDrawList();
            mark->PathLineTo(ImVec2(x0, y0));
            mark->PathLineTo(ImVec2(x1, y1));
            mark->PathStroke(col, 0, kMarkThick);
            mark->PathLineTo(ImVec2(x0, y1));
            mark->PathLineTo(ImVec2(x1, y0));
            mark->PathStroke(col, 0, kMarkThick);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextUnformatted("Menu");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 10.f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 10.f));

        // Tall targets: a thumb on a D-pad wants rows it cannot miss.
        const ImVec2 item_sz(-FLT_MIN, 56.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18.f, 16.f));
        // Hand the ring to the first item, and make the cursor visible: a
        // mouse click on the hamburger hides it (ConfigNavCursorVisibleAuto),
        // and ImGui gates activation on it — so without this, A on a pad did
        // nothing on a drawer that had been opened by mouse.
        if (hub.drawer_focus_pending &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            hub.drawer_focus_pending = false;
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNavCursorVisible(true);
        }
        // Home first: the way back to the platform cards from anywhere,
        // including a settings page that has nothing else to back out to.
        if (accent_button("Home", th, item_sz)) {
            close_settings_pages(hub);
            go_home(hub);
            close_nav_drawer(hub);
        }
        ImGui::Dummy(ImVec2(0, 4.f));
        // Settings stay reachable during Build & Install / library jobs.
        if (ImGui::Button("Library Settings", item_sz)) {
            close_nav_drawer(hub);
            hub.open_settings();
        }
        ImGui::Dummy(ImVec2(0, 4.f));
        if (romm_button("RomM Sync Settings", th, item_sz)) {
            close_nav_drawer(hub);
            hub.open_romm_settings();
        }
        ImGui::Dummy(ImVec2(0, 4.f));
        if (ImGui::Button("Add/Scan Files", item_sz)) {
            close_nav_drawer(hub);
            hub.pending_open_library = true;
        }
#if defined(RETCOMM_HUB_HAVE_PLAY)
        ImGui::Dummy(ImVec2(0, 4.f));
        if (ImGui::Button("Add Core Title…", item_sz)) {
            close_nav_drawer(hub);
            hub.pending_add_core_title = true;
        }
#endif
        ImGui::PopStyleVar();

        // Version block pinned to the bottom (was the Menu modal's footer).
        std::vector<std::pair<std::string, bool>> lines;  // text, warn
        {
            const std::string ver = retcomm::retcomm_app_version();
            const retcomm::RetcommInstallInfo install = retcomm::retcomm_install_info();
            lines.emplace_back("Launcher " + ver, false);
            if (install.self_update_supported && !install.channel_id.empty())
                lines.emplace_back("Channel " + install.channel_id, false);
            std::string tc_line;
            bool tc_upd = false;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                tc_upd = hub.toolchain_update_available;
                tc_line = hub.toolchain_status;
                if (tc_line.empty() && !hub.toolchain_current_version.empty())
                    tc_line = "Toolchain " + hub.toolchain_current_version;
            }
            if (!tc_line.empty()) lines.emplace_back(tc_line, tc_upd);
            lines.emplace_back("F11 fullscreen", false);
            lines.emplace_back("` (tilde) console", false);
        }
        const float line_h = ImGui::GetTextLineHeightWithSpacing();
        const float block_h = line_h * static_cast<float>(lines.size());
        const float bottom_y = ws.y - 20.f - block_h;
        if (ImGui::GetCursorPosY() < bottom_y) ImGui::SetCursorPosY(bottom_y);
        ImGui::PushTextWrapPos(0.f);
        for (const auto& [text, warn] : lines) {
            ImGui::PushStyleColor(ImGuiCol_Text, warn ? th.warn : th.text_muted);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    // Escape or B backs out, from wherever the focus is.
    if (hub.drawer_open && !hub.log_overlay_open &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        close_nav_drawer(hub);

    // So does a click anywhere off the panel. Tested against the panel's own
    // rect: the page behind is inert (BeginDisabled), so the click reaches
    // nothing, and there is no hit box to miss.
    if (hub.drawer_open && !hub.log_overlay_open &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::GetIO().MousePos.x > x + w)
        close_nav_drawer(hub);
}

// Add/Scan Files, as a page rather than a modal: two cards side by side in the
// same frame the settings pages use, so importing a ROM and kicking off a scan
// are things you do *in* the hub instead of through a dialog stacked over it.
void draw_library_panel(HubModel& hub, const Theme& th, SDL_Window* window) {
    ImGui::BeginChild("library_panel", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("ADD / SCAN FILES");
    ImGui::PopStyleColor();
    ImGui::TextWrapped(
        "Bring ROMs, save files, BIOS dumps and texture packs into your library, and "
        "re-scan the folders Retro watches. Imports are copied into the library roots "
        "set in Library Settings.");
    ImGui::Separator();

    const bool busy = hub.job_running.load();
    bool file_busy = false;
    {
        std::lock_guard<std::mutex> lock(hub.file_pick_mu);
        file_busy = hub.file_pick_busy;
    }

    std::vector<std::string> plats;
    {
        std::unordered_set<std::string> seen;
        for (const auto& t : hub.catalog.titles) {
            if (t.platform.empty() || !seen.insert(t.platform).second) continue;
            plats.push_back(t.platform);
        }
        std::sort(plats.begin(), plats.end(), [](const std::string& a, const std::string& b) {
            return std::string(platform_display_name(a)) < std::string(platform_display_name(b));
        });
    }
    if (!hub.library_import_platform.empty() &&
        std::find(plats.begin(), plats.end(), hub.library_import_platform) == plats.end())
        hub.library_import_platform.clear();
    if (!hub.scans_platform_filter.empty() &&
        std::find(plats.begin(), plats.end(), hub.scans_platform_filter) == plats.end())
        hub.scans_platform_filter.clear();

    // Two cards side by side, same frame the platform settings pages use. The
    // footer row is reserved first so neither card runs under Close.
    const float footer_h = ImGui::GetFrameHeight() + 24.f;
    const float gap = 12.f;
    const float card_w = std::max(280.f, (ImGui::GetContentRegionAvail().x - gap) * 0.5f);
    const float card_h = std::max(140.f, ImGui::GetContentRegionAvail().y - footer_h);

    ImGui::BeginChild("library_add", ImVec2(card_w, card_h), ImGuiChildFlags_Borders,
                      page_wheel_flags(hub));
    ImGui::TextColored(th.text_muted, "ADD FILES");
    ImGui::Separator();
    {
        const char* preview = hub.library_import_platform.empty()
                                  ? "Select a Platform"
                                  : platform_display_name(hub.library_import_platform);
        ImGui::BeginDisabled(file_busy || busy);
        if (ImGui::BeginCombo("##library_import_platform", preview)) {
            for (const auto& p : plats) {
                const bool sel = hub.library_import_platform == p;
                if (ImGui::Selectable(platform_display_name(p), sel))
                    hub.library_import_platform = p;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        const bool plat_ok = !hub.library_import_platform.empty();
        ImGui::BeginDisabled(!plat_ok);
        if (ImGui::Button("Import ROM", ImVec2(-1, 0))) {
            const auto exts = rom_exts_for_platform(hub.catalog, hub.library_import_platform);
            begin_file_pick(hub, window, retcomm::hub::FilePickKind::ImportRom,
                            hub.library_import_platform, "ROM files", exts, /*allow_many=*/true);
        }
        if (ImGui::Button("Import save file", ImVec2(-1, 0))) {
            const auto exts = save_exts_for_platform(hub.catalog, hub.library_import_platform);
            begin_file_pick(hub, window, retcomm::hub::FilePickKind::ImportSave,
                            hub.library_import_platform, "Save files", exts, /*allow_many=*/false);
        }
        if (ImGui::Button("Import BIOS", ImVec2(-1, 0))) {
            const auto exts = bios_exts_for_platform(hub.library_import_platform);
            begin_file_pick(hub, window, retcomm::hub::FilePickKind::ImportBios,
                            hub.library_import_platform, "BIOS files", exts, /*allow_many=*/false);
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Copies into your library / saves / BIOS folders, then scans new files for that "
            "platform. Multi-track discs: select the .cue and .bin tracks together.");
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();

    ImGui::SameLine(0.f, gap);
    ImGui::BeginChild("library_scan", ImVec2(0, card_h), ImGuiChildFlags_Borders,
                      page_wheel_flags(hub));
    ImGui::TextColored(th.text_muted, "SCAN");
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "For files you already placed in your folders, or to drop deleted entries.");
    ImGui::PopStyleColor();
    {
        const std::string& filt = hub.scans_platform_filter;
        const char* scope_preview = filt.empty() ? "All Platforms" : platform_display_name(filt);
        const bool any_scan_queued = hub.has_queued_scan();
        auto scan_active = [&](HubJob j) -> bool {
            return busy && hub.job == j && hub.job_platform_filter == filt;
        };
        auto scan_btn_label = [&](HubJob j, const char* idle, const char* active_lbl,
                                  const char* queue_lbl) -> const char* {
            if (scan_active(j)) return active_lbl;
            if (hub.is_job_queued(j, {}, filt)) return "Queued";
            // Another scan already waiting — keep idle label; button is disabled.
            if (any_scan_queued) return idle;
            if (busy) return queue_lbl;
            return idle;
        };

        ImGui::BeginDisabled(file_busy);
        if (ImGui::BeginCombo("##library_advanced_scope", scope_preview)) {
            if (ImGui::Selectable("All Platforms", hub.scans_platform_filter.empty()))
                hub.scans_platform_filter.clear();
            for (const auto& p : plats) {
                const bool sel = hub.scans_platform_filter == p;
                if (ImGui::Selectable(platform_display_name(p), sel))
                    hub.scans_platform_filter = p;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        // One queued library scan max (any type) — avoid spamming the backlog.
        ImGui::BeginDisabled(file_busy || scan_active(HubJob::ScanRoms) || any_scan_queued);
        if (ImGui::Button(scan_btn_label(HubJob::ScanRoms, "Scan new files", "Scanning…",
                                         "Queue Scan new files"),
                          ImVec2(-1, 0))) {
            hub.pending_scan_missing_rom_id.clear();
            hub.start_job(HubJob::ScanRoms);
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(file_busy || scan_active(HubJob::PurgeMissingFiles) ||
                             any_scan_queued);
        if (ImGui::Button(scan_btn_label(HubJob::PurgeMissingFiles, "Clean missing files",
                                         "Cleaning…", "Queue Clean missing files"),
                          ImVec2(-1, 0))) {
            hub.start_job(HubJob::PurgeMissingFiles);
        }
        ImGui::EndDisabled();

        // Always open the confirmation modal (including when queueing).
        ImGui::BeginDisabled(file_busy || scan_active(HubJob::FullScanRoms) || any_scan_queued);
        if (ImGui::Button(scan_btn_label(HubJob::FullScanRoms, "Full rebuild index…", "Scanning…",
                                         "Queue Full rebuild index…"),
                          ImVec2(-1, 0)))
            ImGui::OpenPopup("Full rebuild###confirm_full_rescan");
        ImGui::EndDisabled();
    }

    // Same ID scope as the OpenPopup above: a popup opened inside a child
    // window is keyed to that child, so beginning it outside never finds it.
    if (ImGui::BeginPopupModal("Full rebuild###confirm_full_rescan", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 360.f);
        if (hub.scans_platform_filter.empty()) {
            ImGui::TextWrapped(
                "Re-hash every ROM and BIOS candidate and rebuild the indexes from scratch. "
                "On a large collection this often takes many minutes and will keep the "
                "worker busy until it finishes.");
        } else {
            ImGui::TextWrapped(
                "Re-hash every ROM and BIOS candidate for %s and rebuild that platform's "
                "index from scratch. Other platforms are left alone. On a large collection "
                "this often takes many minutes.",
                platform_display_name(hub.scans_platform_filter));
        }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8));
        const bool full_busy = hub.job_running.load();
        const bool any_scan_queued = hub.has_queued_scan();
        const bool this_active = full_busy && hub.job == HubJob::FullScanRoms &&
                                 hub.job_platform_filter == hub.scans_platform_filter;
        const bool this_queued =
            hub.is_job_queued(HubJob::FullScanRoms, {}, hub.scans_platform_filter);
        const char* rebuild_lbl = this_active  ? "Scanning…"
                                  : this_queued ? "Queued"
                                  : full_busy   ? "Queue Rebuild"
                                                : "Rebuild";
        ImGui::BeginDisabled(this_active || this_queued || any_scan_queued);
        if (accent_button(rebuild_lbl, th, ImVec2(120, 0))) {
            hub.pending_scan_missing_rom_id.clear();
            hub.start_job(HubJob::FullScanRoms);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        close_modal_on_outside_click();
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, 12));
    if (ImGui::Button("Close", ImVec2(160, 0))) hub.show_library_panel = false;


    ImGui::EndChild();
}

fs::path find_hub_logo_path() {
    // Packaged icon location + source-tree assets/retcomm.png via walk.
    std::error_code ec;
    auto try_file = [&](const fs::path& p) -> fs::path {
        if (p.empty()) return {};
        if (fs::is_regular_file(p, ec)) return p;
        return {};
    };
    if (const char* appdir = std::getenv("APPDIR")) {
        const fs::path ad(appdir);
        if (auto h = try_file(ad / "usr" / "share" / "icons" / "hicolor" / "512x512" / "apps" /
                             "retcomm.png");
            !h.empty())
            return h;
        if (auto h = try_file(ad / "usr" / "share" / "retcomm" / "retcomm.png"); !h.empty())
            return h;
    }
    if (const char* base = SDL_GetBasePath()) {
        fs::path walk(base);
        for (int i = 0; i < 6 && !walk.empty(); ++i) {
            if (auto h = try_file(walk / "assets" / "retcomm.png"); !h.empty()) return h;
            if (auto h = try_file(walk / "retcomm.png"); !h.empty()) return h;
            if (auto h = try_file(walk / "share" / "retcomm" / "retcomm.png"); !h.empty())
                return h;
            if (auto h = try_file(walk / "share" / "icons" / "hicolor" / "512x512" / "apps" /
                                  "retcomm.png");
                !h.empty())
                return h;
            walk = walk.parent_path();
        }
    }
    return {};
}

void draw_welcome_panel(BoxartCache& boxart, const Theme& th) {
    ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float logo_max = std::min(220.f, std::min(avail.x - 24.f, avail.y * 0.45f));

    const fs::path logo = find_hub_logo_path();
    const BoxartTexture* tex =
        logo.empty() ? nullptr : boxart.get("hub:logo", logo);

    float block_h = ImGui::GetTextLineHeight() * 2.5f;
    float logo_w = 0.f, logo_h = 0.f;
    if (tex && tex->gl_id && tex->width > 0 && tex->height > 0) {
        const ImVec2 fit = contain_size(static_cast<float>(tex->width),
                                        static_cast<float>(tex->height), logo_max, logo_max);
        logo_w = fit.x;
        logo_h = fit.y;
        block_h += logo_h + 16.f;
    }

    const float start_y = ImGui::GetCursorPosY() + std::max(0.f, (avail.y - block_h) * 0.35f);
    ImGui::SetCursorPosY(start_y);

    if (tex && tex->gl_id && logo_w > 0.f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (avail.x - logo_w) * 0.5f));
        ImGui::Image((ImTextureID)(intptr_t)tex->gl_id, ImVec2(logo_w, logo_h));
        ImGui::Dummy(ImVec2(0, 12.f));
    }

    const char* welcome = "Welcome to Retro";
    const ImVec2 tw = ImGui::CalcTextSize(welcome);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (avail.x - tw.x) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, th.accent);
    ImGui::TextUnformatted(welcome);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 6.f));
    const char* hint = "Choose a platform, then a title.";
    const ImVec2 hw = ImGui::CalcTextSize(hint);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (avail.x - hw.x) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted(hint);
    ImGui::PopStyleColor();
    ImGui::EndChild();
}

// Left column of a title page: what the title *is*. Nothing here is pressable —
// every control lives in the actions column, so a pad reaches them in one place.
void draw_title_info_panel(HubModel& hub, const TitleRow& row, BoxartCache& boxart,
                           const Theme& th) {
    ImGui::BeginChild("title_info", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      page_wheel_flags(hub));
    if (hub.detail_scroll_top) ImGui::SetScrollY(0.f);

    // Cover art, centred and given real size — this column exists to be looked at.
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const BoxartTexture* tex =
        row.boxart_path.empty() ? nullptr : boxart.get(row.id, row.boxart_path);
    if (tex && tex->gl_id && tex->width > 0 && tex->height > 0) {
        const ImVec2 fit = contain_size(static_cast<float>(tex->width),
                                        static_cast<float>(tex->height),
                                        std::min(avail_w, 380.f), 440.f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (avail_w - fit.x) * 0.5f));
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex->gl_id)), fit);
        ImGui::Dummy(ImVec2(0, 14));
    }

    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextWrapped("%s", row.name.c_str());
    ImGui::SetWindowFontScale(1.f);

    ImGui::TextColored(th.text_muted, "%s \xC2\xB7 %s", platform_display_name(row.platform),
                       row.kind.c_str());
    ImGui::TextColored(chip_color(row, th), "%s", chip_label(row));

    // App status: installed tag, a stalled install folder, or what a fresh
    // install would restore.
    ImGui::Dummy(ImVec2(0, 12));
    ImGui::TextColored(th.text_muted, "App");
    if (row.installed) {
        const std::string& shown_tag =
            !row.release_compare_tag.empty() ? row.release_compare_tag : row.installed_tag;
        ImGui::TextColored(th.good, "Installed %s%s",
                           shown_tag.empty() ? "" : shown_tag.c_str(),
                           row.runtime == "wine" ? " (Wine)" : "");
        if (row.update_available)
            ImGui::TextColored(th.warn, "Update available: %s", row.latest_tag.c_str());
    } else if (row.install_dir_present) {
        ImGui::TextColored(th.warn, "Install folder present — launch binary not found");
        if (!row.install_issue.empty()) ImGui::TextWrapped("%s", row.install_issue.c_str());
        if (!row.expected_binary.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Looking for executable \"%s\". Use Reinstall to clear the install folder "
                "and start over, or Manage Game Data → Open Folder to fix setup manually.",
                row.expected_binary.c_str());
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::TextColored(th.text_muted, "Not installed");
        if (row.has_preserved_state) {
            ImGui::TextColored(th.focus, "Preserved saves/config ready");
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            ImGui::TextWrapped(
                "Previous uninstall kept user data under preserved/. The next install "
                "will restore it into the new release.");
            ImGui::PopStyleColor();
        }
    }

    ImGui::Dummy(ImVec2(0, 12));
    const char* author_label =
        (row.kind == "decomp") ? "Decomp Author" : "Recomp Author";
    ImGui::TextColored(th.text_muted, "%s", author_label);
    if (!row.author.empty())
        ImGui::Text("%s", row.author.c_str());
    else
        ImGui::TextColored(th.text_muted, "(unknown)");
    if (!row.author_notes.empty()) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(th.text_muted, "Author's Notes");
        ImGui::TextWrapped("%s", row.author_notes.c_str());
    }

    if (!row.description.empty()) {
        ImGui::Dummy(ImVec2(0, 12));
        ImGui::TextColored(th.text_muted, "About");
        ImGui::TextWrapped("%s", row.description.c_str());
    }

    ImGui::EndChild();
}

// Where a game runs from, which is where its mods/ tree lives. For a build
// install that is the release dir; for an AppImage it is the data dir AppRun
// cds into, not the read-only payload.
fs::path title_game_dir(const HubModel& hub, const TitleRow& row) {
    if (row.binary_path.empty()) return {};
    const fs::path bin(row.binary_path);
    if (bin.filename() == "AppRun" && !row.install_root.empty()) {
        if (const retcomm::Title* t = hub.catalog.find(row.id))
            return retcomm::appimage_data_dir(*t, fs::path(row.install_root));
    }
    return bin.parent_path();
}

// The game's mod packages, read straight from their manifests, laid out the way
// the engines' own mods window is: a grouped feature list on the left, the
// selected feature's detail and settings on the right. A player moving between
// the two should not have to relearn the screen.
//
// Toggling writes mods/state.toml; the engine still re-resolves at boot, so a
// selection it refuses (guard mismatch, conflicting features) fails there
// rather than being silently half-applied here.
struct ModsPageState {
    retcomm::ModScanResult scan;
    std::string title_id;
    std::string sel_package;      // which feature the right panel describes
    std::string sel_feature;
    char search[96]{};
    // Hand-offs between the two panels. ImGui's directional nav only accepts a
    // candidate lying in the pressed direction's quadrant, so a row near the
    // bottom of the list can never reach a setting near the top of the detail
    // column: nothing overlaps it horizontally. The panels therefore pass the
    // ring to each other explicitly.
    bool focus_detail = false;    // set by the list, consumed the same frame
    bool focus_list = false;      // set by the detail column, consumed next frame
    bool pending_rescan = false;  // a write landed; re-read once the panel is done
};

// Right / Left as a player produces them: d-pad, left stick, or the arrow keys
// when driving the same UI from a keyboard.
bool nav_right_pressed() {
    return ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) ||
           ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight, false) ||
           ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
}

bool nav_left_pressed() {
    return ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) ||
           ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft, false) ||
           ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false);
}

ModsPageState& mods_page_state() {
    static ModsPageState s;
    return s;
}

void refresh_mods_page(HubModel& hub, const TitleRow& row) {
    ModsPageState& st = mods_page_state();
    st.scan = retcomm::scan_game_mods(title_game_dir(hub, row), fs::path(row.install_root));
    st.title_id = row.id;
}

// Every (package, feature) pair, flattened and grouped the way the left panel
// shows them. Kept as indices so a rescan cannot leave dangling pointers.
struct ModRowRef {
    size_t pkg = 0;
    size_t feat = 0;              // npos for an all-or-nothing package
    static constexpr size_t kNoFeature = static_cast<size_t>(-1);
};

bool mod_row_matches(const retcomm::ModPackageInfo& p, const retcomm::ModFeatureInfo* f,
                     const std::string& needle) {
    if (needle.empty()) return true;
    auto has = [&](const std::string& hay) {
        return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                           [](char a, char b) {
                               return std::tolower(static_cast<unsigned char>(a)) ==
                                      std::tolower(static_cast<unsigned char>(b));
                           }) != hay.end();
    };
    if (has(p.name) || has(p.id)) return true;
    if (f && (has(f->name) || has(f->id) || has(f->group))) return true;
    return false;
}

void draw_mods_page(HubModel& hub, const Theme& th) {
    ModsPageState& st = mods_page_state();

    TitleRow row;
    bool have_row = false;
    {
        std::lock_guard<std::mutex> lock(hub.mu);
        have_row = hub.selected >= 0 && hub.selected < static_cast<int>(hub.rows.size());
        if (have_row) row = hub.rows[static_cast<size_t>(hub.selected)];
    }
    if (!have_row) {
        hub.show_mods_page = false;
        return;
    }
    if (st.title_id != row.id) refresh_mods_page(hub, row);

    ImGui::BeginChild("mods_page", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("MODS");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(row.name.c_str());
    ImGui::TextColored(th.text_muted, "%s", st.scan.root.string().c_str());
    ImGui::Separator();

    auto set_all = [&](bool on) {
        std::string err;
        bool ok = true;
        for (const retcomm::ModPackageInfo& p : st.scan.packages) {
            if (p.has_features()) {
                for (const retcomm::ModFeatureInfo& f : p.features)
                    ok &= retcomm::set_mod_enabled(title_game_dir(hub, row), p.id, f.id, on, &err);
            } else {
                ok &= retcomm::set_mod_enabled(title_game_dir(hub, row), p.id, {}, on, &err);
            }
        }
        if (!ok) hub.append_log("Mod toggle failed: " + err);
        refresh_mods_page(hub, row);
    };

    ImGui::BeginDisabled(st.scan.packages.empty());
    if (ImGui::Button("Enable all", ImVec2(120, 0))) set_all(true);
    ImGui::SameLine();
    if (ImGui::Button("Disable all", ImVec2(120, 0))) set_all(false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Rescan", ImVec2(100, 0))) refresh_mods_page(hub, row);
    ImGui::SameLine();
    ImGui::BeginDisabled(!st.scan.root_exists);
    if (ImGui::Button("Open Folder", ImVec2(130, 0))) {
        std::string err;
        if (!retcomm::open_path_in_file_manager(st.scan.root, &err))
            hub.append_log("Open folder failed: " + err);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.f);
    hub_input_text_hint("##mods_search", "Search features, groups, packages…", st.search,
                        sizeof(st.search));
    ImGui::Dummy(ImVec2(0, 6));

    const std::string needle = st.search;
    constexpr float kListMinW = 300.f;
    constexpr float kDetailMinW = 320.f;
    const float total_w = ImGui::GetContentRegionAvail().x;
    const float gap = 12.f;
    float list_w = std::clamp(total_w * 0.40f, kListMinW,
                              std::max(kListMinW, total_w - kDetailMinW - gap));

    // ---- left: features, grouped -------------------------------------------
    // NavFlattened, like every other page's panels: a non-flattened child is a
    // single nav item, so a pad lands on the border and the D-pad stops there
    // until A is pressed to step inside. Flattened, Down walks straight from the
    // toolbar into the rows and Right crosses into the detail panel.
    ImGui::BeginChild("mods_list", ImVec2(list_w, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      page_wheel_flags(hub));
    if (!st.scan.root_exists) {
        ImGui::TextWrapped("No mods folder yet. The engine creates mods/bundled at build time; "
                           "mods/installed is where packages are added.");
    } else if (st.scan.packages.empty()) {
        ImGui::TextColored(th.text_muted, "No mod packages installed for this title.");
    }
    if (st.scan.packages.empty() && st.scan.unstaged_manifests > 0) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(th.warn,
                           "%d manifest(s) sit in this title's build tree but were never staged "
                           "into the release, so the game cannot load them either:",
                           st.scan.unstaged_manifests);
        for (const fs::path& u : st.scan.unstaged_roots)
            ImGui::TextColored(th.text_muted, "%s", u.string().c_str());
        ImGui::TextColored(th.text_muted, "Generate & Rebuild restages a build's own packages.");
        ImGui::PopTextWrapPos();
    }

    // Group headings in first-seen order, so a manifest's own ordering shows.
    std::vector<std::string> groups;
    for (const retcomm::ModPackageInfo& p : st.scan.packages) {
        if (p.has_features()) {
            for (const retcomm::ModFeatureInfo& f : p.features)
                if (std::find(groups.begin(), groups.end(), f.group) == groups.end())
                    groups.push_back(f.group);
        } else if (std::find(groups.begin(), groups.end(), "Packages") == groups.end()) {
            groups.push_back("Packages");
        }
    }
    std::sort(groups.begin(), groups.end());

    // Opening the page hands the ring to the first togglable row, the way the
    // grid hands it to the selected card. Only one row may claim it.
    bool focus_handed = false;

    for (const std::string& g : groups) {
        std::vector<ModRowRef> rows;
        for (size_t pi = 0; pi < st.scan.packages.size(); ++pi) {
            const retcomm::ModPackageInfo& p = st.scan.packages[pi];
            if (p.has_features()) {
                for (size_t fi = 0; fi < p.features.size(); ++fi) {
                    if (p.features[fi].group != g) continue;
                    if (!mod_row_matches(p, &p.features[fi], needle)) continue;
                    rows.push_back({pi, fi});
                }
            } else if (g == "Packages" && mod_row_matches(p, nullptr, needle)) {
                rows.push_back({pi, ModRowRef::kNoFeature});
            }
        }
        if (rows.empty()) continue;

        ImGui::PushID(g.c_str());
        if (ImGui::CollapsingHeader(g.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const ModRowRef& r : rows) {
                const retcomm::ModPackageInfo& p = st.scan.packages[r.pkg];
                const bool is_feature = r.feat != ModRowRef::kNoFeature;
                const retcomm::ModFeatureInfo* f = is_feature ? &p.features[r.feat] : nullptr;
                const std::string fid = is_feature ? f->id : std::string();

                ImGui::PushID(static_cast<int>(r.pkg * 1000 + (is_feature ? r.feat : 999)));
                bool on = is_feature ? f->enabled : p.enabled;
                if (hub.mods_focus_pending && !focus_handed && !p.builtin &&
                    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                    hub.mods_focus_pending = false;
                    focus_handed = true;
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                // A built-in provider's mods are the game's to switch; a
                // checkbox here would write state.toml, which it never reads.
                ImGui::BeginDisabled(p.builtin);
                if (ImGui::Checkbox("##en", &on) && !p.builtin) {
                    std::string err;
                    if (retcomm::set_mod_enabled(title_game_dir(hub, row), p.id, fid, on, &err))
                        refresh_mods_page(hub, row);
                    else
                        hub.append_log("Mod toggle failed: " + err);
                    ImGui::EndDisabled();
                    ImGui::PopID();
                    break;          // the vectors were just rebuilt under us
                }
                ImGui::EndDisabled();
                // The right panel follows the ring, the way the library grid's
                // detail follows the focused card: a pad walking the list should
                // not have to also press A to find out what a row does. Down/Up
                // land on the checkbox, Right steps onto the label, so both
                // items claim the row.
                const bool checkbox_focused = ImGui::IsItemFocused();
                ImGui::SameLine();
                // Two lines per row, as the engines' list shows: what the
                // feature is, and which package it came from.
                const bool selected = st.sel_package == p.id && st.sel_feature == fid;
                const std::string label = (is_feature ? f->name : p.name) + "##row";
                const bool clicked = ImGui::Selectable(label.c_str(), selected,
                                                       ImGuiSelectableFlags_AllowOverlap);
                const bool row_focused = ImGui::IsItemFocused();
                if (clicked || checkbox_focused || row_focused) {
                    st.sel_package = p.id;
                    st.sel_feature = fid;
                }
                // Left out of the settings column returns the ring to the row it
                // was reading, not to the top of the list. SetKeyboardFocusHere()
                // cannot do this: it only queues a request, and ImGui::SetNavWindow()
                // clears every pending request — which the still-focused control in
                // the column, drawn after this panel, calls each frame from its own
                // NavProcessItem(). So the request never survives to be applied. Set
                // the focus outright instead.
                if (st.focus_list && selected) {
                    st.focus_list = false;
                    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                    ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
                    ImGui::SetNavCursorVisible(true);
                }
                // The label runs to the panel edge, so Right has nowhere else to
                // go inside the list: cross into this feature's settings.
                if (row_focused && nav_right_pressed()) st.focus_detail = true;
                ImGui::Indent(ImGui::GetFrameHeight() + 8.f);
                ImGui::TextColored(th.text_muted, "%s", p.name.c_str());
                ImGui::Unindent(ImGui::GetFrameHeight() + 8.f);
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }
    for (const std::string& e : st.scan.errors) ImGui::TextColored(th.warn, "%s", e.c_str());
    ImGui::EndChild();

    // ---- right: the selected feature ---------------------------------------
    ImGui::SameLine(0.f, gap);
    ImGui::BeginChild("mods_detail", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      page_wheel_flags(hub));
    const retcomm::ModPackageInfo* sp = nullptr;
    const retcomm::ModFeatureInfo* sf = nullptr;
    for (const retcomm::ModPackageInfo& p : st.scan.packages) {
        if (p.id != st.sel_package) continue;
        sp = &p;
        for (const retcomm::ModFeatureInfo& f : p.features)
            if (f.id == st.sel_feature) sf = &f;
        break;
    }
    if (!sp) {
        ImGui::TextColored(th.text_muted, "Select a feature to see what it does.");
    } else {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(th.accent, "%s", sf ? sf->name.c_str() : sp->name.c_str());
        if (sf && !sf->group.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(th.text_muted, "%s", sf->group.c_str());
        }
        ImGui::TextColored(th.text_muted, "From %s %s", sp->name.c_str(), sp->version.c_str());
        if (!sp->author.empty()) ImGui::TextColored(th.text_muted, "by: %s", sp->author.c_str());
        ImGui::Dummy(ImVec2(0, 6));
        const std::string& desc = sf ? sf->description : sp->description;
        if (!desc.empty()) ImGui::TextWrapped("%s", desc.c_str());

        ImGui::Dummy(ImVec2(0, 6));
        const bool on = sf ? sf->enabled : sp->enabled;
        ImGui::TextColored(on ? th.good : th.text_muted, "%s", on ? "Enabled" : "Disabled");
        if (sf && sf->channel == "experimental")
            ImGui::TextColored(th.warn, "Experimental — default-off and not validated here.");
        if (sp->builtin) {
            ImGui::TextColored(th.warn,
                               "Built into the game. Switch it in the game's own Mods menu — "
                               "it keeps its own state, not mods/state.toml.");
        }
        ImGui::TextColored(th.text_muted, "%s · %s", retcomm::mod_origin_name(sp->origin),
                           sp->manifest.string().c_str());

        // Settings. Read-only: their values live in a [feature.values]
        // sub-table that the surgical enable/disable writer does not touch, and
        // a half-written option is worse than one edited where the game already
        // edits it.
        std::vector<const retcomm::ModOptionInfo*> opts;
        for (const retcomm::ModOptionInfo& o : sp->options)
            if (!sf || o.feature_id == sf->id) opts.push_back(&o);
        if (!opts.empty()) {
            ImGui::Dummy(ImVec2(0, 10));
            ImGui::Separator();
            ImGui::TextColored(th.accent, "%s",
                               sf && !sf->group.empty() ? sf->group.c_str() : "Settings");
            const std::string feat_for_opts = sf ? sf->id : std::string();
            // The rescan is deferred: it reassigns st.scan, and everything the
            // rest of this panel is walking — sp, the option, its choices, the
            // opts vector — points into the packages it replaces.
            auto write_option = [&](const retcomm::ModOptionInfo& o, const std::string& v) {
                std::string err;
                if (retcomm::set_mod_option(title_game_dir(hub, row), sp->id, feat_for_opts,
                                            o.id, v, &err))
                    st.pending_rescan = true;
                else
                    hub.append_log("Mod option failed: " + err);
            };

            constexpr float kOptW = 220.f;
            for (size_t oi = 0; oi < opts.size(); ++oi) {
                const retcomm::ModOptionInfo& o = *opts[oi];
                ImGui::PushID(static_cast<int>(oi));
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(o.label.c_str());
                ImGui::SameLine();
                const float right = ImGui::GetWindowContentRegionMax().x;
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - kOptW));
                ImGui::SetNextItemWidth(kOptW);

                // The list handed the ring over; land it on the first setting.
                // Consume the request either way, so a feature whose settings
                // are all read-only does not leave it armed for a later frame.
                if (st.focus_detail && oi == 0) {
                    st.focus_detail = false;
                    if (!sp->builtin) {
                        ImGui::SetKeyboardFocusHere();
                        ImGui::SetNavCursorVisible(true);
                    }
                }
                // A built-in provider keeps its own state, so its rows are
                // readouts; everything else is editable.
                ImGui::BeginDisabled(sp->builtin);
                const std::string cur = o.value.empty() ? o.default_value : o.value;
                if (!o.choices.empty()) {
                    // Show the choice's label, write its value.
                    const char* preview = cur.c_str();
                    for (const retcomm::ModChoice& c : o.choices)
                        if (c.value == cur) preview = c.label.c_str();
                    if (ImGui::BeginCombo("##opt", preview)) {
                        for (const retcomm::ModChoice& c : o.choices) {
                            const bool selected = c.value == cur;
                            if (ImGui::Selectable(c.label.c_str(), selected) && !selected)
                                write_option(o, c.value);
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                } else if (o.type == "boolean") {
                    bool on = cur == "true" || cur == "1" || cur == "yes";
                    if (ImGui::Checkbox("##opt", &on)) write_option(o, on ? "true" : "false");
                } else if (o.type == "integer") {
                    int v = std::atoi(cur.c_str());
                    const int lo = o.has_range ? static_cast<int>(o.min) : 0;
                    const int hi = o.has_range ? static_cast<int>(o.max) : 0;
                    if (ImGui::InputInt("##opt", &v, o.step ? static_cast<int>(o.step) : 1, 0,
                                        ImGuiInputTextFlags_EnterReturnsTrue)) {
                        if (o.has_range) v = std::clamp(v, lo, hi);
                        write_option(o, std::to_string(v));
                    }
                } else {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
                    if (hub_input_text("##opt", buf, sizeof(buf),
                                       ImGuiInputTextFlags_EnterReturnsTrue))
                        write_option(o, buf);
                }
                ImGui::EndDisabled();
                // Settings sit hard against the panel's right edge, so Left has
                // nowhere to go inside the column: go back to the feature list.
                if (ImGui::IsItemFocused() && nav_left_pressed()) st.focus_list = true;

                if (o.type == "integer" && o.has_range)
                    ImGui::TextColored(th.text_muted, "%ld\xe2\x80\x93%ld", o.min, o.max);
                if (!o.description.empty())
                    ImGui::TextColored(th.text_muted, "%s", o.description.c_str());
                ImGui::PopID();
            }
            if (sp->builtin) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::TextColored(th.text_muted,
                                   "Set these in the game's own Mods menu \xe2\x80\x94 the "
                                   "provider keeps their values, not mods/state.toml.");
            }
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    // Unclaimed because the selected feature has no settings at all: forget it
    // rather than let it fire on whatever the next selection turns out to be.
    st.focus_detail = false;
    ImGui::EndChild();

    if (st.pending_rescan) {
        st.pending_rescan = false;
        refresh_mods_page(hub, row);
    }
}

// Right column of a title page: every control that used to live in the detail
// strip beside the grid — Play/Install, disc, saves, data, texture packs, BIOS.
void draw_title_actions_panel(HubModel& hub, const TitleRow& row, const Theme& th,
                              SDL_Window* window) {
    const bool job_busy = hub.job_running.load();
    const bool install_op = job_busy && retcomm::hub::hub_job_is_install(hub.job);
    // Any exclusive worker job blocks starting another install/scan/mutate.
    const bool block_title_mutate = job_busy;

    ImGui::BeginChild("title_actions", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                      page_wheel_flags(hub));
    if (hub.detail_scroll_top) ImGui::SetScrollY(0.f);

    // The ring lands on the primary action, so opening a title and pressing A
    // plays it. Same hand-off the grid and the drawer use: the child arrives a
    // frame after the window, so ImGui's own default focus never fires.
    if (hub.detail_focus_pending &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        hub.detail_focus_pending = false;
        ImGui::SetKeyboardFocusHere();
        ImGui::SetNavCursorVisible(true);
    }

    // Primary actions.
    const float btn_w = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
    if (row.installed) {
        // OpenBIOS-only build + retail dump selected → rebuild with SCPH + OpenBIOS.
        const bool reinstall_w_bios =
            row.built_with_openbios && row.supports_local_build &&
            row.bios_choice != retcomm::kOpenBiosChoice && !row.bios_choice.empty();
        if (reinstall_w_bios) {
            // Same queue pattern as Update — GenerateRebuild is hub_job_is_queueable.
            const bool reb_active =
                job_busy && hub.job == HubJob::GenerateRebuild && hub.job_title_id == row.id;
            const bool reb_queued = hub.is_job_queued(HubJob::GenerateRebuild, row.id);
            const char* reb_label =
                reb_active  ? "Reinstalling…"
                : reb_queued ? "Queued"
                : job_busy   ? "Queue Reinstall w BIOS"
                             : "Reinstall w BIOS";
            ImGui::BeginDisabled(reb_active || reb_queued ||
                                 (!job_busy && block_title_mutate));
            const bool reb_click = good_button(reb_label, th, ImVec2(btn_w, 0));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "This install was built with OpenBIOS only.\n"
                    "Regenerate and rebuild with your selected retail BIOS "
                    "(SCPH1001) and OpenBIOS.\n"
                    "Can be queued while another install/update is running.");
            }
            if (reb_click) hub.start_job(HubJob::GenerateRebuild, row.id);
            ImGui::EndDisabled();
        } else {
            // Play (+ update preflight) uses launch_worker — stays free during Install.
            ImGui::BeginDisabled(hub.launch_running.load());
            const bool play_clicked =
                row.update_available ? ImGui::Button("Play", ImVec2(btn_w, 0))
                                     : good_button("Play", th, ImVec2(btn_w, 0));
            if (play_clicked) {
                if (hub.cfg.check_updates_before_launch) {
                    const auto* t = hub.catalog.find(row.id);
                    if (t && !t->release.github.empty())
                        hub.start_job(HubJob::CheckLaunchUpdate, row.id);
                    else
                        hub.start_job(HubJob::Launch, row.id);
                } else {
                    hub.start_job(HubJob::Launch, row.id);
                }
            }
            ImGui::EndDisabled();
        }
        ImGui::SameLine(0, 8);
        {
            const bool upd_active =
                job_busy && hub.job == HubJob::Update && hub.job_title_id == row.id;
            const bool upd_queued = hub.is_job_queued(HubJob::Update, row.id);
            const char* upd_label =
                upd_active  ? "Updating…"
                : upd_queued ? "Queued"
                : job_busy   ? "Queue Update"
                             : "Update";
            ImGui::BeginDisabled(upd_active || upd_queued ||
                                 (!job_busy && block_title_mutate));
            const bool upd_click = row.update_available
                                       ? good_button(upd_label, th, ImVec2(btn_w, 0))
                                       : ImGui::Button(upd_label, ImVec2(btn_w, 0));
            if (upd_click) hub.start_job(HubJob::Update, row.id);
            ImGui::EndDisabled();
        }
    } else if (row.supports_local_build) {
        // Default Install uses local generate+cmake whenever a build recipe
        // exists (install_title_auto); release zip is source, not a Play shortcut.
        if (!row.has_rom) {
            ImGui::PushStyleColor(ImGuiCol_Text, th.warn);
            ImGui::TextWrapped(
                "No verified .cue / ROM in your library yet. Install will prompt to "
                "rescan %s, import files, or download from RomM.",
                platform_display_name(row.platform));
            ImGui::PopStyleColor();
        }
        const bool inst_active =
            job_busy && retcomm::hub::hub_job_is_install(hub.job) &&
            hub.job_title_id == row.id && hub.job != HubJob::Update;
        const bool inst_queued = hub.is_job_queued(HubJob::Install, row.id);
        const char* inst_label =
            inst_active ? (row.install_dir_present ? "Reinstalling…" : "Installing…")
            : inst_queued ? "Queued"
            : job_busy
                ? (row.install_dir_present ? "Queue Reinstall" : "Queue Install")
                : (row.install_dir_present ? "Reinstall" : "Install");
        ImGui::BeginDisabled(inst_active || inst_queued ||
                             (!job_busy && block_title_mutate));
        if (good_button(inst_label, th, ImVec2(-1, 0))) hub.begin_install(row.id);
        ImGui::EndDisabled();
    } else {
        const bool inst_active =
            job_busy && retcomm::hub::hub_job_is_install(hub.job) &&
            hub.job_title_id == row.id && hub.job != HubJob::Update;
        const bool inst_queued = hub.is_job_queued(HubJob::Install, row.id);
        const char* inst_label =
            inst_active ? (row.install_dir_present ? "Reinstalling…" : "Installing…")
            : inst_queued ? "Queued"
            : job_busy
                ? (row.install_dir_present ? "Queue Reinstall" : "Queue Install")
                : (row.install_dir_present ? "Reinstall" : "Install");
        ImGui::BeginDisabled(inst_active || inst_queued ||
                             (!job_busy && block_title_mutate));
        if (good_button(inst_label, th, ImVec2(-1, 0))) hub.begin_install(row.id);
        ImGui::EndDisabled();
    }

    // Skip Launcher, directly under Play: it changes what pressing Play lands
    // you in. Per-install and engine-specific (settings.toml [launcher] for
    // psxrecomp, config.ini [General] for snesrecomp), so it is read from and
    // written to this title's own files rather than the platform defaults.
    if (row.installed) {
        const fs::path game_dir = title_game_dir(hub, row);
        if (!game_dir.empty()) {
            bool skip = retcomm::title_skip_launcher(game_dir, row.platform);
            if (ImGui::Checkbox("Skip Launcher", &skip)) {
                std::string err;
                if (!retcomm::set_title_skip_launcher(game_dir, row.platform, skip, &err)) {
                    hub.append_log("Skip Launcher failed: " + err);
                    hub.set_status("Skip Launcher failed");
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "Boot straight into the game, past the engine's own launcher screen.\n"
                    "Stored with this install, not the platform defaults.");
            }
        }
    }

    // Disc selection under Play (multi-disc sets only). Writes the install's
    // settings.toml [disc], which is what the runtime boots, so the choice
    // holds whether the game is started from here or run directly.
    if (row.disc_choice_paths.size() > 1) {
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::TextColored(th.text_muted, "Disc");
        const char* preview =
            (row.selected_disc_index >= 0 &&
             row.selected_disc_index < static_cast<int>(row.disc_choice_labels.size()))
                ? row.disc_choice_labels[static_cast<size_t>(row.selected_disc_index)].c_str()
                : "(select)";
        ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled(hub.launch_running.load());
        if (ImGui::BeginCombo("##disc_choice", preview)) {
            for (size_t i = 0; i < row.disc_choice_paths.size(); ++i) {
                const bool selected = static_cast<int>(i) == row.selected_disc_index;
                const std::string item =
                    row.disc_choice_labels[i] + "##disc" + std::to_string(i);
                if (ImGui::Selectable(item.c_str(), selected)) {
                    std::string err;
                    if (!hub.set_title_preferred_disc(row.id, row.disc_choice_paths[i], &err))
                        hub.append_log("Disc selection failed: " + err);
                    else
                        hub.set_status("Disc set to " + row.disc_choice_labels[i]);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", row.disc_choice_paths[i].c_str());
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (!row.installed) {
            ImGui::TextColored(th.text_muted,
                               "Applies to the install once this title is installed.");
        }
    }

    // Slot selection under Play; install lifecycle in Manage Game Data.
    if (row.installed) {
        ImGui::Dummy(ImVec2(0, 8));
        draw_detail_save_controls(hub, row, th, block_title_mutate, window);
    }

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::BeginDisabled(install_op);
    if (ImGui::Button("Manage Game Data", ImVec2(-1, 0)))
        ImGui::OpenPopup("Manage Game Data###detail_manage_game");
    ImGui::EndDisabled();

    draw_detail_manage_game_popup(hub, row, th, block_title_mutate);

    // HD texture packs: per-title, so it lives here beside Manage Game Data
    // rather than in the global PlayStation settings page.
    if (row.installed && retcomm::is_psx_platform(row.platform)) {
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::BeginDisabled(install_op);
        if (ImGui::Button("Texture Packs", ImVec2(-1, 0))) {
            hub.refresh_texture_packs(row.id);
            hub.texpack_status.clear();
            ImGui::OpenPopup("Texture Packs###detail_texture_packs");
        }
        ImGui::EndDisabled();
        if (!row.active_texture_pack.empty()) {
            ImGui::TextColored(th.text_muted, "Active: %s", row.active_texture_pack.c_str());
        }
        draw_detail_texture_packs_popup(hub, row, th, block_title_mutate, window);
    }

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button("Mods", ImVec2(-1, 0))) hub.pending_open_mods = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip(
            "Mod packages installed for this title, read from their manifests under "
            "the game's mods/ folder.");
    }

    if (!row.github_url.empty()) {
        ImGui::Dummy(ImVec2(0, 6));
        if (ImGui::Button("GitHub Source", ImVec2(-1, 0))) {
            std::string err;
            if (!retcomm::open_url_in_browser(row.github_url, &err))
                hub.append_log("Open URL failed: " + err);
        }
    }

    // Seldom-touched: BIOS last.
    if (row.needs_bios || row.supports_openbios) {
        ImGui::Dummy(ImVec2(0, 16));
        ImGui::TextColored(th.text_muted, "BIOS");
        int dump_count = 0;
        bool has_openbios_opt = false;
        for (const auto& id : row.bios_choice_ids) {
            if (id == retcomm::kOpenBiosChoice) has_openbios_opt = true;
            else ++dump_count;
        }
        const bool show_bios_combo =
            static_cast<int>(row.bios_choice_ids.size()) > 1; // conflict / choice
        if (show_bios_combo) {
            const char* preview =
                (row.preferred_bios_index >= 0 &&
                 row.preferred_bios_index < static_cast<int>(row.bios_choice_labels.size()))
                    ? row.bios_choice_labels[static_cast<size_t>(row.preferred_bios_index)].c_str()
                    : "(select)";
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##bios_choice", preview)) {
                for (size_t i = 0; i < row.bios_choice_ids.size(); ++i) {
                    const bool selected = static_cast<int>(i) == row.preferred_bios_index;
                    const std::string item =
                        row.bios_choice_labels[i] + "##" + row.bios_choice_ids[i];
                    if (ImGui::Selectable(item.c_str(), selected)) {
                        std::string err;
                        if (!hub.set_title_preferred_bios(row.id, row.bios_choice_ids[i], &err))
                            hub.append_log("BIOS preference failed: " + err);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (row.bios_choice == retcomm::kOpenBiosChoice) {
                ImGui::TextColored(th.text_muted,
                                   "OpenBIOS regenerates at Install (no dump needed).");
            } else if (row.built_with_openbios && row.installed) {
                ImGui::TextColored(th.warn,
                                   "Install used OpenBIOS — Play becomes Reinstall w BIOS.");
            }
        } else if (dump_count == 1) {
            const char* label =
                (row.preferred_bios_index >= 0 &&
                 row.preferred_bios_index < static_cast<int>(row.bios_choice_labels.size()))
                    ? row.bios_choice_labels[static_cast<size_t>(row.preferred_bios_index)].c_str()
                    : "matched";
            ImGui::TextColored(th.good, "Using %s", label);
            if (row.built_with_openbios && row.installed &&
                row.bios_choice != retcomm::kOpenBiosChoice) {
                ImGui::TextColored(th.warn,
                                   "Install used OpenBIOS — Play becomes Reinstall w BIOS.");
            }
        } else if (has_openbios_opt) {
            ImGui::TextColored(th.good, "Using OpenBIOS");
            ImGui::TextColored(th.text_muted, "Regenerates at Install (no dump needed).");
        } else if (row.has_bios) {
            ImGui::TextColored(th.good, "BIOS matched");
        } else {
            ImGui::TextColored(th.warn, "Missing — Import BIOS from Library");
            if (row.supports_openbios)
                ImGui::TextColored(th.text_muted, "Or use OpenBIOS after a catalog refresh.");
        }
    }

    ImGui::EndChild();
}

// One title, full window: info on the left, everything pressable on the right.
void draw_detail(HubModel& hub, BoxartCache& boxart, const Theme& th, SDL_Window* window) {
    TitleRow row;
    bool have_row = false;
    {
        std::lock_guard<std::mutex> lock(hub.mu);
        have_row = hub.selected >= 0 && hub.selected < static_cast<int>(hub.rows.size());
        if (have_row) row = hub.rows[static_cast<size_t>(hub.selected)];
    }
    if (!have_row) {
        // A rescan dropped the row out from under the page — fall back to the grid.
        go_back_to_titles(hub);
        draw_welcome_panel(boxart, th);
        return;
    }

    ImGui::BeginChild("title_page", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);

    // Same header the grid uses, Configure included: the platform's settings
    // stay one press away whether you are picking a title or looking at one.
    {
        const char* header =
            row.platform.empty() ? "LIBRARY" : platform_display_name(row.platform);
        switch (draw_page_header(th, header, row.platform)) {
            case PageHeaderAction::Configure:
                if (retcomm::is_snes_platform(row.platform)) hub.open_snes_settings();
                else hub.open_psx_settings();
                break;
            case PageHeaderAction::None:
                break;
        }
    }

    // Extra width goes to the info column; the actions column is flexible but
    // capped so a maximised window does not stretch a stack of buttons.
    constexpr float kActionsMaxW = 480.f;
    constexpr float kActionsMinW = 300.f;
    constexpr float kInfoMinW = 320.f;
    const float total_w = ImGui::GetContentRegionAvail().x;
    const float gap_x = ImGui::GetStyle().ItemSpacing.x;
    float right_w = std::min(kActionsMaxW, total_w * 0.42f);
    right_w = std::clamp(right_w, kActionsMinW,
                         std::max(kActionsMinW, total_w - kInfoMinW - gap_x));
    const float left_w = std::max(kInfoMinW, total_w - right_w - gap_x);

    ImGui::BeginChild("title_info_host", ImVec2(left_w, 0), ImGuiChildFlags_NavFlattened);
    draw_title_info_panel(hub, row, boxart, th);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("title_actions_host", ImVec2(right_w, 0), ImGuiChildFlags_NavFlattened);
    draw_title_actions_panel(hub, row, th, window);
    ImGui::EndChild();

    hub.detail_scroll_top = false;
    ImGui::EndChild();

    // B (or Escape) returns to the grid, unless something modal or the drawer
    // is in front and owns that key.
    if (!hub.drawer_open && !hub.log_overlay_open &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        !ImGui::IsAnyItemActive() &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        go_back_to_titles(hub);
}

void draw_settings_panel(HubModel& hub, const Theme& th, SDL_Window* window) {
    ImGui::BeginChild("settings", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      page_wheel_flags(hub));
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("LIBRARY SETTINGS");
    ImGui::PopStyleColor();
    ImGui::TextWrapped("Paths and platform folder names written to config.json.");
    ImGui::Separator();

    // Where Retro itself lives (toolchain, engines, installs, caches). Read-only
    // here — changing it relocates the tree, so it goes through its own dialog.
    {
        const bool custom = retcomm::using_custom_root(hub.paths);
        // A portable install's folder has no fixed location — the launcher works
        // out where it is on every start — so showing only an absolute path here
        // reads as a hard-coded one. Lead with the path relative to the launcher
        // whenever the folder sits under it, and keep the absolute form as the
        // secondary line so nothing is hidden.
        const fs::path launcher_dir = hub.root_marker_dir();
        std::error_code rel_ec;
        fs::path rel;
        if (!launcher_dir.empty())
            rel = fs::relative(hub.paths.data_dir, launcher_dir, rel_ec);
        const bool under_launcher =
            !rel_ec && !rel.empty() &&
            rel.native().rfind(fs::path("..").native(), 0) != 0 && rel != fs::path(".");

        ImGui::TextColored(th.text_muted, "Retro data folder");
        if (under_launcher) {
            // "./RetComM-Data" — built through fs::path so the separator is the
            // platform's own.
            ImGui::TextWrapped("%s", (fs::path(".") / rel).string().c_str());
        } else {
            ImGui::TextWrapped("%s", hub.paths.data_dir.string().c_str());
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        if (under_launcher) {
            ImGui::TextWrapped("Relative to %s — the folder moves with it.",
                               launcher_dir.filename().empty()
                                   ? launcher_dir.string().c_str()
                                   : (launcher_dir.filename().string() + "/").c_str());
            ImGui::TextWrapped("%s", hub.paths.data_dir.string().c_str());
        }
        ImGui::TextWrapped("Source: %s%s",
                           retcomm::data_root_source_label(hub.paths.root_source),
                           custom ? "" : " (toolchain, engines, installs and caches)");
        ImGui::PopStyleColor();
        if (ImGui::Button("Open folder##data_root")) {
            std::string err;
            if (!retcomm::open_path_in_file_manager(hub.paths.data_dir, &err))
                hub.append_log("open data folder failed: " + err);
        }
        ImGui::SameLine();
        // A user-set RETCOMM_HOME and --root are re-read on every launch, so a
        // pointer we wrote here would be silently ignored. Say why instead of
        // misleading. The portable stub's RETCOMM_HOME is not one of those: it
        // derives from retcomm-root.json beside the .exe, which is exactly what
        // changing the folder writes, so that change does take effect.
        const bool env_pinned = hub.paths.root_source == retcomm::DataRootSource::Env ||
                                hub.paths.root_source == retcomm::DataRootSource::Explicit;
        ImGui::BeginDisabled(env_pinned || hub.job_running.load());
        if (ImGui::Button("Change…##data_root")) {
            hub.seed_data_root_input();
            hub.data_root_mode = retcomm::RootMigrationMode::Move;
            hub.show_data_root_dialog = true;
        }
        ImGui::EndDisabled();
        if (env_pinned && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Pinned by %s for this launch — clear it to change the folder here.",
                              retcomm::data_root_source_label(hub.paths.root_source));
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 6));
    }

    if (path_field_with_browse("ROM library root", "##library_root", hub.settings.library_root,
                               sizeof(hub.settings.library_root), hub, window,
                               FolderPickTarget::LibraryRoot, th))
        hub.settings.dirty = true;

    ImGui::Dummy(ImVec2(0, 6));
    if (path_field_with_browse("BIOS root", "##bios_root", hub.settings.bios_root,
                               sizeof(hub.settings.bios_root), hub, window,
                               FolderPickTarget::BiosRoot, th))
        hub.settings.dirty = true;

    ImGui::Dummy(ImVec2(0, 6));
    if (path_field_with_browse("Game saves root", "##saves_root", hub.settings.saves_root,
                               sizeof(hub.settings.saves_root), hub, window,
                               FolderPickTarget::SavesRoot, th))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Native SRAM / memcard files (RomM sync + launch). Each game gets "
        "…/<platform>/<title_id>/ under this root (e.g. …/saves/ps/masters-of-teras-kasi-psx). "
        "Leave empty to keep saves inside each install.");
    ImGui::PopStyleColor();

    // Relative roots are the answer to "my whole library lives on a share and I
    // have to re-point every path when I move it". Shown here because nothing
    // else would tell a user the option exists.
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "These roots, and the install locations below, may be written relative in "
        "config.json — \"roms\", \"./bios\", \"../shared/saves\" — and resolve against "
        "%s. Written that way they move with the folder, so a setup copied to another "
        "drive or machine needs no re-pointing; editing a path here replaces it with an "
        "absolute one, the rest keep the form they were written in.",
        retcomm::config_relative_base(hub.paths.config_path).string().c_str());
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(th.text_muted, "Exclude dirs (comma-separated basenames)");
    if (hub_input_text("##exclude_dirs", hub.settings.exclude_dirs,
                       sizeof(hub.settings.exclude_dirs)))
        hub.settings.dirty = true;

    ImGui::Dummy(ImVec2(0, 10));
    if (accent_button("Save##library_paths", th, ImVec2(160, 0))) {
        std::string err;
        if (!hub.save_settings(&err)) {
            hub.append_log("settings save failed: " + err);
            hub.set_status("Save failed");
        } else {
            hub.show_toast("Saved!");
        }
    }

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("GAME INSTALL LOCATIONS");
    ImGui::PopStyleColor();
    ImGui::TextWrapped(
        "Folders that hold installed titles (each contains <game>/releases/…). "
        "Default is Retro's apps/ folder. Add another root (e.g. an external drive) when "
        "you want Install to ask where to put a new game. Existing installs stay where they are.");
    if (ImGui::BeginTable("install_roots", 4,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##def", ImGuiTableColumnFlags_WidthFixed, 56.f);
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 180.f);
        ImGui::TableSetupColumn("path", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 36.f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(hub.settings.install_roots.size()); ++i) {
            auto& row = hub.settings.install_roots[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::RadioButton("##default_root", &hub.settings.default_install_root_index, i))
                hub.settings.dirty = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Default for new installs");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.f);
            if (hub_input_text("##label", row.label, sizeof(row.label)))
                hub.settings.dirty = true;
            ImGui::TableNextColumn();
            {
                const float browse_w = 72.f;
                const float gap = ImGui::GetStyle().ItemSpacing.x;
                const float input_w = ImGui::GetContentRegionAvail().x - browse_w - gap;
                if (input_w > 40.f) ImGui::SetNextItemWidth(input_w);
                if (hub_input_text("##path", row.path, sizeof(row.path)))
                    hub.settings.dirty = true;
                ImGui::SameLine();
                bool busy = false;
                {
                    std::lock_guard<std::mutex> lock(hub.folder_pick_mu);
                    busy = hub.folder_pick_busy;
                }
                ImGui::BeginDisabled(busy);
                if (ImGui::Button("…", ImVec2(browse_w, 0))) {
                    hub.folder_pick_install_index = i;
                    begin_folder_pick(hub, window, FolderPickTarget::InstallRoot, row.path);
                }
                ImGui::EndDisabled();
            }
            ImGui::TableNextColumn();
            if (ImGui::Button("X")) {
                hub.settings.install_roots.erase(hub.settings.install_roots.begin() + i);
                if (hub.settings.default_install_root_index >=
                    static_cast<int>(hub.settings.install_roots.size())) {
                    hub.settings.default_install_root_index =
                        std::max(0, static_cast<int>(hub.settings.install_roots.size()) - 1);
                } else if (hub.settings.default_install_root_index > i) {
                    --hub.settings.default_install_root_index;
                }
                hub.settings.dirty = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (accent_button("Save##install_roots", th, ImVec2(120, 0))) {
        std::string err;
        if (!hub.save_settings(&err)) {
            hub.append_log("settings save failed: " + err);
            hub.set_status("Save failed");
        } else {
            hub.show_toast("Saved!");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Install Location", ImVec2(200, 0)))
        hub.add_install_root_row();

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Add ROMs / BIOS / saves, refresh folders, or clean missing files from the Library "
        "button in the top bar.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    if (ImGui::Checkbox("Hide Unowned Catalog Items", &hub.settings.filter_unsupported_titles))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Hide catalog titles that do not have a ROM available — neither on your local ROM "
        "library path nor (when scanned) on RomM. Installed titles stay visible. Save, then "
        "use Install / RomM sync so remote-only matches appear.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Checkbox("Always Check For Updates On Startup",
                        &hub.settings.check_updates_on_startup))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "When enabled, Retro checks for launcher, toolchain, and game updates after the hub "
        "starts. Turn off to skip the startup prompt (Check Updates in the menu still works). "
        "Save to apply.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Checkbox("Scan For New Games After A Catalog Update",
                        &hub.settings.auto_scan_after_catalog_update))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "When the catalog adds titles, Retro binds them to ROMs you already have using "
        "cached hashes, then scans only the affected platforms for anything still unmatched. "
        "New games show up without pressing Add/Scan Files. Save to apply.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Checkbox("Always Check For Updates Before Game Launch",
                        &hub.settings.check_updates_before_launch))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "When enabled, Play queries GitHub for that title and asks before launching if a newer "
        "release is available. Save to apply.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::TextColored(th.text_muted, "Interface scale");
    {
        struct ScaleChoice {
            const char* label;
            float value;
        };
        static const ScaleChoice kChoices[] = {
            {"Auto (follow display)", 0.f}, {"100%", 1.f},   {"125%", 1.25f}, {"150%", 1.5f},
            {"175%", 1.75f},                {"200%", 2.f},   {"250%", 2.5f},  {"300%", 3.f},
        };
        int cur = -1;
        for (int i = 0; i < IM_ARRAYSIZE(kChoices); ++i) {
            if (std::fabs(hub.settings.ui_scale - kChoices[i].value) < 0.005f) {
                cur = i;
                break;
            }
        }
        // A hand-edited config.json may hold a value the presets do not cover.
        char custom[32];
        std::snprintf(custom, sizeof(custom), "%d%%",
                      static_cast<int>(hub.settings.ui_scale * 100.f + 0.5f));
        ImGui::SetNextItemWidth(240.f);
        if (ImGui::BeginCombo("##ui_scale", cur >= 0 ? kChoices[cur].label : custom)) {
            for (int i = 0; i < IM_ARRAYSIZE(kChoices); ++i) {
                if (ImGui::Selectable(kChoices[i].label, i == cur)) {
                    hub.settings.ui_scale = kChoices[i].value;
                    hub.settings.dirty = true;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Size of hub text and controls. Auto follows the desktop's own scaling (Windows "
        "\"Scale and layout\", GNOME/KDE fractional scaling). Applies as soon as you pick it; "
        "Save to keep it.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::TextColored(th.text_muted, "GitHub token (optional)");
    ImGui::SetNextItemWidth(-1);
    if (hub_input_text("##github_token", hub.settings.github_token,
                       sizeof(hub.settings.github_token), ImGuiInputTextFlags_Password))
        hub.settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Optional PAT for api.github.com when downloading release assets (Install / Apply "
        "Update). Catalog, launcher, toolchain, and game *version checks* use github.com and "
        "do not need a token. Use a classic public_repo token or fine-grained Contents read "
        "if you hit API rate limits during downloads. GITHUB_TOKEN / GH_TOKEN in the "
        "environment still overrides this. Save to apply.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8));
    {
        const bool busy = hub.job_running.load();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Find Missing Boxart", ImVec2(200, 0)))
            hub.start_job(HubJob::FetchBoxart, {}, false);
        ImGui::SameLine();
        if (ImGui::Button("Resync All Boxart", ImVec2(200, 0)))
            hub.start_job(HubJob::FetchBoxart, {}, true);
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Find Missing downloads covers only for titles with no cached art. "
            "Resync All clears the cover cache and re-downloads every catalog title "
            "(Libretro thumbnails, or RomM when Sync Boxart is enabled).");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 8));
    {
        const bool busy = hub.job_running.load();
        constexpr float kGap = 8.f;
        const float btn_w = (ImGui::GetContentRegionAvail().x - kGap) * 0.5f;
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Clean Unlisted Installs…", ImVec2(btn_w, 0))) {
            const size_t n = hub.refresh_orphan_installs();
            if (n == 0) {
                hub.set_status("No installs outside the catalog");
                hub.append_log("Orphan scan: none");
            } else {
                hub.orphan_prompt_pending.store(true);
            }
        }
        ImGui::SameLine(0, kGap);
        if (ImGui::Button("Clean Old Update Files", ImVec2(btn_w, 0)))
            hub.start_job(HubJob::CleanupOldReleases);
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Unlisted: remove apps/ installs no longer in the catalog. Old update files: "
            "promote saves/config into the current release, then delete leftover "
            "releases/<old-tag>/ folders.");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 10));
    if (ImGui::CollapsingHeader("Advanced")) {
        if (ImGui::Checkbox("Auto-clean cmake build directories after install",
                            &hub.settings.auto_clean_build_dirs))
            hub.settings.dirty = true;
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "When enabled, Retro deletes each title's src/current/build/ after a successful "
            "local build to free disk space. Leave off for faster package updates (incremental "
            "Ninja). Save to apply.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 8));
        if (ImGui::Checkbox("Auto-prune shared caches after builds", &hub.settings.auto_gc_caches))
            hub.settings.dirty = true;
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Keeps a few recent toolchain/SDK versions, drops unreferenced engine pins, old "
            "release zip downloads, and cmake build/ trees idle longer than the day limit. "
            "Shared ccache lives under the Retro data dir with a size cap.");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Keep toolchain versions", &hub.settings.keep_toolchain_versions)) {
            if (hub.settings.keep_toolchain_versions < 1) hub.settings.keep_toolchain_versions = 1;
            hub.settings.dirty = true;
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Keep SDK versions", &hub.settings.keep_sdk_versions)) {
            if (hub.settings.keep_sdk_versions < 1) hub.settings.keep_sdk_versions = 1;
            hub.settings.dirty = true;
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Keep orphan engine pins", &hub.settings.keep_orphan_engine_pins)) {
            if (hub.settings.keep_orphan_engine_pins < 0) hub.settings.keep_orphan_engine_pins = 0;
            hub.settings.dirty = true;
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Idle build keep days", &hub.settings.idle_build_keep_days)) {
            if (hub.settings.idle_build_keep_days < 0) hub.settings.idle_build_keep_days = 0;
            hub.settings.dirty = true;
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("ccache max GiB", &hub.settings.ccache_max_gb)) {
            if (hub.settings.ccache_max_gb < 0) hub.settings.ccache_max_gb = 0;
            hub.settings.dirty = true;
        }

        ImGui::Dummy(ImVec2(0, 6));
        {
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Prune shared caches now", ImVec2(-1, 0)))
                hub.start_job(HubJob::CleanupSharedCaches);
            ImGui::EndDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Runs the same GC as post-build pruning (toolchains, SDKs, engines, release zips, "
            "idle builds). Safe: never deletes Play binaries or generated game C.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 6));
        {
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Clean all cmake build dirs", ImVec2(-1, 0)))
                hub.start_job(HubJob::CleanupCmakeBuildDirs);
            ImGui::EndDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Immediately delete cmake build/ trees under every game install's src/ "
            "(Play binary, saves, and generated C are kept). The next Update or Generate & "
            "Rebuild will reconfigure from scratch.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Separator();
        // Use U+26A0 only (no U+FE0F) — variation selectors render as "?" without emoji fonts.
        {
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (danger_button("\xE2\x9A\xA0 Delete All Apps & Save Data \xE2\x9A\xA0", th,
                              ImVec2(-1, 0)))
                ImGui::OpenPopup("Delete everything?###delete_all_apps");
            ImGui::EndDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Permanently removes every installed game under your install locations, managed "
            "saves under the Game saves root, in-install saves/config, and global platform "
            "Configure prefs. Library ROMs and BIOS dumps are kept.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 8));
        {
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (danger_button("\xE2\x9A\xA0 Hard Reset Library Settings \xE2\x9A\xA0", th,
                              ImVec2(-1, 0)))
                ImGui::OpenPopup("Hard reset?###hard_reset_library");
            ImGui::EndDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Wipes library paths, scan databases, and RomM sync settings from config, then "
            "restarts Retro into the first-time setup wizard. Installed games, ROM files, "
            "and BIOS dumps are not deleted.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 8));
        {
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (danger_button("\xE2\x9A\xA0 Uninstall All Retro Data \xE2\x9A\xA0", th,
                              ImVec2(-1, 0))) {
                hub.refresh_uninstall_plan();
                hub.uninstall_confirm[0] = '\0';
                ImGui::OpenPopup("Uninstall Retro?###uninstall_retcomm");
            }
            ImGui::EndDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Removes Retro entirely: the whole Retro data folder (AppData / .local/share "
            "or your custom folder) with every installed game, save, toolchain and cache, plus "
            "the launcher itself. Your ROM library and BIOS folders are the only things kept. "
            "This cannot be undone.");
        ImGui::PopStyleColor();

        if (ImGui::BeginPopupModal("Delete everything?###delete_all_apps", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextColored(th.warn, "This cannot be undone.");
            ImGui::TextWrapped(
                "Delete all app installations and clean up save/config data?\n\n"
                "• Every title under configured install locations\n"
                "• Managed files under your Game saves root\n"
                "• Preserved install saves/config and platform Configure prefs\n\n"
                "ROM library and BIOS folders are not deleted.");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (danger_button("Yes, delete everything", th, ImVec2(220, 0))) {
                hub.start_job(HubJob::DeleteAllAppsAndSaves);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Hard reset?###hard_reset_library", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextColored(th.warn, "Retro will restart into first-time setup.");
            ImGui::TextWrapped(
                "Hard reset library settings?\n\n"
                "• Deletes config.json (library / BIOS / saves roots, RomM sync)\n"
                "• Clears library-index, bios-index, and RomM ROM index databases\n"
                "• Clears the setup-completed marker\n\n"
                "Installed apps, ROM library files, and BIOS dumps stay on disk.");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (danger_button("Yes, hard reset & restart", th, ImVec2(240, 0))) {
                hub.start_job(HubJob::HardResetLibrarySettings);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("Uninstall Retro?###uninstall_retcomm", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 460.f);
            ImGui::TextColored(th.warn,
                               "\xE2\x9A\xA0 This deletes Retro and everything it stores. "
                               "There is no undo. \xE2\x9A\xA0");
            ImGui::TextWrapped(
                "Deleted for good:\n"
                "\xE2\x80\xA2 Every installed game, managed save, toolchain, engine and cache\n"
                "\xE2\x80\xA2 Library / BIOS / RomM settings and every scan database\n"
                "\xE2\x80\xA2 The launcher itself\n\n"
                "Kept: your ROM library folder and BIOS folder, and any install location "
                "outside the Retro data folder.");
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextColored(th.text_muted, "Folders that will be deleted:");
            for (const auto& p : hub.uninstall_plan.data_paths)
                ImGui::BulletText("%s", p.string().c_str());
            if (!hub.uninstall_plan.app_note.empty()) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::TextColored(th.text_muted, "The app: %s",
                                   hub.uninstall_plan.app_note.c_str());
            }
            ImGui::PopTextWrapPos();

            ImGui::Dummy(ImVec2(0, 10));
            ImGui::TextColored(th.text_muted, "Type UNINSTALL to confirm:");
            ImGui::SetNextItemWidth(240.f);
            hub_input_text("##uninstall_confirm", hub.uninstall_confirm,
                           sizeof(hub.uninstall_confirm));

            ImGui::Dummy(ImVec2(0, 10));
            const bool busy = hub.job_running.load();
            const bool confirmed = std::string(hub.uninstall_confirm) == "UNINSTALL";
            ImGui::BeginDisabled(busy || !confirmed);
            if (danger_button("Yes, uninstall Retro", th, ImVec2(240, 0))) {
                hub.start_job(HubJob::UninstallEverything);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                hub.uninstall_confirm[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }
    }

    ImGui::Dummy(ImVec2(0, 10));
    if (ImGui::CollapsingHeader("Advanced folder mapping")) {
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Catalog platform slug → folder name(s) under the library / BIOS / saves roots "
            "(e.g. psx → ps, ps1). Most setups can leave the defaults.");
        ImGui::PopStyleColor();
        ImGui::Separator();

        if (ImGui::BeginTable("platform_folders", 3,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("platform", ImGuiTableColumnFlags_WidthFixed, 120.f);
            ImGui::TableSetupColumn("folders", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 36.f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(hub.settings.platform_folders.size()); ++i) {
                auto& row = hub.settings.platform_folders[static_cast<size_t>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (hub_input_text("##plat", row.platform, sizeof(row.platform)))
                    hub.settings.dirty = true;
                ImGui::TableNextColumn();
                if (hub_input_text("##folders", row.folders, sizeof(row.folders)))
                    hub.settings.dirty = true;
                ImGui::TableNextColumn();
                if (ImGui::Button("X")) {
                    hub.settings.platform_folders.erase(hub.settings.platform_folders.begin() + i);
                    hub.settings.dirty = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Add platform row")) hub.add_platform_folder_row();
    }

    ImGui::Dummy(ImVec2(0, 12));
    if (accent_button("Save", th, ImVec2(160, 0))) {
        std::string err;
        if (!hub.save_settings(&err)) {
            hub.append_log("settings save failed: " + err);
            hub.set_status("Save failed");
        } else {
            hub.show_toast("Saved!");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        hub.show_settings = false;
        hub.settings.dirty = false;
    }
    if (hub.settings.dirty) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(th.warn, "unsaved changes");
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(th.text_muted, "%s", hub.paths.config_path.string().c_str());
    ImGui::EndChild();
}

// Draw-list only — must not submit ImGui items (breaks SameLine for side-by-side cards).
void draw_list_wrapped_text(ImDrawList* dl, ImVec2 pos, float wrap_w, ImU32 col, const char* text) {
    if (!dl || !text || !text[0] || wrap_w <= 1.f) return;
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const float scale = (font && font->FontSize > 0.f) ? (font_size / font->FontSize) : 1.f;
    const float line_h = ImGui::GetTextLineHeightWithSpacing();
    const char* end = text + std::strlen(text);
    const char* s = text;
    float y = pos.y;
    while (s < end) {
        while (s < end && (*s == '\n' || *s == '\r')) {
            y += line_h;
            ++s;
        }
        if (s >= end) break;
        const char* line_end =
            font ? font->CalcWordWrapPositionA(scale, s, end, wrap_w) : end;
        if (line_end == s) line_end = s + 1; // always advance
        dl->AddText(font, font_size, ImVec2(pos.x, y), col, s, line_end);
        s = line_end;
        while (s < end && (*s == ' ' || *s == '\t')) ++s;
        y += line_h;
    }
}

bool draw_setup_path_card(BoxartCache& boxart, const Theme& th, const char* id,
                          const char* title, const char* subtitle, const char* asset_file,
                          float card_w, float card_h) {
    ImGui::PushID(id);
    // Single layout item so SameLine keeps both cards on one row.
    const ImVec2 card_min = ImGui::GetCursorScreenPos();
    const ImVec2 card_max(card_min.x + card_w, card_min.y + card_h);
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(card_w, card_h));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(hovered ? th.panel_hovered : th.panel);
    const ImU32 border =
        ImGui::ColorConvertFloat4ToU32(hovered ? th.accent_dim : th.border);
    dl->AddRectFilled(card_min, card_max, fill, th.radius_lg);
    dl->AddRect(card_min, card_max, border, th.radius_lg, 0, hovered ? 2.f : 1.f);
    dl->PushClipRect(ImVec2(card_min.x + 1.f, card_min.y + 1.f),
                     ImVec2(card_max.x - 1.f, card_max.y - 1.f), true);

    const fs::path icon = find_hub_asset_file("setup", asset_file);
    const BoxartTexture* tex =
        icon.empty() ? nullptr : boxart.get(std::string("setup:") + id, icon);
    const float pad = 20.f;
    const float icon_max = std::min(card_w - pad * 2.f, card_h * 0.42f);
    if (tex && tex->gl_id && tex->width > 0 && tex->height > 0) {
        const ImVec2 fit = contain_size(static_cast<float>(tex->width),
                                        static_cast<float>(tex->height), icon_max, icon_max);
        const float ix = card_min.x + (card_w - fit.x) * 0.5f;
        const float iy = card_min.y + pad + 6.f;
        dl->AddImage((ImTextureID)(intptr_t)tex->gl_id, ImVec2(ix, iy),
                     ImVec2(ix + fit.x, iy + fit.y));
    }

    const ImVec2 title_sz = ImGui::CalcTextSize(title);
    const float text_y = card_min.y + pad + 6.f + icon_max + 12.f;
    dl->AddText(ImVec2(card_min.x + (card_w - title_sz.x) * 0.5f, text_y),
                ImGui::ColorConvertFloat4ToU32(th.text), title);
    draw_list_wrapped_text(dl, ImVec2(card_min.x + pad, text_y + title_sz.y + 8.f),
                           card_w - pad * 2.f, ImGui::ColorConvertFloat4ToU32(th.text_muted),
                           subtitle);
    dl->PopClipRect();

    ImGui::PopID();
    return clicked;
}

void draw_setup_wizard(HubModel& hub, BoxartCache& boxart, const Theme& th, SDL_Window* window) {
    if (!hub.show_setup) return;

    ImGui::OpenPopup("Welcome to Retro###setup_wizard");
    constexpr float kWizW = 820.f; // a bit wider so the two path cards can breathe
    constexpr float kWizH = 628.f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float target_w = kWizW;
    float target_h = kWizH;
    if (hub.setup_path == SetupPath::Easy && hub.setup_step != 0) {
        // Sparse content — ~30% smaller than the shared wizard size. The
        // Installation Directory step (step 0) is as dense as the Advanced one,
        // so it keeps the full size rather than cramming radios and two paths
        // into the compact window.
        target_w = 780.f * 0.70f;
        target_h = 628.f * 0.70f;
    }
    const float wiz_w = std::min(target_w, vp->WorkSize.x * 0.96f);
    const float wiz_h = std::min(target_h, vp->WorkSize.y * 0.92f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(wiz_w, wiz_h), ImVec2(wiz_w, wiz_h));
    ImGui::SetNextWindowSize(ImVec2(wiz_w, wiz_h), ImGuiCond_Always);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Welcome to Retro###setup_wizard", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar))
        return;

    auto push_wrap = [&] {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
    };

    // Reserve footer space, then pin action buttons to the bottom-left with padding.
    auto footer_reserve = [&](float warn_h = 0.f) {
        const float gap_above = 24.f;
        const float pad_below = 18.f;
        return gap_above + ImGui::GetFrameHeight() + warn_h + pad_below;
    };
    auto pin_footer_row = [&](float warn_h = 0.f) {
        const float pad_below = 18.f;
        const float pad_left = 6.f; // on top of window padding
        const float row_h = ImGui::GetFrameHeight() + warn_h;
        ImGui::SetCursorPosY(ImGui::GetWindowContentRegionMax().y - pad_below - row_h);
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + pad_left);
    };

    auto advance_to_platform_step = [&]() {
        hub.seed_setup_platform_folders();
        hub.setup_step = 2;
        hub.setup_confirm_create_roots = false;
    };

    if (hub.setup_confirm_create_roots && hub.setup_path == SetupPath::Advanced) {
        const float footer_h = footer_reserve();
        ImGui::BeginChild("##setup_create_roots_body", ImVec2(0.f, -footer_h),
                          ImGuiChildFlags_None);
        push_wrap();
        ImGui::TextWrapped("These folders do not exist yet. Create them now?");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8));
        for (const auto& p : hub.setup_missing_roots) {
            push_wrap();
            ImGui::BulletText("%s", p.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        pin_footer_row();
        if (accent_button("Create folders", th, ImVec2(160, 0))) {
            std::string err;
            if (!hub.create_missing_setup_roots(&err)) {
                hub.append_log("setup create roots failed: " + err);
                hub.set_status("Could not create folders");
            } else {
                hub.set_status("Folders created");
                advance_to_platform_step();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(120, 0))) {
            hub.setup_confirm_create_roots = false;
            hub.setup_missing_roots.clear();
        }
        ImGui::EndPopup();
        return;
    }

    if (hub.setup_path == SetupPath::Chooser) {
        // No footer here any more — first-run setup is not skippable, and the
        // two cards are the only way forward. Give them the reclaimed height.
        ImGui::BeginChild("##setup_chooser_body", ImVec2(0.f, -18.f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);
        push_wrap();
        ImGui::TextWrapped("How do you want to set up your library?");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 16));

        const float gap = 20.f;
        const float side_pad = 4.f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + side_pad);
        const float row_w = ImGui::GetContentRegionAvail().x - side_pad;
        const float card_w = (row_w - gap) * 0.5f;
        const float card_h = std::max(240.f, ImGui::GetContentRegionAvail().y - 4.f);
        if (draw_setup_path_card(boxart, th, "easy", "Easy Setup",
                                 "Pick one Emulation folder. Retro creates roms, bios, "
                                 "and saves under it with default platform folders.",
                                 "setup_easy_rocket.png", card_w, card_h)) {
            hub.setup_path = SetupPath::Easy;
            hub.setup_step = 0; // Installation Directory, then the game folder.
            hub.seed_data_root_input();
            hub.pending_data_root.clear();
            hub.pending_data_root_change = false;
            hub.apply_suggested_emulation_root(/*overwrite_nonempty=*/false);
            hub.apply_roots_from_emulation_parent();
        }
        ImGui::SameLine(0.f, gap);
        if (draw_setup_path_card(boxart, th, "advanced", "Advanced Setup",
                                 "Choose roms, bios, and saves separately. Optionally connect "
                                 "RomM and edit platform folder mappings.",
                                 "setup_advanced_wrench.png", card_w, card_h)) {
            hub.setup_path = SetupPath::Advanced;
            hub.setup_step = 0;
            hub.seed_data_root_input();
            hub.apply_suggested_library_roots(/*overwrite_nonempty=*/false);
        }
        ImGui::EndChild();

    } else if (hub.setup_path == SetupPath::Easy && hub.setup_step == 0) {
        // Step 1 of 2 — the install location. Deliberately plainer language than
        // the Advanced wording: "Installation Directory" is what people expect
        // to be asked for, and it has to come first because everything the next
        // step creates lands under it.
        hub.refresh_data_root_plan();
        const bool custom = hub.setup_use_custom_data_root;
        const bool root_ok = !custom || hub.data_root_plan.blocker.empty();
        const float warn_h = root_ok ? 0.f : ImGui::GetTextLineHeightWithSpacing();
        const float footer_h = footer_reserve(warn_h);
        ImGui::BeginChild("##setup_easy_root_body", ImVec2(0.f, -footer_h), ImGuiChildFlags_None);
        push_wrap();
        ImGui::TextWrapped(
            "Step 1 of 2 — Installation Directory. This is where Retro installs itself: "
            "the games it builds, the build tools it downloads, and its caches. It can grow "
            "to tens of GB, so pick a drive with room. Your game files are chosen next and "
            "can live anywhere.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 12));

        bool changed = false;
        if (ImGui::RadioButton("Install to the default location", !custom)) {
            hub.setup_use_custom_data_root = false;
            changed = true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        push_wrap();
        ImGui::TextWrapped("  %s", retcomm::default_os_data_dir().string().c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 8));
        if (ImGui::RadioButton("Install to a folder I choose", custom)) {
            hub.setup_use_custom_data_root = true;
            if (hub.data_root_input[0] == '\0' && !hub.exe_dir.empty()) {
                copy_buf(hub.data_root_input, sizeof(hub.data_root_input),
                         (hub.exe_dir / "Retro").string());
            }
            changed = true;
        }
        if (changed) hub.data_root_plan_dirty = true;

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::BeginDisabled(!custom);
        if (path_field_with_browse("Installation Directory", "##setup_easy_data_root",
                                   hub.data_root_input, sizeof(hub.data_root_input), hub, window,
                                   FolderPickTarget::DataRoot, th))
            hub.data_root_plan_dirty = true;
        ImGui::EndDisabled();

        if (custom) {
            const auto& plan = hub.data_root_plan;
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            push_wrap();
            if (plan.blocker.empty()) {
                ImGui::TextWrapped("Retro will be installed in:\n  %s",
                                   plan.to_root.string().c_str());
                if (!plan.note.empty()) {
                    ImGui::Dummy(ImVec2(0, 4));
                    ImGui::TextWrapped("%s", plan.note.c_str());
                }
                if (plan.existing_bytes > 0) {
                    ImGui::Dummy(ImVec2(0, 4));
                    ImGui::TextWrapped("Existing Retro files (%s) move here when you finish.",
                                       human_bytes(plan.existing_bytes).c_str());
                }
            } else {
                ImGui::TextWrapped("Pick a folder Retro can create and write to.");
            }
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            if (!plan.warning.empty()) {
                push_wrap();
                ImGui::TextColored(th.warn, "%s", plan.warning.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        ImGui::EndChild();

        pin_footer_row(warn_h);
        if (ImGui::Button("Back", ImVec2(100, 0))) {
            hub.setup_path = SetupPath::Chooser;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!root_ok);
        if (accent_button("Next", th, ImVec2(120, 0))) {
            const fs::path chosen =
                custom ? fs::path(hub.data_root_input).lexically_normal() : fs::path();
            hub.pending_data_root = chosen;
            hub.pending_data_root_change =
                custom ? !(retcomm::using_custom_root(hub.paths) && hub.paths.root == chosen)
                       : retcomm::using_custom_root(hub.paths);
            hub.setup_step = 1;
        }
        ImGui::EndDisabled();
        if (!root_ok) {
            ImGui::TextColored(th.warn, "%s", hub.data_root_plan.blocker.c_str());
        }
    } else if (hub.setup_path == SetupPath::Easy) {
        const bool emu_ok = hub.setup_emulation_root[0] != '\0';
        const float warn_h = emu_ok ? 0.f : ImGui::GetTextLineHeightWithSpacing();
        const float footer_h = footer_reserve(warn_h);
        ImGui::BeginChild("##setup_easy_body", ImVec2(0.f, -footer_h), ImGuiChildFlags_None);
        push_wrap();
        ImGui::TextWrapped(
            "Step 2 of 2 — Choose your Emulation folder. This is where your game files "
            "live, separate from the installation directory. Retro will use …/roms, "
            "…/bios, and …/saves under it, create any that are missing, and seed default "
            "platform folders.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 10));
        if (ImGui::Button("Use ~/Emulation")) {
            hub.apply_suggested_emulation_root(/*overwrite_nonempty=*/true);
            hub.apply_roots_from_emulation_parent();
            hub.set_status("Suggested Emulation folder applied");
        }
        ImGui::Dummy(ImVec2(0, 10));
        if (path_field_with_browse("Emulation folder", "##setup_emulation_root",
                                   hub.setup_emulation_root, sizeof(hub.setup_emulation_root),
                                   hub, window, FolderPickTarget::EmulationRoot, th)) {
            hub.apply_roots_from_emulation_parent();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        push_wrap();
        if (hub.setup_emulation_root[0] != '\0') {
            const fs::path emu(hub.setup_emulation_root);
            const std::string roms = (emu / "roms").string();
            const std::string bios = (emu / "bios").string();
            const std::string saves = (emu / "saves").string();
            ImGui::TextWrapped("Will use:\n  %s\n  %s\n  %s", roms.c_str(), bios.c_str(),
                               saves.c_str());
        } else {
            ImGui::TextWrapped("Pick a parent folder (for example ~/Emulation).");
        }
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::EndChild();

        pin_footer_row(warn_h);
        if (ImGui::Button("Back", ImVec2(100, 0))) {
            hub.setup_step = 0; // back to the Installation Directory
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!emu_ok);
        if (accent_button("Finish", th, ImVec2(120, 0))) {
            std::string err;
            if (!hub.finish_easy_setup(&err)) {
                hub.append_log("easy setup failed: " + err);
                hub.set_status("Easy setup failed");
            } else if (hub.pending_data_root_change) {
                // Same as the Advanced path: config, indexes and the setup
                // marker are written under the current root, so they travel
                // with the move. The job relaunches; the scan prompt comes back
                // on the next run.
                hub.append_log("First-time easy setup saved");
                hub.pending_data_root_change = false;
                hub.show_setup_scan_prompt = false;
                hub.start_job(HubJob::MigrateDataRoot);
            } else {
                hub.set_status("Setup complete");
                hub.append_log("First-time easy setup saved");
            }
        }
        ImGui::EndDisabled();
        if (!emu_ok) {
            ImGui::TextColored(th.warn, "Choose an Emulation folder to continue.");
        }
    } else if (hub.setup_step == 0) {
        // Step 1 of 3 — where Retro keeps its own files. This has to come
        // first: everything the later steps create lands under it.
        hub.refresh_data_root_plan();
        const bool custom = hub.setup_use_custom_data_root;
        const bool root_ok = !custom || hub.data_root_plan.blocker.empty();
        const float warn_h = root_ok ? 0.f : ImGui::GetTextLineHeightWithSpacing();
        const float footer_h = footer_reserve(warn_h);
        ImGui::BeginChild("##setup_dataroot_body", ImVec2(0.f, -footer_h), ImGuiChildFlags_None);
        push_wrap();
        ImGui::TextWrapped(
            "Step 1 of 3 — Choose where Retro stores its own files: the build toolchain, "
            "engine sources, installed games, and caches. This can grow to tens of GB, so "
            "put it on a drive with room. Your ROM, BIOS, and saves folders are set next "
            "and can live anywhere.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 12));

        bool changed = false;
        if (ImGui::RadioButton("Use the default location", !custom)) {
            hub.setup_use_custom_data_root = false;
            changed = true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        push_wrap();
        ImGui::TextWrapped("  %s", retcomm::default_os_data_dir().string().c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 8));
        if (ImGui::RadioButton("Use a custom folder (portable / another drive)", custom)) {
            hub.setup_use_custom_data_root = true;
            if (hub.data_root_input[0] == '\0' && !hub.exe_dir.empty()) {
                // Sensible portable default: a folder beside the launcher.
                copy_buf(hub.data_root_input, sizeof(hub.data_root_input),
                         (hub.exe_dir / "RetComM-Data").string());
            }
            changed = true;
        }
        if (changed) hub.data_root_plan_dirty = true;

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::BeginDisabled(!custom);
        if (path_field_with_browse("Retro folder", "##setup_data_root", hub.data_root_input,
                                   sizeof(hub.data_root_input), hub, window,
                                   FolderPickTarget::DataRoot, th))
            hub.data_root_plan_dirty = true;
        ImGui::EndDisabled();

        if (custom) {
            const auto& plan = hub.data_root_plan;
            ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
            push_wrap();
            if (plan.blocker.empty()) {
                ImGui::TextWrapped("Will use:\n  %s\n  %s", plan.to_config.string().c_str(),
                                   plan.to_data.string().c_str());
                if (!plan.note.empty()) {
                    ImGui::Dummy(ImVec2(0, 4));
                    ImGui::TextWrapped("%s", plan.note.c_str());
                }
                if (plan.existing_bytes > 0) {
                    ImGui::Dummy(ImVec2(0, 4));
                    ImGui::TextWrapped(
                        "Existing Retro data (%s) will be moved here when you finish.",
                        human_bytes(plan.existing_bytes).c_str());
                }
            } else {
                ImGui::TextWrapped("Pick a folder Retro can create and write to.");
            }
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            if (!plan.warning.empty()) {
                push_wrap();
                ImGui::TextColored(th.warn, "%s", plan.warning.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        ImGui::EndChild();

        pin_footer_row(warn_h);
        if (ImGui::Button("Back", ImVec2(100, 0))) {
            hub.setup_path = SetupPath::Chooser;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!root_ok);
        if (accent_button("Next", th, ImVec2(120, 0))) {
            const fs::path chosen =
                custom ? fs::path(hub.data_root_input).lexically_normal() : fs::path();
            hub.pending_data_root = chosen;
            hub.pending_data_root_change =
                custom ? !(retcomm::using_custom_root(hub.paths) && hub.paths.root == chosen)
                       : retcomm::using_custom_root(hub.paths);
            hub.setup_step = 1;
        }
        ImGui::EndDisabled();
        if (!root_ok) {
            ImGui::TextColored(th.warn, "%s", hub.data_root_plan.blocker.c_str());
        }
    } else if (hub.setup_step == 1) {
        const bool library_ok = hub.settings.library_root[0] != '\0';
        const float warn_h = library_ok ? 0.f : ImGui::GetTextLineHeightWithSpacing();
        const float footer_h = footer_reserve(warn_h);
        ImGui::BeginChild("##setup_adv0_body", ImVec2(0.f, -footer_h), ImGuiChildFlags_None);
        push_wrap();
        ImGui::TextWrapped(
            "Step 2 of 3 — Set your ROM library, BIOS, and game-saves folders. Suggested "
            "paths use ~/Emulation/{roms,bios,saves}. Optionally connect RomM for library "
            "sync later.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8));
        if (ImGui::Button("Use suggested paths")) {
            hub.apply_suggested_library_roots(/*overwrite_nonempty=*/true);
            hub.set_status("Suggested Emulation paths applied");
        }
        ImGui::Dummy(ImVec2(0, 10));

        if (path_field_with_browse("ROM library root", "##setup_library_root",
                                   hub.settings.library_root, sizeof(hub.settings.library_root),
                                   hub, window, FolderPickTarget::LibraryRoot, th))
            hub.settings.dirty = true;
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        push_wrap();
        ImGui::TextWrapped("Required — EmulationStation / RomM-style root (e.g. …/roms).");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 10));
        if (path_field_with_browse("BIOS root", "##setup_bios_root", hub.settings.bios_root,
                                   sizeof(hub.settings.bios_root), hub, window,
                                   FolderPickTarget::BiosRoot, th))
            hub.settings.dirty = true;
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        push_wrap();
        ImGui::TextWrapped("Optional — system BIOS / firmware dumps (e.g. …/bios).");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 10));
        if (path_field_with_browse("Game saves root", "##setup_saves_root",
                                   hub.settings.saves_root, sizeof(hub.settings.saves_root), hub,
                                   window, FolderPickTarget::SavesRoot, th))
            hub.settings.dirty = true;
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        push_wrap();
        ImGui::TextWrapped(
            "Recommended — SRAM / memcard library (e.g. …/saves). RomM sync and launches "
            "quarantine each game under …/<platform>/<title_id>/.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 14));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::TextColored(th.text_muted, "RomM (optional)");
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        push_wrap();
        ImGui::TextWrapped(
            "Leave blank for local-only. Use a Client API Token from RomM → Administration → "
            "Client API Tokens (Bearer rmm_…).");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(th.text_muted, "RomM Instance URL");
        if (hub_input_text("##setup_romm_base_url", hub.romm_settings.base_url,
                           sizeof(hub.romm_settings.base_url)))
            hub.romm_settings.dirty = true;
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(th.text_muted, "RomM Client API Key");
        if (hub_input_text("##setup_romm_api_token", hub.romm_settings.api_token,
                           sizeof(hub.romm_settings.api_token), ImGuiInputTextFlags_Password))
            hub.romm_settings.dirty = true;
        ImGui::EndChild();

        pin_footer_row(warn_h);
        if (ImGui::Button("Back", ImVec2(100, 0))) {
            hub.setup_step = 0;
            hub.setup_confirm_create_roots = false;
            hub.setup_missing_roots.clear();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!library_ok);
        if (accent_button("Next", th, ImVec2(120, 0))) {
            hub.collect_missing_setup_roots();
            if (!hub.setup_missing_roots.empty()) {
                hub.setup_confirm_create_roots = true;
            } else {
                advance_to_platform_step();
            }
        }
        ImGui::EndDisabled();
        if (!library_ok) {
            ImGui::TextColored(th.warn, "Choose a ROM library folder to continue.");
        }
    } else {
        const float footer_h = footer_reserve();
        // Outer body never scrolls — only the mappings table does.
        ImGui::BeginChild("##setup_adv1_body", ImVec2(0.f, -footer_h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 4.f));
        push_wrap();
        ImGui::PushStyleColor(ImGuiCol_Text, th.text);
        ImGui::TextWrapped(
            "Step 3 of 3 - Assign platform folder mappings.  Platform name is on the left, "
            "and on the right is a list of folder names to search.  Retro will create empty "
            "folders for any platforms that are missing from your library, to import new files "
            "you provide.");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Checkbox("Create missing platform folders under roms / bios / saves",
                        &hub.setup_create_platform_folders);

        const float add_row_h = ImGui::GetFrameHeightWithSpacing();
        const float table_h = std::max(120.f, ImGui::GetContentRegionAvail().y - add_row_h);
        ImGui::BeginChild("##setup_platform_table", ImVec2(0, table_h), ImGuiChildFlags_Borders);
        if (ImGui::BeginTable("setup_platform_folders", 3,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("platform", ImGuiTableColumnFlags_WidthFixed, 100.f);
            ImGui::TableSetupColumn("folders", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 36.f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(hub.settings.platform_folders.size()); ++i) {
                auto& row = hub.settings.platform_folders[static_cast<size_t>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (hub_input_text("##plat", row.platform, sizeof(row.platform)))
                    hub.settings.dirty = true;
                ImGui::TableNextColumn();
                if (hub_input_text("##folders", row.folders, sizeof(row.folders)))
                    hub.settings.dirty = true;
                ImGui::TableNextColumn();
                if (ImGui::Button("X")) {
                    hub.settings.platform_folders.erase(hub.settings.platform_folders.begin() +
                                                        i);
                    hub.settings.dirty = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        if (ImGui::Button("Add platform row")) hub.add_platform_folder_row();
        ImGui::PopStyleVar();
        ImGui::EndChild();

        pin_footer_row();
        if (ImGui::Button("Back", ImVec2(100, 0))) {
            hub.setup_step = 1;
        }
        ImGui::SameLine();
        if (accent_button("Finish", th, ImVec2(120, 0))) {
            std::string err;
            if (!hub.save_settings(&err)) {
                hub.append_log("setup save failed: " + err);
                hub.set_status("Setup save failed");
            } else if (hub.setup_create_platform_folders &&
                       !hub.create_setup_platform_folders(&err)) {
                hub.append_log("setup platform folders failed: " + err);
                hub.set_status("Could not create platform folders");
            } else if (!hub.save_romm_settings(&err, /*refresh_boxart=*/false)) {
                hub.append_log("setup RomM save failed: " + err);
                hub.set_status("Setup RomM save failed");
            } else if (!hub.complete_setup(&err)) {
                hub.append_log("setup marker failed: " + err);
                hub.set_status("Setup marker failed");
            } else if (hub.pending_data_root_change) {
                // Config, indexes and the setup marker are all written under the
                // current root by now, so they travel with the move. The job
                // relaunches; the scan prompt comes back on the next run.
                hub.append_log("First-time setup saved");
                hub.pending_data_root_change = false;
                hub.start_job(HubJob::MigrateDataRoot);
            } else {
                hub.set_status("Setup complete");
                hub.append_log("First-time setup saved");
                hub.show_setup_scan_prompt = true;
            }
        }
    }

    ImGui::EndPopup();
}

// Change where Retro keeps config + data. The move itself runs on the job
// thread (HubJob::MigrateDataRoot) and ends in a relaunch.
void draw_data_root_dialog(HubModel& hub, const Theme& th, SDL_Window* window) {
    if (!hub.show_data_root_dialog) return;
    ImGui::OpenPopup("Retro data folder###data_root_dialog");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(660.f, vp->WorkSize.x * 0.94f), 0),
                             ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Retro data folder###data_root_dialog", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    hub.refresh_data_root_plan();
    const auto& plan = hub.data_root_plan;
    const bool to_default = hub.data_root_input[0] == '\0';

    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 620.f);
    ImGui::TextWrapped(
        "Moves the toolchain, engine sources, installed games, and caches. Your ROM, BIOS, "
        "and saves folders are not affected.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(th.text_muted, "Current");
    ImGui::TextWrapped("%s", hub.paths.data_dir.string().c_str());

    ImGui::Dummy(ImVec2(0, 8));
    if (path_field_with_browse("New folder", "##data_root_new", hub.data_root_input,
                               sizeof(hub.data_root_input), hub, window,
                               FolderPickTarget::DataRoot, th))
        hub.data_root_plan_dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    if (retcomm::using_custom_root(hub.paths))
        ImGui::TextWrapped("Leave empty to go back to the default (%s).",
                           retcomm::default_os_data_dir().string().c_str());
    else
        ImGui::TextWrapped("Pick a folder on a drive with room — Retro creates "
                           "config/ and data/ inside it.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 10));
    if (!plan.blocker.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 620.f);
        ImGui::TextColored(th.warn, "%s", plan.blocker.c_str());
        ImGui::PopTextWrapPos();
    } else if (plan.same_as_current) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 620.f);
        ImGui::TextColored(th.text_muted, "%s", plan.note.c_str());
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextColored(th.text_muted, "New location");
        ImGui::TextWrapped("%s", plan.to_data.string().c_str());
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::TextColored(th.text_muted, "Existing data: %s",
                           human_bytes(plan.existing_bytes).c_str());
        ImGui::Dummy(ImVec2(0, 4));
        int mode = static_cast<int>(hub.data_root_mode);
        ImGui::RadioButton("Move existing data to the new folder", &mode, 0);
        ImGui::RadioButton("Use data already at the new folder", &mode, 1);
        ImGui::RadioButton("Start fresh (old folder left on disk)", &mode, 2);
        hub.data_root_mode = static_cast<retcomm::RootMigrationMode>(mode);
        if (!plan.warning.empty()) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 620.f);
            ImGui::TextColored(th.warn, "%s", plan.warning.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(th.warn, "Retro will restart.");
    }

    ImGui::Dummy(ImVec2(0, 12));
    const bool busy = hub.job_running.load();
    ImGui::BeginDisabled(!plan.blocker.empty() || plan.same_as_current || busy);
    if (accent_button("Continue", th, ImVec2(160, 0))) {
        hub.pending_data_root = to_default ? fs::path()
                                           : fs::path(hub.data_root_input).lexically_normal();
        hub.show_data_root_dialog = false;
        hub.start_job(HubJob::MigrateDataRoot);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        hub.show_data_root_dialog = false;
        ImGui::CloseCurrentPopup();
    }
    close_modal_on_outside_click();
    ImGui::EndPopup();
}

void draw_setup_scan_prompt(HubModel& hub, const Theme& th) {
    if (!hub.show_setup_scan_prompt) return;
    ImGui::OpenPopup("Scan library?###setup_scan_prompt");
    center_modal_next();
    if (!ImGui::BeginPopupModal("Scan library?###setup_scan_prompt", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
    ImGui::TextWrapped(
        "Folder layout is ready. Scan your library now? This indexes ROMs, BIOS, and save "
        "files (and refreshes the catalog first).");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 14));
    if (accent_button("Scan now", th, ImVec2(140, 0))) {
        hub.show_setup_scan_prompt = false;
        hub.job_prefetch_catalog = true;
        hub.start_job(HubJob::ScanRoms);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Not now", ImVec2(120, 0))) {
        hub.show_setup_scan_prompt = false;
        hub.set_status("Setup complete — refresh later from Library");
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_romm_settings_panel(HubModel& hub, const Theme& th) {
    ImGui::BeginChild("romm_settings", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      page_wheel_flags(hub));
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("ROMM SYNC SETTINGS");
    ImGui::PopStyleColor();
    ImGui::TextWrapped(
        "Connect to a RomM instance for future sync. Use a Client API Token from "
        "RomM → Administration → Client API Tokens (Bearer rmm_…). No username/password "
        "needed in Retro.");
    ImGui::Separator();

    ImGui::TextColored(th.text_muted, "RomM Instance URL");
    if (hub_input_text("##romm_base_url", hub.romm_settings.base_url,
                       sizeof(hub.romm_settings.base_url)))
        hub.romm_settings.dirty = true;

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextColored(th.text_muted, "RomM Client API Key");
    if (hub_input_text("##romm_api_token", hub.romm_settings.api_token,
                       sizeof(hub.romm_settings.api_token), ImGuiInputTextFlags_Password))
        hub.romm_settings.dirty = true;

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Checkbox("Sync Boxart", &hub.romm_settings.sync_boxart))
        hub.romm_settings.dirty = true;
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    if (hub.romm_settings.sync_boxart)
        ImGui::TextWrapped("On: all covers use RomM (requires URL + API key).");
    else
        ImGui::TextWrapped("Off (default): all covers use Libretro Named_Boxarts.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    if (hub.cfg.romm.enabled())
        ImGui::TextWrapped("Configured: %s", hub.cfg.romm.base_url.c_str());
    else
        ImGui::TextWrapped("Not configured — set a URL and save to enable RomM.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 12));
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "RomM matching is on-demand during Install. Enable Sync Boxart here, then "
        "use Find Missing Boxart in Library Settings to refresh covers.");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 12));
    if (accent_button("Save", th, ImVec2(160, 0))) {
        std::string err;
        if (!hub.save_romm_settings(&err)) {
            hub.append_log("RomM settings save failed: " + err);
            hub.set_status("Save failed");
        } else {
            hub.show_toast("Saved!");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        hub.show_romm_settings = false;
        hub.romm_settings.dirty = false;
    }
    if (hub.romm_settings.dirty) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(th.warn, "unsaved changes");
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(th.text_muted, "%s", hub.paths.config_path.string().c_str());
    ImGui::EndChild();
}

// ---- Settings rows ---------------------------------------------------------
// One row of a platform settings card: the label is pinned to the panel's left
// edge, the control to its right edge. Reading down a card you get a column of
// names and a column of controls, instead of a ragged middle that moves with
// whatever the longest label happens to be.
//
// The caller submits its control immediately after; `ctrl_w` is that control's
// width, and the item width is pre-set so a combo fills it exactly.
// Returns the width the control actually got: a long label on a narrow card
// squeezes it rather than pushing it off the right edge.
float settings_row(const char* label, const Theme& th, float ctrl_w) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(th.text_muted, "%s", label);
    const float right = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine();
    const float after = ImGui::GetCursorPosX() + 8.f;
    float w = ctrl_w;
    if (right - after < w) w = (std::max)(56.f, right - after);
    ImGui::SetCursorPosX((std::max)(after, right - w));
    ImGui::SetNextItemWidth(w);
    return w;
}

// Width every dropdown in a settings card shares, so the right edge is a line
// rather than a staircase.
constexpr float kSettingsCtrlW = 190.f;

// Right-aligned checkbox row. The label is the row's, not the checkbox's, so
// `id` must be a "##"-prefixed identifier.
bool settings_checkbox(const char* label, const char* id, const Theme& th, bool* value) {
    settings_row(label, th, ImGui::GetFrameHeight());
    return ImGui::Checkbox(id, value);
}

// Right-aligned dropdown over `count` labels; *value is the chosen index.
bool settings_combo(const char* label, const char* id, const Theme& th,
                    const char* const* items, int count, int* value,
                    float w = kSettingsCtrlW) {
    const int cur = std::clamp(*value, 0, count - 1);
    settings_row(label, th, w);
    bool changed = false;
    if (ImGui::BeginCombo(id, items[cur])) {
        for (int i = 0; i < count; ++i) {
            const bool sel = (i == cur);
            if (ImGui::Selectable(items[i], sel) && i != *value) {
                *value = i;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Right-aligned dropdown over a fixed set of values (window widths, sample
// rates, rewind depths). An off-list *value previews as the first entry but is
// left alone until the user picks something.
bool settings_combo_values(const char* label, const char* id, const Theme& th,
                           const char* const* items, const int* values, int count, int* value,
                           float w = kSettingsCtrlW) {
    int cur = 0;
    for (int i = 0; i < count; ++i)
        if (values[i] == *value) {
            cur = i;
            break;
        }
    settings_row(label, th, w);
    bool changed = false;
    if (ImGui::BeginCombo(id, items[cur])) {
        for (int i = 0; i < count; ++i) {
            const bool sel = (i == cur);
            if (ImGui::Selectable(items[i], sel) && values[i] != *value) {
                *value = values[i];
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Two-way dropdown for a setting that is a bool but reads as a choice
// ("Nearest" / "Bilinear"), not as an on/off.
bool settings_combo_bool(const char* label, const char* id, const Theme& th, const char* off_label,
                         const char* on_label, bool* value, float w = kSettingsCtrlW) {
    const char* items[2] = {off_label, on_label};
    int idx = *value ? 1 : 0;
    if (settings_combo(label, id, th, items, 2, &idx, w)) {
        *value = idx != 0;
        return true;
    }
    return false;
}

// Right-aligned binding button (hotkey / pad chord rows).
bool settings_bind_button(const char* label, const Theme& th, const char* text, bool capturing,
                          float w = kSettingsCtrlW) {
    const float got = settings_row(label, th, w);
    if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
    const bool hit = ImGui::Button(text, ImVec2(got, 0));
    if (capturing) ImGui::PopStyleColor();
    return hit;
}

SDL_Gamepad* gamepad_handle_for_id(SDL_JoystickID id);   /* defined below */

// Controller-hotkey capture. Unlike the keyboard path, this commits on RELEASE:
// a chord is several buttons held at once, so sampling the first button-down
// would record "select" every time. Accumulate everything held while the player
// holds it, then commit the union once they let go.
void poll_psx_pad_hotkey_capture(HubModel& hub) {
    auto& draft = hub.psx_settings;
    if (draft.capturing_pad_hotkey < 0) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        draft.capturing_pad_hotkey = -1;
        draft.pad_hotkey_mask = 0;
        return;
    }

    unsigned held = 0;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            SDL_Gamepad* pad = gamepad_handle_for_id(ids[i]);
            if (!pad) continue;
            // Union across pads: which controller the player grabbed is not
            // something they should have to tell us first.
            for (int b = 0; b < 21; ++b) {
                if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b)))
                    held |= (1u << b);
            }
        }
        SDL_free(ids);
    }

    if (held) {
        draft.pad_hotkey_mask |= held;
        return;                     // still holding — keep accumulating
    }
    if (!draft.pad_hotkey_mask) return;   // nothing pressed yet

    const unsigned mask = draft.pad_hotkey_mask;
    int value;
    if (mask & (mask - 1)) {              // more than one bit -> a chord
        value = retcomm::PsxPlatformSettings::kPadBindCombo + static_cast<int>(mask);
    } else {                              // single button: 1 + code
        int code = 0;
        while (((mask >> code) & 1u) == 0) ++code;
        value = code + 1;
    }
    switch (draft.capturing_pad_hotkey) {
        case 0: draft.settings.hotkey_pad_rewind = value; break;
        case 1: draft.settings.hotkey_pad_save_state_menu = value; break;
        case 2: draft.settings.hotkey_pad_fast_forward = value; break;
        default: draft.settings.hotkey_pad_fast_forward_toggle = value; break;
    }
    draft.dirty = true;
    draft.capturing_pad_hotkey = -1;
    draft.pad_hotkey_mask = 0;
}

// One frame of keyboard hotkey capture, in the recomp-ui / host_keymap name
// vocabulary ("Ctrl+Alt+Shift+Key"). Returns 1 with `out` filled when a key was
// pressed, -1 when Escape cancelled, 0 while still waiting.
int poll_hotkey_capture_key(std::string& out) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return -1;
    for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END;
         key = (ImGuiKey)(key + 1)) {
        if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift ||
            key == ImGuiKey_RightShift || key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
            key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper || key == ImGuiKey_Escape)
            continue;
        if (!ImGui::IsKeyPressed(key, false)) continue;
        std::string name;
        if (ImGui::GetIO().KeyCtrl) name += "Ctrl+";
        if (ImGui::GetIO().KeyAlt) name += "Alt+";
        if (ImGui::GetIO().KeyShift) name += "Shift+";
        const char* kn = ImGui::GetKeyName(key);
        if (!kn || !kn[0]) continue;
        // Match recomp-ui / host_keymap vocabulary for common keys.
        if (std::strcmp(kn, "Enter") == 0) name += "Return";
        else if (std::strcmp(kn, "KeypadEnter") == 0) name += "Keypad Enter";
        else if (std::strncmp(kn, "Keypad", 6) == 0) {
            name += "Keypad ";
            name += (kn + 6);
        } else {
            name += kn;
        }
        out = name;
        return 1;
    }
    return 0;
}

void poll_psx_hotkey_capture(HubModel& hub) {
    auto& draft = hub.psx_settings;
    if (draft.capturing_hotkey < 0 ||
        draft.capturing_hotkey >= retcomm::PsxPlatformSettings::kHotkeyCount)
        return;
    std::string name;
    const int r = poll_hotkey_capture_key(name);
    if (r < 0) {
        draft.capturing_hotkey = -1;
        return;
    }
    if (r > 0) {
        draft.settings.hotkeys[static_cast<size_t>(draft.capturing_hotkey)] = name;
        draft.dirty = true;
        draft.capturing_hotkey = -1;
    }
}

struct HubGamepadOpt {
    char guid[40]{};
    char name[64]{};
    bool live = false;
    SDL_JoystickID id = 0;
};

// SDL3 only emits GAMEPAD_* events for pads that stay open. Keep a small table
// of open handles (same idea as recomp-ui launcher_input_poll).
constexpr int kMaxOpenHubPads = 16;
struct OpenHubPad {
    SDL_JoystickID id = 0;
    SDL_Gamepad* handle = nullptr;
    char guid[64]{};
};
OpenHubPad g_open_pads[kMaxOpenHubPads]{};

bool guid_eq_ci(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        const unsigned char ca = static_cast<unsigned char>(*a++);
        const unsigned char cb = static_cast<unsigned char>(*b++);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return *a == *b;
}

void hub_close_all_gamepads() {
    for (int i = 0; i < kMaxOpenHubPads; ++i) {
        if (g_open_pads[i].handle) SDL_CloseGamepad(g_open_pads[i].handle);
        g_open_pads[i] = {};
    }
}

// Open newly connected pads; close disconnected ones. Call once per frame.
void hub_sync_open_gamepads() {
    bool keep[kMaxOpenHubPads]{};
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count; ++i) {
            const SDL_JoystickID id = ids[i];
            int slot = -1;
            for (int j = 0; j < kMaxOpenHubPads; ++j) {
                if (g_open_pads[j].handle && g_open_pads[j].id == id) {
                    slot = j;
                    break;
                }
            }
            if (slot < 0) {
                for (int j = 0; j < kMaxOpenHubPads; ++j) {
                    if (g_open_pads[j].handle) continue;
                    SDL_Gamepad* gp = SDL_OpenGamepad(id);
                    if (!gp) break;
                    g_open_pads[j].handle = gp;
                    g_open_pads[j].id = id;
                    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), g_open_pads[j].guid,
                                     static_cast<int>(sizeof(g_open_pads[j].guid)));
                    slot = j;
                    break;
                }
            }
            if (slot >= 0) keep[slot] = true;
        }
        SDL_free(ids);
    }
    for (int j = 0; j < kMaxOpenHubPads; ++j) {
        if (!g_open_pads[j].handle || keep[j]) continue;
        SDL_CloseGamepad(g_open_pads[j].handle);
        g_open_pads[j] = {};
    }
}

// Snapshot of pad state when bind capture starts — poll path commits the first
// button/axis that rises above this baseline (covers missed GAMEPAD_* events).
struct CapturePadBaseline {
    bool valid = false;
    SDL_JoystickID id = 0;
    bool buttons[static_cast<int>(SDL_GAMEPAD_BUTTON_COUNT)]{};
    Sint16 axes[static_cast<int>(SDL_GAMEPAD_AXIS_COUNT)]{};
    // Ignore commits until this SDL tick (avoids click/press bleed into capture).
    Uint64 arm_until_ms = 0;
    // Poll debounce: require the same candidate for N frames before committing.
    int debounce_kind = 0; // 1 button, 2 axis
    int debounce_code = -1;
    int debounce_dir = 0;
    int debounce_frames = 0;
};
CapturePadBaseline g_capture_baseline{};
constexpr int kCaptureArmMs = 180;
constexpr int kCapturePollDebounceFrames = 3;
constexpr int kCaptureAxisCommit = 20000;

SDL_Gamepad* gamepad_handle_for_id(SDL_JoystickID id) {
    if (!id) return nullptr;
    SDL_Gamepad* pad = SDL_GetGamepadFromID(id);
    if (pad) return pad;
    for (int i = 0; i < kMaxOpenHubPads; ++i) {
        if (g_open_pads[i].id == id && g_open_pads[i].handle) return g_open_pads[i].handle;
    }
    return nullptr;
}

void snapshot_capture_baseline(SDL_JoystickID id) {
    const Uint64 arm = g_capture_baseline.arm_until_ms;
    g_capture_baseline = {};
    g_capture_baseline.id = id;
    g_capture_baseline.arm_until_ms = arm;
    SDL_Gamepad* pad = gamepad_handle_for_id(id);
    if (!pad) return;
    for (int b = 0; b < static_cast<int>(SDL_GAMEPAD_BUTTON_COUNT); ++b)
        g_capture_baseline.buttons[b] =
            SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b)) != 0;
    for (int a = 0; a < static_cast<int>(SDL_GAMEPAD_AXIS_COUNT); ++a)
        g_capture_baseline.axes[a] = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a));
    g_capture_baseline.valid = true;
}

void clear_capture_baseline() { g_capture_baseline = {}; }

bool capture_armed() {
    return SDL_GetTicks() >= g_capture_baseline.arm_until_ms;
}

void reset_capture_debounce() {
    g_capture_baseline.debounce_kind = 0;
    g_capture_baseline.debounce_code = -1;
    g_capture_baseline.debounce_dir = 0;
    g_capture_baseline.debounce_frames = 0;
}

// PSX slot layout: 0-3 d-pad, 9/11 L2/R2, 16-23 stick directions.
bool slot_is_dpad(int slot) { return slot >= 0 && slot <= 3; }
bool slot_is_stick_dir(int slot) { return slot >= 16 && slot < 24; }
bool slot_is_l2(int slot) { return slot == 9; }
bool slot_is_r2(int slot) { return slot == 11; }

bool is_dpad_button(int button) {
    return button == static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_UP) ||
           button == static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ||
           button == static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ||
           button == static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
}

bool is_stick_axis(int axis) {
    return axis == static_cast<int>(SDL_GAMEPAD_AXIS_LEFTX) ||
           axis == static_cast<int>(SDL_GAMEPAD_AXIS_LEFTY) ||
           axis == static_cast<int>(SDL_GAMEPAD_AXIS_RIGHTX) ||
           axis == static_cast<int>(SDL_GAMEPAD_AXIS_RIGHTY);
}

bool is_trigger_axis(int axis) {
    return axis == static_cast<int>(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) ||
           axis == static_cast<int>(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
}

// Slot 0-3 = Up/Down/Left/Right — require that exact SDL D-pad button so a
// Windows Xbox mis-report of Left as Up cannot store dpup on the Left chip.
int expected_dpad_button(int slot) {
    switch (slot) {
    case 0: return static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_UP);
    case 1: return static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    case 2: return static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    case 3: return static_cast<int>(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    default: return -1;
    }
}

// Slot-aware accept filters — stops triggers/face noise from stealing D-pad binds.
bool capture_accepts_button(int slot, int button) {
    if (button < 0 || button >= static_cast<int>(SDL_GAMEPAD_BUTTON_COUNT)) return false;
    if (slot_is_dpad(slot)) return button == expected_dpad_button(slot);
    if (slot_is_stick_dir(slot)) return is_dpad_button(button); // digital fold remaps
    if (slot_is_l2(slot) || slot_is_r2(slot)) return false; // triggers: axes only
    return !is_dpad_button(button); // face/shoulders — reject stray D-pad edges
}

bool capture_accepts_axis(int slot, int axis) {
    if (axis < 0 || axis >= static_cast<int>(SDL_GAMEPAD_AXIS_COUNT)) return false;
    if (slot_is_dpad(slot)) return false;
    if (slot_is_stick_dir(slot)) return is_stick_axis(axis);
    if (slot_is_l2(slot)) return axis == static_cast<int>(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    if (slot_is_r2(slot)) return axis == static_cast<int>(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    // Other digital slots: never bind stick or trigger axes by accident.
    if (is_stick_axis(axis) || is_trigger_axis(axis)) return false;
    return true;
}

// When several D-pad buttons edge at once (diagonal), wait for a single cardinal.
int sole_new_dpad_button(SDL_Gamepad* pad) {
    int hit = -1;
    int n = 0;
    static const SDL_GamepadButton kDpad[] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    };
    for (SDL_GamepadButton b : kDpad) {
        const int bi = static_cast<int>(b);
        const bool now = SDL_GetGamepadButton(pad, b) != 0;
        if (now && !g_capture_baseline.buttons[bi]) {
            ++n;
            hit = bi;
        }
    }
    return n == 1 ? hit : -1;
}

SDL_JoystickID find_live_pad_id(const char* guid); // defined below

// Resolve SDL button/axis name the same way the runtime input.ini parser expects.
void fill_bind_source_string(int kind, int code, int axis_dir, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (kind == 1) {
        const char* n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(code));
        if (!n || !n[0]) return;
        if (std::strcmp(n, "south") == 0) n = "a";
        else if (std::strcmp(n, "east") == 0) n = "b";
        else if (std::strcmp(n, "west") == 0) n = "x";
        else if (std::strcmp(n, "north") == 0) n = "y";
        std::snprintf(out, cap, "%s", n);
    } else if (kind == 2) {
        const char* n = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(code));
        if (!n || !n[0]) n = "axis";
        std::snprintf(out, cap, "%s%c", n, axis_dir < 0 ? '-' : '+');
    }
}

void append_live_sdl_dpad_hint(SDL_Gamepad* pad, char* out, size_t cap) {
    if (!pad || !out || cap < 8) return;
    char bits[64]{};
    auto add = [&](SDL_GamepadButton b, const char* name) {
        if (!SDL_GetGamepadButton(pad, b)) return;
        if (bits[0]) std::strncat(bits, "+", sizeof(bits) - std::strlen(bits) - 1);
        std::strncat(bits, name, sizeof(bits) - std::strlen(bits) - 1);
    };
    add(SDL_GAMEPAD_BUTTON_DPAD_UP, "up");
    add(SDL_GAMEPAD_BUTTON_DPAD_DOWN, "down");
    add(SDL_GAMEPAD_BUTTON_DPAD_LEFT, "left");
    add(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "right");
    if (!bits[0]) return;
    const size_t used = std::strlen(out);
    if (used + 16 >= cap) return;
    std::snprintf(out + used, cap - used, "\nSDL D-pad: %s", bits);
}

// True when the bound input.ini source string is currently pressed on `pad`.
bool gamepad_bound_source_pressed(SDL_Gamepad* pad, const char* raw, int deadzone_raw) {
    if (!pad || !raw || !raw[0]) return false;
    char s[48]{};
    std::snprintf(s, sizeof(s), "%s", raw);
    for (char* p = s; *p; ++p) *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    if (char* comma = std::strchr(s, ',')) *comma = '\0';

    int dir = 0;
    const size_t n = std::strlen(s);
    if (n >= 2) {
        const char last = s[n - 1];
        const char prev = s[n - 2];
        if ((last == '+' || last == '-') &&
            (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_')) {
            dir = (last == '+') ? +1 : -1;
            s[n - 1] = '\0';
        }
    }

    auto as_btn = [&](SDL_GamepadButton b) {
        return SDL_GetGamepadButton(pad, b) != 0;
    };
    auto as_axis = [&](SDL_GamepadAxis a, int want_dir) {
        const int v = static_cast<int>(SDL_GetGamepadAxis(pad, a));
        const int dz = deadzone_raw > 0 ? deadzone_raw : 8000;
        if (want_dir < 0) return v < -dz;
        return v > dz;
    };

    // Face / legacy aliases first (match runtime parse_controller_source).
    if (std::strcmp(s, "a") == 0 || std::strcmp(s, "south") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_SOUTH);
    if (std::strcmp(s, "b") == 0 || std::strcmp(s, "east") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_EAST);
    if (std::strcmp(s, "x") == 0 || std::strcmp(s, "west") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_WEST);
    if (std::strcmp(s, "y") == 0 || std::strcmp(s, "north") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_NORTH);
    if (std::strcmp(s, "back") == 0 || std::strcmp(s, "view") == 0 || std::strcmp(s, "select") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_BACK);
    if (std::strcmp(s, "start") == 0 || std::strcmp(s, "menu") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_START);
    if (std::strcmp(s, "guide") == 0) return as_btn(SDL_GAMEPAD_BUTTON_GUIDE);
    if (std::strcmp(s, "leftstick") == 0) return as_btn(SDL_GAMEPAD_BUTTON_LEFT_STICK);
    if (std::strcmp(s, "rightstick") == 0) return as_btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    if (std::strcmp(s, "leftshoulder") == 0 || std::strcmp(s, "lb") == 0 || std::strcmp(s, "l1") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    if (std::strcmp(s, "rightshoulder") == 0 || std::strcmp(s, "rb") == 0 ||
        std::strcmp(s, "r1") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    if (std::strcmp(s, "dpup") == 0 || std::strcmp(s, "dpadup") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_DPAD_UP);
    if (std::strcmp(s, "dpdown") == 0 || std::strcmp(s, "dpaddown") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    if (std::strcmp(s, "dpleft") == 0 || std::strcmp(s, "dpadleft") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    if (std::strcmp(s, "dpright") == 0 || std::strcmp(s, "dpadright") == 0)
        return as_btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    if (std::strcmp(s, "lefttrigger") == 0 || std::strcmp(s, "lt") == 0 || std::strcmp(s, "l2") == 0)
        return as_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, dir == 0 ? +1 : dir);
    if (std::strcmp(s, "righttrigger") == 0 || std::strcmp(s, "rt") == 0 ||
        std::strcmp(s, "r2") == 0)
        return as_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, dir == 0 ? +1 : dir);
    if (std::strcmp(s, "leftx") == 0 && dir != 0) return as_axis(SDL_GAMEPAD_AXIS_LEFTX, dir);
    if (std::strcmp(s, "lefty") == 0 && dir != 0) return as_axis(SDL_GAMEPAD_AXIS_LEFTY, dir);
    if (std::strcmp(s, "rightx") == 0 && dir != 0) return as_axis(SDL_GAMEPAD_AXIS_RIGHTX, dir);
    if (std::strcmp(s, "righty") == 0 && dir != 0) return as_axis(SDL_GAMEPAD_AXIS_RIGHTY, dir);

    const SDL_GamepadButton btn = SDL_GetGamepadButtonFromString(s);
    if (btn != SDL_GAMEPAD_BUTTON_INVALID) return as_btn(btn);
    const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(s);
    if (axis != SDL_GAMEPAD_AXIS_INVALID) return as_axis(axis, dir == 0 ? +1 : dir);
    return false;
}

void cancel_psx_bind_capture(HubModel& hub) {
    hub.psx_settings.capturing_bind = -1;
    hub.psx_settings.map_all_active = false;
    hub.psx_settings.map_all_wait_release = false;
    hub.psx_settings.map_all_step = 0;
    clear_capture_baseline();
}

void begin_psx_bind_capture(HubModel& hub, int button, bool is_pad) {
    hub.psx_settings.capturing_bind = button;
    hub.psx_settings.capture_is_pad = is_pad;
    if (!hub.psx_settings.map_all_active) hub.psx_settings.map_all_wait_release = false;
    clear_capture_baseline();
    if (!is_pad) return;
    g_capture_baseline.arm_until_ms = SDL_GetTicks() + static_cast<Uint64>(kCaptureArmMs);
    reset_capture_debounce();
    const int p = hub.psx_settings.configuring_player;
    if (p < 0) return;
    const std::string& guid =
        hub.psx_settings.settings.player_guid[static_cast<size_t>(p)];
    SDL_JoystickID id = find_live_pad_id(guid.c_str());
    if (!id) {
        int live_n = 0;
        SDL_JoystickID only = 0;
        for (int i = 0; i < kMaxOpenHubPads; ++i) {
            if (!g_open_pads[i].handle) continue;
            ++live_n;
            only = g_open_pads[i].id;
        }
        if (live_n == 1) id = only;
    }
    if (id) snapshot_capture_baseline(id);
}

void begin_psx_map_all(HubModel& hub, bool is_pad) {
    hub.psx_settings.map_all_active = true;
    hub.psx_settings.map_all_wait_release = false;
    hub.psx_settings.map_all_step = 0;
    begin_psx_bind_capture(hub, retcomm::psx_pad_map_all_order()[0], is_pad);
}

void advance_psx_map_all(HubModel& hub) {
    auto& d = hub.psx_settings;
    if (!d.map_all_active) return;
    d.map_all_step++;
    if (d.map_all_step >= retcomm::kPsxPadButtonCount) {
        cancel_psx_bind_capture(hub);
        return;
    }
    // Gamepads need a release/rest gate so one press doesn't fill every slot.
    d.map_all_wait_release = d.capture_is_pad;
    begin_psx_bind_capture(hub, retcomm::psx_pad_map_all_order()[d.map_all_step], d.capture_is_pad);
}

SDL_JoystickID find_live_pad_id(const char* guid) {
    if (!guid || !guid[0]) return 0;
    for (int i = 0; i < kMaxOpenHubPads; ++i) {
        if (g_open_pads[i].handle && guid_eq_ci(g_open_pads[i].guid, guid))
            return g_open_pads[i].id;
    }
    // Fallback if sync hasn't run yet this frame.
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return 0;
    SDL_JoystickID hit = 0;
    for (int i = 0; i < n; ++i) {
        char g[64]{};
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), g, static_cast<int>(sizeof(g)));
        if (guid_eq_ci(g, guid)) {
            hit = ids[i];
            break;
        }
    }
    SDL_free(ids);
    return hit;
}

bool gamepad_at_rest(SDL_JoystickID id) {
    SDL_Gamepad* pad = gamepad_handle_for_id(id);
    if (!pad) return true;
    for (int b = 0; b < static_cast<int>(SDL_GAMEPAD_BUTTON_COUNT); ++b) {
        if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b))) return false;
    }
    for (int a = 0; a < static_cast<int>(SDL_GAMEPAD_AXIS_COUNT); ++a) {
        const Sint16 v = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a));
        if (v > 8000 || v < -8000) return false;
    }
    return true;
}

// Live pads + remembered GUID profiles (input.ini) + currently assigned slots.
void collect_hub_gamepads(HubModel& hub, std::vector<HubGamepadOpt>& out) {
    out.clear();
    const auto& s = hub.psx_settings.settings;
    auto already = [&](const char* guid) {
        if (!guid || !guid[0]) return true;
        for (const auto& o : out)
            if (guid_eq_ci(o.guid, guid)) return true;
        return false;
    };
    auto push = [&](const char* guid, const char* name, bool live, SDL_JoystickID id) {
        if (!guid || !guid[0] || already(guid)) return;
        HubGamepadOpt o;
        std::snprintf(o.guid, sizeof(o.guid), "%s", guid);
        const char* nm = (name && name[0] && std::strcmp(name, "Gamepad") != 0) ? name : "Controller";
        std::snprintf(o.name, sizeof(o.name), "%s", nm);
        o.live = live;
        o.id = id;
        out.push_back(o);
    };

    const int known = retcomm::psx_pad_binds_known_count(hub.paths);
    for (int i = 0; i < known; ++i) {
        char guid[40]{}, name[64]{};
        if (!retcomm::psx_pad_binds_known_at(hub.paths, i, guid, sizeof(guid), name, sizeof(name)))
            continue;
        push(guid, name, false, 0);
    }

    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (ids) {
        for (int i = 0; i < n; ++i) {
            const SDL_JoystickID id = ids[i];
            char guid_str[64]{};
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid_str, static_cast<int>(sizeof(guid_str)));
            const char* name = SDL_GetGamepadNameForID(id);
            // Refresh live entry / prefer custom registry name.
            bool found = false;
            for (auto& o : out) {
                if (!guid_eq_ci(o.guid, guid_str)) continue;
                o.live = true;
                o.id = id;
                if (!retcomm::psx_pad_binds_name_is_custom(hub.paths, guid_str) && name && name[0] &&
                    std::strcmp(name, "Gamepad") != 0)
                    std::snprintf(o.name, sizeof(o.name), "%s", name);
                found = true;
                break;
            }
            if (!found) push(guid_str, name, true, id);
        }
        SDL_free(ids);
    }
    for (int p = 0; p < retcomm::PsxPlatformSettings::kMaxPlayers; ++p) {
        if (s.player_src[static_cast<size_t>(p)] != 2) continue;
        const std::string& g = s.player_guid[static_cast<size_t>(p)];
        if (g.empty()) continue;
        char nm[64]{};
        retcomm::psx_pad_binds_name(hub.paths, g, nm, sizeof(nm));
        push(g.c_str(), nm[0] ? nm : "Controller", false, 0);
    }
}

const char* psx_player_src_preview(const retcomm::PsxPlatformSettings& s, int p,
                                   const std::vector<HubGamepadOpt>& pads) {
    const int src = std::clamp(s.player_src[static_cast<size_t>(p)], 0, 2);
    if (src == 0) return "None";
    if (src == 1) return "Keyboard";
    const std::string& guid = s.player_guid[static_cast<size_t>(p)];
    if (guid.empty()) return "Gamepad";
    for (const auto& pad : pads) {
        if (guid == pad.guid) return pad.name;
    }
    return guid.c_str();
}

void assign_psx_player_source(HubModel& hub, int p, int src, const char* guid, const char* name,
                              bool* dirty) {
    auto& s = hub.psx_settings.settings;
    s.player_src[static_cast<size_t>(p)] = src;
    if (src == 1) {
        s.player_guid[static_cast<size_t>(p)].clear();
        s.player_mode[static_cast<size_t>(p)] = 2; // digital default for keyboard
    } else if (src == 2 && guid && guid[0]) {
        s.player_guid[static_cast<size_t>(p)] = guid;
        retcomm::psx_pad_binds_remember(hub.paths, guid, name ? name : "Controller", -1);
        const int dz = retcomm::psx_pad_binds_deadzone(hub.paths, guid);
        s.player_deadzone[static_cast<size_t>(p)] =
            std::clamp((dz * 32767 + 50) / 100, 0, 32767);
        // Default DualShock/analog only when unset — do not clobber an explicit
        // digital/analog choice (re-selecting the same pad used to wipe digital).
        if (s.player_mode[static_cast<size_t>(p)] != 1 &&
            s.player_mode[static_cast<size_t>(p)] != 2)
            s.player_mode[static_cast<size_t>(p)] = 1;
    } else {
        s.player_guid[static_cast<size_t>(p)].clear();
    }
    *dirty = true;
}

void draw_psx_player_source_combo(HubModel& hub, int p, const std::vector<HubGamepadOpt>& pads,
                                  bool* dirty) {
    auto& s = hub.psx_settings.settings;
    const char* preview = psx_player_src_preview(s, p, pads);
    ImGui::SetNextItemWidth(-1.f);
    if (!ImGui::BeginCombo("##src", preview)) return;

    if (ImGui::Selectable("None", s.player_src[static_cast<size_t>(p)] == 0))
        assign_psx_player_source(hub, p, 0, nullptr, nullptr, dirty);
    if (ImGui::Selectable("Keyboard", s.player_src[static_cast<size_t>(p)] == 1))
        assign_psx_player_source(hub, p, 1, nullptr, nullptr, dirty);

    if (pads.empty()) {
        ImGui::BeginDisabled();
        ImGui::Selectable("(no gamepad connected)");
        ImGui::EndDisabled();
    } else {
        for (const auto& pad : pads) {
            bool claimed = false;
            for (int o = 0; o < retcomm::PsxPlatformSettings::kMaxPlayers; ++o) {
                if (o == p) continue;
                if (s.player_src[static_cast<size_t>(o)] == 2 &&
                    s.player_guid[static_cast<size_t>(o)] == pad.guid) {
                    claimed = true;
                    break;
                }
            }
            char label[96];
            if (pad.live)
                std::snprintf(label, sizeof(label), "%s", pad.name);
            else
                std::snprintf(label, sizeof(label), "%s (disconnected)", pad.name);
            const bool sel = s.player_src[static_cast<size_t>(p)] == 2 &&
                             s.player_guid[static_cast<size_t>(p)] == pad.guid;
            if (claimed) ImGui::BeginDisabled();
            if (ImGui::Selectable(label, sel) && !claimed)
                assign_psx_player_source(hub, p, 2, pad.guid, pad.name, dirty);
            if (claimed) ImGui::EndDisabled();
        }
    }
    ImGui::EndCombo();
}

// Overlay chips — normalized centers on the stylized PS1 pad art
// (assets/controllers/pad_analog.png / pad_digital.png).
struct PsxPadHit {
    int button;
    float nx, ny;
};

// Short labels drawn on chips (binding string goes in the tooltip).
const char* psx_pad_chip_label(int b) {
    static const char* k[] = {
        "U",    "D",    "L",    "R",     // d-pad
        "Tri",  "Cir",  "Cro",  "Sq",    // face
        "L1",   "L2",   "R1",   "R2",    // shoulders
        "L3",   "R3",   "Start", "Select",
        "LS^",  "LSv",  "LS<",  "LS>",   // left stick
        "RS^",  "RSv",  "RS<",  "RS>",   // right stick
    };
    if (b < 0 || b >= retcomm::kPsxPadButtonCount) return "?";
    return k[b];
}

const PsxPadHit* psx_pad_hits(int* count) {
    // Exact centers from the procedural flat-retro pad art (720x400).
    // D-pad / stick dirs are pushed out from the art centers so chips don't overlap
    // after the UV crop zoom (chips are wider than the physical buttons).
    static const PsxPadHit kHits[] = {
        {9, 0.274f, 0.257f},  // L2
        {8, 0.268f, 0.302f},  // L1
        {11, 0.726f, 0.257f}, // R2
        {10, 0.733f, 0.302f}, // R1
        {0, 0.311f, 0.438f},  // Up
        {1, 0.311f, 0.572f},  // Down
        {2, 0.2665f, 0.505f},  // Left
        {3, 0.355f, 0.505f},  // Right
        {4, 0.689f, 0.421f},  // Triangle
        {5, 0.736f, 0.505f},  // Circle
        {6, 0.689f, 0.589f},  // Cross
        {7, 0.642f, 0.505f},  // Square
        {15, 0.447f, 0.520f}, // Select
        {14, 0.553f, 0.520f}, // Start
        {16, 0.409f, 0.610f}, // LS Up
        {17, 0.409f, 0.740f}, // LS Down
        {18, 0.350f, 0.675f}, // LS Left
        {19, 0.468f, 0.675f}, // LS Right
        {12, 0.409f, 0.675f}, // L3
        {20, 0.591f, 0.610f}, // RS Up
        {21, 0.591f, 0.740f}, // RS Down
        {22, 0.532f, 0.675f}, // RS Left
        {23, 0.650f, 0.675f}, // RS Right
        {13, 0.591f, 0.675f}, // R3
    };
    *count = static_cast<int>(sizeof(kHits) / sizeof(kHits[0]));
    return kHits;
}

bool poll_psx_bind_capture(HubModel& hub, const SDL_Event& e) {
    auto& d = hub.psx_settings;
    if (d.configuring_player < 0) return false;
    if (d.capturing_bind < 0 && !d.map_all_active) return false;

    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
        cancel_psx_bind_capture(hub);
        return true;
    }

    const int p = d.configuring_player;
    const auto& s = d.settings;
    const bool is_pad = d.capture_is_pad;

    if (!is_pad) {
        if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat) return true;
        if (d.capturing_bind < 0) return true;
        retcomm::psx_keybinds_set_scancode(hub.paths, p, d.capturing_bind,
                                           static_cast<int>(e.key.scancode));
        d.dirty = true;
        if (d.map_all_active) advance_psx_map_all(hub);
        else cancel_psx_bind_capture(hub);
        return true;
    }

    const std::string& guid = s.player_guid[static_cast<size_t>(p)];
    SDL_JoystickID want = find_live_pad_id(guid.c_str());
    // If GUID lookup failed but exactly one pad is open, accept that device so
    // mapping still works across GUID string quirks.
    if (!want) {
        int live_n = 0;
        SDL_JoystickID only = 0;
        for (int i = 0; i < kMaxOpenHubPads; ++i) {
            if (!g_open_pads[i].handle) continue;
            ++live_n;
            only = g_open_pads[i].id;
        }
        if (live_n == 1) want = only;
    }
    auto from_selected = [&](SDL_JoystickID which) {
        if (!want) return false;
        return which == want;
    };

    auto try_clear_release = [&](SDL_JoystickID which) {
        if (!d.map_all_wait_release) return;
        if (!from_selected(which)) return;
        if (gamepad_at_rest(which)) {
            d.map_all_wait_release = false;
            snapshot_capture_baseline(which);
            reset_capture_debounce();
        }
    };

    auto commit = [&](int kind, int code, int axis_dir) {
        char src[48]{};
        fill_bind_source_string(kind, code, axis_dir, src, sizeof(src));
        if (src[0])
            retcomm::psx_pad_binds_set_source(hub.paths, guid, d.capturing_bind, src);
        else
            retcomm::psx_pad_binds_set(hub.paths, guid, d.capturing_bind, kind, code, axis_dir);
        retcomm::psx_pad_binds_remember(hub.paths, guid, "", -1);
        d.dirty = true;
        reset_capture_debounce();
        if (d.map_all_active) advance_psx_map_all(hub);
        else cancel_psx_bind_capture(hub);
    };

    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        if (d.map_all_wait_release) {
            try_clear_release(e.gbutton.which);
            return true;
        }
        if (!capture_armed()) return true;
        if (d.capturing_bind >= 0 && from_selected(e.gbutton.which)) {
            const int btn = static_cast<int>(e.gbutton.button);
            if (capture_accepts_button(d.capturing_bind, btn)) commit(1, btn, 0);
        }
        return true;
    }
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        try_clear_release(e.gbutton.which);
        return true;
    }
    if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        if (!from_selected(e.gaxis.which)) return true;
        if (d.map_all_wait_release) {
            try_clear_release(e.gaxis.which);
            return true;
        }
        if (!capture_armed()) return true;
        if (d.capturing_bind < 0) return true;
        const int val = static_cast<int>(e.gaxis.value);
        if (val < kCaptureAxisCommit && val > -kCaptureAxisCommit) return true;
        const int axis = static_cast<int>(e.gaxis.axis);
        if (!capture_accepts_axis(d.capturing_bind, axis)) return true;
        commit(2, axis, val > 0 ? 1 : -1);
        return true;
    }
    return true; // swallow while capturing
}

// Poll-based capture fallback: commit after debounce when a filtered candidate
// rises above the baseline (covers missed GAMEPAD_* events).
void tick_psx_bind_capture_poll(HubModel& hub) {
    auto& d = hub.psx_settings;
    if (d.configuring_player < 0) return;
    if (!d.capture_is_pad) return;
    if (d.capturing_bind < 0 && !d.map_all_active) return;

    const int p = d.configuring_player;
    const std::string& guid = d.settings.player_guid[static_cast<size_t>(p)];
    SDL_JoystickID want = find_live_pad_id(guid.c_str());
    if (!want) {
        int live_n = 0;
        SDL_JoystickID only = 0;
        for (int i = 0; i < kMaxOpenHubPads; ++i) {
            if (!g_open_pads[i].handle) continue;
            ++live_n;
            only = g_open_pads[i].id;
        }
        if (live_n == 1) want = only;
    }
    if (!want) return;

    if (d.map_all_wait_release) {
        if (gamepad_at_rest(want)) {
            d.map_all_wait_release = false;
            snapshot_capture_baseline(want);
            reset_capture_debounce();
        }
        return;
    }
    if (d.capturing_bind < 0) return;
    if (!capture_armed()) return;

    SDL_Gamepad* pad = gamepad_handle_for_id(want);
    if (!pad) return;
    if (!g_capture_baseline.valid || g_capture_baseline.id != want)
        snapshot_capture_baseline(want);

    const int slot = d.capturing_bind;

    auto commit = [&](int kind, int code, int axis_dir) {
        char src[48]{};
        fill_bind_source_string(kind, code, axis_dir, src, sizeof(src));
        if (src[0])
            retcomm::psx_pad_binds_set_source(hub.paths, guid, d.capturing_bind, src);
        else
            retcomm::psx_pad_binds_set(hub.paths, guid, d.capturing_bind, kind, code, axis_dir);
        retcomm::psx_pad_binds_remember(hub.paths, guid, "", -1);
        d.dirty = true;
        reset_capture_debounce();
        if (d.map_all_active) advance_psx_map_all(hub);
        else cancel_psx_bind_capture(hub);
    };

    auto note_candidate = [&](int kind, int code, int axis_dir) {
        if (g_capture_baseline.debounce_kind == kind && g_capture_baseline.debounce_code == code &&
            g_capture_baseline.debounce_dir == axis_dir) {
            g_capture_baseline.debounce_frames++;
        } else {
            g_capture_baseline.debounce_kind = kind;
            g_capture_baseline.debounce_code = code;
            g_capture_baseline.debounce_dir = axis_dir;
            g_capture_baseline.debounce_frames = 1;
        }
        if (g_capture_baseline.debounce_frames >= kCapturePollDebounceFrames)
            commit(kind, code, axis_dir);
    };

    // D-pad slots: only the expected cardinal (reject diagonals / Up-as-Left).
    if (slot_is_dpad(slot)) {
        const int expect = expected_dpad_button(slot);
        const int sole = sole_new_dpad_button(pad);
        if (sole >= 0 && sole == expect) note_candidate(1, sole, 0);
        else reset_capture_debounce();
        return;
    }

    int cand_kind = 0, cand_code = -1, cand_dir = 0;
    int cand_n = 0;

    for (int b = 0; b < static_cast<int>(SDL_GAMEPAD_BUTTON_COUNT); ++b) {
        if (!capture_accepts_button(slot, b)) continue;
        const bool now = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b)) != 0;
        if (now && !g_capture_baseline.buttons[b]) {
            ++cand_n;
            cand_kind = 1;
            cand_code = b;
            cand_dir = 0;
        }
    }
    for (int a = 0; a < static_cast<int>(SDL_GAMEPAD_AXIS_COUNT); ++a) {
        if (!capture_accepts_axis(slot, a)) continue;
        const int val = static_cast<int>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a)));
        if (val < kCaptureAxisCommit && val > -kCaptureAxisCommit) continue;
        const int base = static_cast<int>(g_capture_baseline.axes[a]);
        if (std::abs(base) >= kCaptureAxisCommit) continue;
        ++cand_n;
        cand_kind = 2;
        cand_code = a;
        cand_dir = val > 0 ? 1 : -1;
    }

    if (cand_n == 1) note_candidate(cand_kind, cand_code, cand_dir);
    else reset_capture_debounce();
}

void draw_psx_mapping_panel(HubModel& hub, const Theme& th, int p, bool is_pad,
                            const std::string& guid, BoxartCache& boxart) {
    auto& d = hub.psx_settings;
    const bool digital = hub.psx_settings.settings.player_mode[static_cast<size_t>(p)] == 2;
    fs::path art = find_hub_asset_file("controllers", digital ? "pad_digital.png" : "pad_analog.png");
    if (art.empty())
        art = find_hub_asset_file("controllers", digital ? "pad_digital.tga" : "pad_analog.tga");
    const BoxartTexture* tex =
        art.empty() ? nullptr
                    : boxart.get(digital ? "psx:pad_digital" : "psx:pad_analog", art);

    ImGui::BeginChild("psx_map_panel", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(th.text_muted, is_pad ? "GAMEPAD BINDINGS" : "KEYBOARD BINDINGS");
    if (is_pad && !digital) {
        ImGui::TextColored(th.text_muted,
                           "Analog mode: D-pad folds onto the left stick. Chips light when the "
                           "bound control is pressed.");
    } else if (is_pad) {
        ImGui::TextColored(th.text_muted,
                           "Chips light when the bound control is pressed — verify each bind.");
    }
    ImGui::Separator();

    // Footer inside the panel: status only — action buttons live on the modal bar.
    const float status_reserve = (d.capturing_bind >= 0) ? 28.f : 8.f;
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float avail_h = std::max(260.f, ImGui::GetContentRegionAvail().y - status_reserve);

    // pad_*.png is 720x400 with large empty margins around the silhouette. Zoom the
    // UV rect to the controller content so the panel isn't mostly letterbox.
    // Hit coords (psx_pad_hits) are normalized in the full 720x400 canvas.
    constexpr float kCropU0 = 0.105f;
    constexpr float kCropV0 = 0.175f;
    constexpr float kCropU1 = 0.895f;
    constexpr float kCropV1 = 0.865f;
    const float crop_w = kCropU1 - kCropU0;
    const float crop_h = kCropV1 - kCropV0;
    const float full_aspect = (tex && tex->width > 0 && tex->height > 0)
                                  ? (static_cast<float>(tex->width) / static_cast<float>(tex->height))
                                  : (720.f / 400.f);
    const float aspect = full_aspect * (crop_w / std::max(0.01f, crop_h));

    // Fill the panel — thin edge pad, then center any leftover on the short axis.
    constexpr float kEdgePad = 4.f;
    float img_w = std::max(1.f, avail_w - kEdgePad * 2.f);
    float img_h = img_w / aspect;
    if (img_h > avail_h - kEdgePad * 2.f) {
        img_h = std::max(1.f, avail_h - kEdgePad * 2.f);
        img_w = img_h * aspect;
    }
    const float ox = (avail_w - img_w) * 0.5f;
    const float oy = (avail_h - img_h) * 0.5f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float img_x = origin.x + ox;
    const float img_y = origin.y + oy;

    // Reserve the full available area so the child layout matches the painted art.
    ImGui::Dummy(ImVec2(avail_w, avail_h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex && tex->gl_id) {
        dl->AddImage((ImTextureID)(intptr_t)tex->gl_id, ImVec2(img_x, img_y),
                     ImVec2(img_x + img_w, img_y + img_h), ImVec2(kCropU0, kCropV0),
                     ImVec2(kCropU1, kCropV1));
    } else {
        dl->AddRectFilled(ImVec2(img_x, img_y), ImVec2(img_x + img_w, img_y + img_h),
                          ImGui::ColorConvertFloat4ToU32(th.control));
        dl->AddText(ImVec2(img_x + 12.f, img_y + 12.f),
                    ImGui::ColorConvertFloat4ToU32(th.warn),
                    art.empty() ? "pad art missing (assets/controllers)" : "pad art failed to load");
    }

    int hit_n = 0;
    const PsxPadHit* hits = psx_pad_hits(&hit_n);
    // Scale chips with the on-screen pad (cropped reference width ~570px at 720 source).
    const float ui_scale = std::clamp(img_w / 570.f, 0.95f, 2.1f);
    const float chip_w = 44.f * ui_scale;
    const float chip_h = 16.f * ui_scale;

    SDL_Gamepad* live_pad = nullptr;
    int live_dz = 8000;
    if (is_pad && !guid.empty()) {
        const SDL_JoystickID id = find_live_pad_id(guid.c_str());
        live_pad = gamepad_handle_for_id(id);
        live_dz = (retcomm::psx_pad_binds_deadzone(hub.paths, guid) * 32767 + 50) / 100;
        if (live_dz < 1) live_dz = 8000;
    }
    int kb_nkeys = 0;
    const bool* kb_keys = is_pad ? nullptr : SDL_GetKeyboardState(&kb_nkeys);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.f * ui_scale, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f * ui_scale);
    ImGui::SetWindowFontScale(0.78f * ui_scale);
    for (int i = 0; i < hit_n; ++i) {
        const int b = hits[i].button;
        if (digital && b >= 16) continue;
        char bound[48]{};
        if (is_pad)
            retcomm::psx_pad_binds_label(hub.paths, guid, b, bound, sizeof(bound));
        else
            retcomm::psx_keybinds_label(hub.paths, p, b, bound, sizeof(bound));

        bool live_pressed = false;
        if (is_pad && live_pad && bound[0] && std::strcmp(bound, "(unbound)") != 0) {
            live_pressed = gamepad_bound_source_pressed(live_pad, bound, live_dz);
        } else if (!is_pad && kb_keys) {
            const int sc = retcomm::psx_keybinds_get_scancode(hub.paths, p, b);
            if (sc > 0 && sc < kb_nkeys) live_pressed = kb_keys[sc] != 0;
        }

        const float cx = img_x + ((hits[i].nx - kCropU0) / crop_w) * img_w;
        const float cy = img_y + ((hits[i].ny - kCropV0) / crop_h) * img_h;
        ImGui::SetCursorScreenPos(ImVec2(cx - chip_w * 0.5f, cy - chip_h * 0.5f));
        ImGui::PushID(b);
        const bool capturing = d.capturing_bind == b;
        char btn[64];
        if (capturing && d.map_all_wait_release)
            std::snprintf(btn, sizeof(btn), "…");
        else if (capturing)
            std::snprintf(btn, sizeof(btn), "[%s]", psx_pad_chip_label(b));
        else
            std::snprintf(btn, sizeof(btn), "%s", psx_pad_chip_label(b));

        if (capturing)
            ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
        else if (live_pressed)
            ImGui::PushStyleColor(ImGuiCol_Button, th.good_button);
        else
            ImGui::PushStyleColor(ImGuiCol_Button,
                                 ImVec4(th.control.x, th.control.y, th.control.z, 0.88f));
        if (ImGui::Button(btn, ImVec2(chip_w, chip_h))) {
            d.map_all_active = false;
            d.map_all_wait_release = false;
            begin_psx_bind_capture(hub, b, is_pad);
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            char tip[160]{};
            std::snprintf(tip, sizeof(tip), "%s\nBound: %s%s", retcomm::psx_pad_button_label(b),
                          bound[0] ? bound : "(unbound)", live_pressed ? "\n(pressed)" : "");
            if (live_pad) append_live_sdl_dpad_hint(live_pad, tip, sizeof(tip));
            ImGui::SetTooltip("%s", tip);
        }
        ImGui::PopID();
    }
    ImGui::SetWindowFontScale(1.f);
    ImGui::PopStyleVar(2);

    ImGui::SetCursorScreenPos(ImVec2(origin.x, img_y + img_h + 6.f));
    ImGui::Dummy(ImVec2(avail_w, 0));
    if (d.capturing_bind >= 0) {
        if (d.map_all_wait_release)
            ImGui::TextColored(th.warn, "Release controls… (Esc cancels%s)",
                               d.map_all_active ? " — Auto Map" : "");
        else
            ImGui::TextColored(th.warn, "Map an input to %s (Esc cancels%s)",
                               retcomm::psx_pad_button_label(d.capturing_bind),
                               d.map_all_active ? " — Auto Map" : "");
    }
    ImGui::EndChild();
}

void draw_psx_configure_modal(HubModel& hub, const Theme& th, const std::vector<HubGamepadOpt>& pads,
                              BoxartCache& boxart) {
    auto& draft = hub.psx_settings;
    if (draft.configuring_player < 0 ||
        draft.configuring_player >= retcomm::PsxPlatformSettings::kMaxPlayers)
        return;
    const int p = draft.configuring_player;
    auto& s = draft.settings;
    bool dirty = false;

    char title[48];
    std::snprintf(title, sizeof(title), "CONTROLLER - PLAYER %d", p + 1);
    ImGui::OpenPopup("##psx_pad_cfg");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(1100.f, vp->WorkSize.x * 0.96f),
                                    std::min(860.f, vp->WorkSize.y * 0.94f)),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##psx_pad_cfg", nullptr, ImGuiWindowFlags_None)) {
        ImGui::TextColored(th.accent, "%s", title);
        ImGui::SameLine();
        {
            const float close_w = 80.f;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - close_w);
            if (ImGui::Button("Close", ImVec2(close_w, 0))) {
                cancel_psx_bind_capture(hub);
                draft.configuring_player = -1;
                draft.rename_open = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::Separator();
        ImGui::PushID(p);

        const bool is_pad =
            s.player_src[static_cast<size_t>(p)] == 2 && !s.player_guid[static_cast<size_t>(p)].empty();
        const bool is_kb = s.player_src[static_cast<size_t>(p)] == 1;
        if (is_kb) s.player_mode[static_cast<size_t>(p)] = 2;

        // Two-row table keeps labels and controls on the same baselines across columns.
        if (ImGui::BeginTable("psx_cfg_top", 3,
                              ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(th.text_muted, "Input source");
            ImGui::TableNextColumn();
            ImGui::TextColored(th.text_muted, "Pad mode");
            ImGui::TableNextColumn();
            ImGui::TextColored(th.text_muted, "Deadzone");

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            draw_psx_player_source_combo(hub, p, pads, &dirty);

            ImGui::TableNextColumn();
            {
                static const char* kModes[] = {"Analog (DualShock)", "Digital (D-Pad)"};
                int mode_idx = s.player_mode[static_cast<size_t>(p)] == 2 ? 1 : 0;
                if (is_kb) ImGui::BeginDisabled();
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::BeginCombo("##pmode", kModes[mode_idx])) {
                    for (int m = 0; m < 2; ++m) {
                        const bool sel = m == mode_idx;
                        if (ImGui::Selectable(kModes[m], sel) && m != mode_idx) {
                            s.player_mode[static_cast<size_t>(p)] = m == 1 ? 2 : 1;
                            dirty = true;
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (is_kb) {
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Keyboard seats use digital mode.");
                    ImGui::EndDisabled();
                }
            }

            ImGui::TableNextColumn();
            {
                int pct = 0;
                if (is_pad)
                    pct = retcomm::psx_pad_binds_deadzone(hub.paths,
                                                         s.player_guid[static_cast<size_t>(p)]);
                else
                    pct = (s.player_deadzone[static_cast<size_t>(p)] * 100 + 32767 / 2) / 32767;
                pct = std::clamp(pct, 0, 100);
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::SliderInt("##dz", &pct, 0, 50, "%d%%")) {
                    s.player_deadzone[static_cast<size_t>(p)] =
                        std::clamp((pct * 32767 + 50) / 100, 0, 32767);
                    if (is_pad)
                        retcomm::psx_pad_binds_set_deadzone(
                            hub.paths, s.player_guid[static_cast<size_t>(p)], pct);
                    dirty = true;
                }
            }
            ImGui::EndTable();
        }
        ImGui::Dummy(ImVec2(0, 6));

        if (draft.rename_open) ImGui::OpenPopup("Rename Profile");
        if (ImGui::BeginPopupModal("Rename Profile", &draft.rename_open,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Display name for this gamepad profile:");
            ImGui::SetNextItemWidth(320.f);
            const bool enter = hub_input_text("##rename_pad", draft.rename_buf,
                                              sizeof(draft.rename_buf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                draft.rename_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            const bool ok = draft.rename_buf[0] != '\0';
            ImGui::BeginDisabled(!ok);
            if ((ImGui::Button("OK", ImVec2(120, 0)) || enter) && ok) {
                retcomm::psx_pad_binds_rename(hub.paths, s.player_guid[static_cast<size_t>(p)],
                                             draft.rename_buf);
                draft.rename_open = false;
                dirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        // Bindings fill remaining height above the bottom action bar.
        const float footer_h = ImGui::GetFrameHeightWithSpacing() + 8.f;
        const float body_h = std::max(200.f, ImGui::GetContentRegionAvail().y - footer_h);
        ImGui::BeginChild("psx_cfg_body", ImVec2(0, body_h), ImGuiChildFlags_None);
        if (s.player_src[static_cast<size_t>(p)] == 0) {
            ImGui::BeginChild("psx_map_empty", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::TextColored(th.text_muted, "Assign an input source to edit mappings.");
            ImGui::EndChild();
        } else {
            draw_psx_mapping_panel(hub, th, p, is_pad, s.player_guid[static_cast<size_t>(p)],
                                   boxart);
        }
        ImGui::EndChild();

        // Bottom bar: profile + map actions + Save (saves platform prefs and closes).
        // Shared width so Rename/Delete/Auto Map/Reset read as one even strip.
        const char* kFooterBtns[] = {"Rename", "Delete", "Auto Map", "Reset"};
        float footer_btn_w = 0.f;
        for (const char* lab : kFooterBtns)
            footer_btn_w = std::max(footer_btn_w, ImGui::CalcTextSize(lab).x);
        footer_btn_w += ImGui::GetStyle().FramePadding.x * 2.f + 16.f;
        const ImVec2 footer_btn_sz(footer_btn_w, 0);

        if (!is_pad) ImGui::BeginDisabled();
        if (ImGui::Button("Rename", footer_btn_sz)) {
            char nm[64]{};
            retcomm::psx_pad_binds_name(hub.paths, s.player_guid[static_cast<size_t>(p)], nm,
                                        sizeof(nm));
            if (!nm[0]) {
                for (const auto& pad : pads)
                    if (pad.guid == s.player_guid[static_cast<size_t>(p)]) {
                        std::snprintf(nm, sizeof(nm), "%s", pad.name);
                        break;
                    }
            }
            std::snprintf(draft.rename_buf, sizeof(draft.rename_buf), "%s", nm);
            draft.rename_open = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete", footer_btn_sz)) {
            const std::string g = s.player_guid[static_cast<size_t>(p)];
            retcomm::psx_pad_binds_delete(hub.paths, g);
            for (int o = 0; o < retcomm::PsxPlatformSettings::kMaxPlayers; ++o) {
                if (s.player_src[static_cast<size_t>(o)] == 2 &&
                    s.player_guid[static_cast<size_t>(o)] == g)
                    assign_psx_player_source(hub, o, 1, nullptr, nullptr, &dirty);
            }
            cancel_psx_bind_capture(hub);
        }
        if (!is_pad) ImGui::EndDisabled();

        ImGui::SameLine();
        if (accent_button("Auto Map", th, footer_btn_sz)) {
            if (s.player_src[static_cast<size_t>(p)] != 0) begin_psx_map_all(hub, is_pad);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", footer_btn_sz)) {
            if (is_pad && !s.player_guid[static_cast<size_t>(p)].empty())
                retcomm::psx_pad_binds_reset(hub.paths, s.player_guid[static_cast<size_t>(p)]);
            else if (is_kb)
                retcomm::psx_keybinds_reset_player(hub.paths, p);
            cancel_psx_bind_capture(hub);
            dirty = true;
        }

        {
            const float save_w = 120.f;
            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 8.f,
                                          ImGui::GetWindowContentRegionMax().x - save_w));
            if (accent_button("Save", th, ImVec2(save_w, 0))) {
                if (dirty) draft.dirty = true;
                // Persist profile name/deadzone/maps already written live; also
                // flush settings.toml player seats + close the modal.
                if (is_pad && !s.player_guid[static_cast<size_t>(p)].empty()) {
                    char nm[64]{};
                    retcomm::psx_pad_binds_name(hub.paths, s.player_guid[static_cast<size_t>(p)], nm,
                                                sizeof(nm));
                    const int dz = retcomm::psx_pad_binds_deadzone(
                        hub.paths, s.player_guid[static_cast<size_t>(p)]);
                    retcomm::psx_pad_binds_save_profile(
                        hub.paths, s.player_guid[static_cast<size_t>(p)], nm[0] ? nm : "Controller",
                        retcomm::psx_pad_binds_name_is_custom(hub.paths,
                                                              s.player_guid[static_cast<size_t>(p)]),
                        dz);
                }
                std::string err;
                if (!hub.save_psx_settings(&err)) {
                    hub.append_log("PlayStation settings save failed: " + err);
                } else {
                    hub.show_toast("Saved!");
                }
                cancel_psx_bind_capture(hub);
                draft.configuring_player = -1;
                draft.rename_open = false;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::PopID();
        ImGui::EndPopup();
    }
    if (dirty) draft.dirty = true;
}

void draw_psx_gamepads_panel(HubModel& hub, const Theme& th, float panel_h, BoxartCache& boxart) {
    auto& s = hub.psx_settings.settings;
    bool dirty = false;
    std::vector<HubGamepadOpt> pads;
    collect_hub_gamepads(hub, pads);

    ImGui::BeginChild("psx_gamepads", ImVec2(0, panel_h), ImGuiChildFlags_Borders);
    ImGui::TextColored(th.text_muted, "CONTROLLERS");
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Slots 1–8 for titles that support multitap / high player counts. Assignments "
        "write pN_device / pN_mode / pN_deadzone into the global PlayStation settings.toml "
        "and are applied to games on install, update, and launch.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 8));

    constexpr int kN = retcomm::PsxPlatformSettings::kMaxPlayers;
    const float gap = th.spacing_md;
    const float availw = ImGui::GetContentRegionAvail().x;
    const float pref = 280.f;
    int cols = static_cast<int>((availw + gap) / (pref + gap));
    if (cols < 1) cols = 1;
    if (cols > 4) cols = 4;
    float cardw = (availw - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    if (cardw < 1.f) cardw = availw;

    for (int p = 0; p < kN; ++p) {
        if (p % cols) ImGui::SameLine(0, gap);
        else if (p) ImGui::Dummy(ImVec2(0, gap));

        ImGui::PushID(p);
        ImGui::BeginChild("pcard", ImVec2(cardw, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        {
            char eb[24];
            std::snprintf(eb, sizeof(eb), "PLAYER %d", p + 1);
            ImGui::TextColored(th.text_muted, "%s", eb);
            ImGui::Dummy(ImVec2(0, 4));
            draw_psx_player_source_combo(hub, p, pads, &dirty);
            ImGui::Dummy(ImVec2(0, 4));
            const float cw = ImGui::GetContentRegionAvail().x;
            const float half = (cw - th.spacing_sm) * 0.5f;
            const float btnh = 32.f;
            if (ImGui::Button("Configure", ImVec2(half, btnh))) {
                cancel_psx_bind_capture(hub);
                hub.psx_settings.configuring_player = p;
                if (s.player_src[static_cast<size_t>(p)] == 1)
                    s.player_mode[static_cast<size_t>(p)] = 2;
                else if (s.player_src[static_cast<size_t>(p)] == 2 &&
                         !s.player_guid[static_cast<size_t>(p)].empty()) {
                    const char* nm = "Controller";
                    for (const auto& pad : pads)
                        if (pad.guid == s.player_guid[static_cast<size_t>(p)]) {
                            nm = pad.name;
                            break;
                        }
                    retcomm::psx_pad_binds_remember(hub.paths, s.player_guid[static_cast<size_t>(p)],
                                                   nm, -1);
                }
            }
            ImGui::SameLine(0, th.spacing_sm);
            const bool on = s.player_src[static_cast<size_t>(p)] != 0;
            const char* st = on ? "connected" : "not assigned";
            const float sw = 10.f + 8.f + ImGui::CalcTextSize(st).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (half - sw) * 0.5f));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                                 (btnh - ImGui::GetTextLineHeight()) * 0.5f);
            const ImVec2 dot_c = ImGui::GetCursorScreenPos();
            const float r = 4.f;
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(dot_c.x + r, dot_c.y + ImGui::GetTextLineHeight() * 0.5f), r,
                ImGui::ColorConvertFloat4ToU32(on ? th.good : th.text_muted));
            ImGui::Dummy(ImVec2(r * 2.f + 6.f, 0));
            ImGui::SameLine(0, 0);
            ImGui::TextColored(on ? th.good : th.text_muted, "%s", st);
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (dirty) hub.psx_settings.dirty = true;
    draw_psx_configure_modal(hub, th, pads, boxart);
}

void draw_psx_settings_panel(HubModel& hub, const Theme& th, BoxartCache& boxart) {
    poll_psx_hotkey_capture(hub);
    poll_psx_pad_hotkey_capture(hub);
    auto& s = hub.psx_settings.settings;
    auto mark = [&] { hub.psx_settings.dirty = true; };
    const bool gamepads = hub.psx_settings.gamepads_tab;

    ImGui::BeginChild("psx_settings", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("PLAYSTATION SETTINGS");
    ImGui::PopStyleColor();

    constexpr float kTabW = 110.f;
    const float right = ImGui::GetWindowContentRegionMax().x;
    const float wrap_x = ImGui::GetCursorPosX() + (right - ImGui::GetCursorPosX()) - kTabW - 12.f;
    const float desc_y = ImGui::GetCursorPosY();
    ImGui::PushTextWrapPos(wrap_x);
    ImGui::TextWrapped(
        "%s",
        gamepads
            ? "Global controller slots for PlayStation titles. Saved prefs are written into "
              "each game's settings.toml on install, update, and launch (unless excluded in "
              "Manage Game Data)."
            : "Global Display, Audio, Input, and Hotkeys for PlayStation titles. Saved prefs "
              "are written into each game's settings.toml / config.ini on install, update, and "
              "launch (unless excluded in Manage Game Data).");
    ImGui::PopTextWrapPos();
    ImGui::SameLine();
    ImGui::SetCursorPosY(desc_y);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - kTabW));
    if (gamepads) {
        if (good_button("System", th, ImVec2(kTabW, 0))) {
            hub.psx_settings.gamepads_tab = false;
            cancel_psx_bind_capture(hub);
            hub.psx_settings.configuring_player = -1;
        }
    } else {
        if (good_button("Gamepads", th, ImVec2(kTabW, 0)))
            hub.psx_settings.gamepads_tab = true;
    }
    ImGui::Separator();

    // Reserve gap + Save/Cancel row so panes don't crowd the footer.
    const float footer_h = ImGui::GetFrameHeight() + 24.f;
    const float gap = 12.f;
    const float avail_x = ImGui::GetContentRegionAvail().x;
    const float col_w = std::max(240.f, (avail_x - gap) * 0.5f);
    const float panel_h = std::max(120.f, ImGui::GetContentRegionAvail().y - footer_h);

    if (gamepads) {
        draw_psx_gamepads_panel(hub, th, panel_h, boxart);
    } else {
    ImGui::BeginChild("psx_display_audio", ImVec2(col_w, panel_h), ImGuiChildFlags_Borders,
                      page_wheel_flags(hub));
    ImGui::TextColored(th.text_muted, "DISPLAY");
    ImGui::Separator();
    {
        static const char* kWidthLabels[] = {"960 px", "1280 px", "1600 px", "1920 px"};
        static const int kWidths[] = {960, 1280, 1600, 1920};
        if (settings_combo_values("Window size", "##ww", th, kWidthLabels, kWidths, 4,
                                  &s.window_width))
            mark();

        static const char* kRenderer[] = {"Software", "OpenGL", "Vulkan"};
        if (settings_combo("Renderer", "##ren", th, kRenderer, 3, &s.renderer)) mark();

        static const char* kSsLabels[] = {"1x", "2x", "3x", "4x"};
        static const int kSs[] = {1, 2, 3, 4};
        if (settings_combo_values("Supersampling", "##ss", th, kSsLabels, kSs, 4,
                                  &s.supersampling))
            mark();

        static const char* kFs[] = {"Off", "Borderless", "Exclusive"};
        if (settings_combo("Fullscreen", "##fs", th, kFs, 3, &s.fullscreen)) mark();

        static const char* kView[] = {"4:3 (Native)", "16:9 (Widescreen)", "21:9 (Ultrawide)",
                                      "Adaptive"};
        if (settings_combo("View mode", "##view", th, kView, 4, &s.view_mode)) mark();
        if (s.view_mode != 0) {
            // Its own line: the control column owns the right edge now, so a
            // note cannot ride along beside the dropdown.
            ImGui::TextColored(th.warn, "\xe2\x9a\xa0\xef\xb8\x8f Experimental \xe2\x9a\xa0\xef\xb8\x8f");
        }

        if (settings_combo_bool("Texture filtering", "##tf", th, "Nearest", "Bilinear",
                                &s.texture_filter_bilinear))
            mark();

        if (settings_checkbox("Antialiasing", "##aa", th, &s.antialiasing)) mark();

        // How a decoded FMV is scaled up. Separate from Texture filtering
        // above; with antialiasing off the runtime presents video with hard
        // pixels regardless, so the row previews Nearest and is disabled —
        // what it would do, not what is stored.
        {
            static const char* kFmv[] = {"Nearest", "Bilinear", "Sharp", "Bicubic"};
            if (s.antialiasing) {
                if (settings_combo("FMV filtering", "##fmv", th, kFmv, 4, &s.fmv_filter)) mark();
            } else {
                ImGui::BeginDisabled();
                int shown = 0;
                settings_combo("FMV filtering", "##fmv", th, kFmv, 4, &shown);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(
                        "Antialiasing is off, so video is presented with hard pixels "
                        "regardless.");
                }
            }
        }

        if (settings_checkbox("Perspective textures", "##persp", th, &s.perspective_texturing))
            mark();

        static const char* kScreen[] = {"Raw", "CRT", "Composite", "Trinitron"};
        if (settings_combo("Screen model", "##scr", th, kScreen, 4, &s.screen_kind)) mark();

        if (settings_checkbox("Scanlines", "##sl", th, &s.scanlines)) mark();
        if (s.scanlines) {
            int pct = std::clamp(static_cast<int>(s.scanline_strength * 100.f + 0.5f), 0, 100);
            settings_row("Scanline strength", th, kSettingsCtrlW);
            if (ImGui::SliderInt("##slstr", &pct, 0, 100, "%d%%")) {
                s.scanline_strength = static_cast<float>(pct) / 100.f;
                mark();
            }
        }

        if (settings_checkbox("Geometry correction", "##geo", th, &s.geometry_correction)) mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "[video] geometry_correction \xe2\x80\x94 correct the PS1's integer vertex "
                "snapping, the source of the wobble on flat surfaces.");
        }

        if (s.renderer != 0) {
            if (settings_checkbox("Frame interpolation", "##fi", th, &s.frame_interpolation))
                mark();
            if (s.frame_interpolation) {
                static const char* kFpsLabels[] = {"Display", "120 fps", "144 fps", "165 fps",
                                                  "240 fps"};
                static const int kFps[] = {0, 120, 144, 165, 240};
                if (settings_combo_values("Presentation target", "##fifps", th, kFpsLabels, kFps,
                                          5, &s.frame_interpolation_fps))
                    mark();
            }
        }

        {
            static const char* kVsyncLabels[] = {"On", "Off", "Adaptive"};
            static const int kVsync[] = {1, 0, -1};
            if (settings_combo_values("VSync", "##vs", th, kVsyncLabels, kVsync, 3, &s.vsync))
                mark();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "On: the swap waits for the panel \xe2\x80\x94 no tearing.\n"
                    "Off: swap immediately \xe2\x80\x94 lowest display "
                    "latency, may tear.\n"
                    "Adaptive: vsync while the game keeps up, immediate when it "
                    "drops below the refresh rate.\n\n"
                    "The runtime still paces frames to the console's own rate "
                    "either way, so Off does not run the game fast.");
            }
        }

        // What the machine does, as opposed to how it is drawn. Rewind's
        // depth/interval live here with the switch that gates them.
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        ImGui::TextColored(th.text_muted, "EMULATION");
        if (settings_checkbox("Fast boot", "##fboot", th, &s.fast_boot)) mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("[video] fast_boot \xe2\x80\x94 skip the BIOS boot animation.");

        if (settings_checkbox("Skip FMVs", "##skipfmv", th, &s.auto_skip_fmv)) mark();

        if (settings_checkbox("Low-latency input", "##lli", th, &s.low_latency_input)) mark();

        // Rewind's master switch. psxrecomp defaults [video] rewind to false,
        // so the depth/interval rows below only mean anything once this is on.
        if (settings_checkbox("Rewind", "##rewind", th, &s.rewind_enabled)) mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "Keep a ring of whole-machine snapshots so the Rewind hotkey can step "
                "back through them.\n"
                "Costs memory while a game runs (depth x ~3.5 MB). Applied when a title "
                "launches.");
        }

        ImGui::BeginDisabled(!s.rewind_enabled);
        {
            static const char* kDepthLabels[] = {"50", "100", "150", "200"};
            static const int kDepth[] = {50, 100, 150, 200};
            if (settings_combo_values("Rewind buffer", "##rwbuf", th, kDepthLabels, kDepth, 4,
                                      &s.rewind_depth))
                mark();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "How many local rewind snapshots to keep (50 / 100 / 150 / 200).\n"
                    "Applied when a title launches.");
            }

            static const char* kIvLabels[] = {"1", "4", "8", "12", "15"};
            static const int kIv[] = {1, 4, 8, 12, 15};
            if (settings_combo_values("Rewind interval", "##rwint", th, kIvLabels, kIv, 5,
                                      &s.rewind_interval))
                mark();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "Frames between rewind snapshots (1 / 4 / 8 / 12 / 15).\n"
                    "FMV still densifies toward 4 when this is sparser.\n"
                    "Applied when a title launches.");
            }
        }
        ImGui::EndDisabled();

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        ImGui::TextColored(th.text_muted, "AUDIO");
        if (settings_checkbox("High-quality SPU", "##spuhq", th, &s.spu_hq)) mark();

        // Output sample rate ([audio] frequency). Same four recomp-ui offers;
        // 32040 Hz is the SPU's own rate, so it resamples least.
        {
            static const char* kFreqLabels[] = {"32040 Hz", "32000 Hz", "44100 Hz", "48000 Hz"};
            static const int kFreq[] = {32040, 32000, 44100, 48000};
            if (settings_combo_values("Sample rate", "##freq", th, kFreqLabels, kFreq, 4,
                                      &s.audio_freq))
                mark();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(0.f, gap);
    ImGui::BeginChild("psx_input_hotkeys", ImVec2(0, panel_h), ImGuiChildFlags_Borders,
                      page_wheel_flags(hub));
    ImGui::TextColored(th.text_muted, "INPUT & HOTKEYS");
    ImGui::Separator();
    {
        if (settings_checkbox("Multitap", "##multitap", th, &s.multitap_enabled)) mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "Enable SCPH-1070 multitap for 3+ player seats. Off limits local play "
                "to two native controller ports.");
        }
        if (settings_checkbox("Multitap analog (hack)", "##multitap_analog", th,
                              &s.multitap_analog))
            mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "Allow DualShock sticks on multitap tap seats (not faithful). Not applied "
                "to titles with digital pad locked in game.toml.");
        }

        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Separator();
        ImGui::TextColored(th.text_muted, "CONTROLLER SHORTCUTS");
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Click a binding, then hold the buttons together and release "
            "(Esc cancels). Lets a player reach these from the couch.");
        ImGui::PopStyleColor();
        {
            static const char* kPadHkLabel[4] = {"Rewind", "Save-state menu", "Fast-forward",
                                                "Fast-forward (toggle)"};
            for (int i = 0; i < 4; ++i) {
                ImGui::PushID(1000 + i);
                const bool cap = hub.psx_settings.capturing_pad_hotkey == i;
                const int cur = (i == 0)   ? s.hotkey_pad_rewind
                                : (i == 1) ? s.hotkey_pad_save_state_menu
                                : (i == 2) ? s.hotkey_pad_fast_forward
                                           : s.hotkey_pad_fast_forward_toggle;
                const std::string lbl =
                    cap ? "[ hold buttons... ]"
                        : retcomm::PsxPlatformSettings::pad_bind_label(cur);
                if (settings_bind_button(kPadHkLabel[i], th, lbl.c_str(), cap)) {
                    hub.psx_settings.capturing_pad_hotkey = i;
                    hub.psx_settings.pad_hotkey_mask = 0;
                }
                ImGui::PopID();
            }
        }

        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Separator();
        ImGui::TextColored(th.text_muted, "HOTKEYS");
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped("Click a binding, then press a key (Esc cancels).");
        ImGui::PopStyleColor();

        for (int i = 0; i < retcomm::PsxPlatformSettings::kHotkeyCount; ++i) {
            ImGui::PushID(i);
            const bool cap = hub.psx_settings.capturing_hotkey == i;
            const std::string& cur = s.hotkeys[static_cast<size_t>(i)];
            const char* bl = cap ? "[ press... ]"
                                 : (cur.empty()
                                        ? retcomm::PsxPlatformSettings::hotkey_default(i)
                                        : cur.c_str());
            if (settings_bind_button(retcomm::PsxPlatformSettings::hotkey_label(i), th, bl, cap))
                hub.psx_settings.capturing_hotkey = i;
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    } // system tab

    ImGui::Dummy(ImVec2(0, 12));
    const float footer_y = ImGui::GetCursorPosY();
    constexpr float kResetW = 150.f;
    if (accent_button("Save", th, ImVec2(160, 0))) {
        std::string err;
        if (!hub.save_psx_settings(&err)) {
            hub.append_log("PlayStation settings save failed: " + err);
            hub.set_status("Save failed");
        } else {
            hub.show_toast("Saved!");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        hub.show_psx_settings = false;
        hub.psx_settings.dirty = false;
        hub.psx_settings.capturing_hotkey = -1;
        cancel_psx_bind_capture(hub);
        hub.psx_settings.configuring_player = -1;
        hub.psx_settings.gamepads_tab = false;
    }
    if (hub.psx_settings.dirty) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(th.warn, "unsaved changes");
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(th.text_muted, "%s",
                       retcomm::psx_platform_settings_dir(hub.paths).string().c_str());
    // System tab only: restore Display / Audio / Multitap / Hotkeys defaults.
    if (!gamepads) {
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - kResetW, footer_y));
        if (ImGui::Button("Reset to Default", ImVec2(kResetW, 0))) {
            hub.psx_settings.capturing_hotkey = -1;
            s.reset_system_to_defaults();
            mark();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "Restore Display, Audio, Multitap, and Hotkeys to defaults.\n"
                "Gamepad seat assignments are left unchanged.");
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Super Nintendo Configure page (global snesrecomp prefs). Mirrors the
// PlayStation page above, but the runner keeps everything in config.ini +
// keybinds.ini and has no per-device gamepad identity yet, so seats, gamepad
// button maps, and keyboard binds are all per player on one page.
// ---------------------------------------------------------------------------

void poll_snes_hotkey_capture(HubModel& hub) {
    auto& d = hub.snes_settings;
    if (d.capturing_hotkey < 0 ||
        d.capturing_hotkey >= retcomm::SnesPlatformSettings::kHotkeyCount)
        return;
    std::string name;
    const int r = poll_hotkey_capture_key(name);
    if (r < 0) {
        d.capturing_hotkey = -1;
        return;
    }
    if (r > 0) {
        d.settings.hotkeys[static_cast<size_t>(d.capturing_hotkey)] = name;
        d.dirty = true;
        d.capturing_hotkey = -1;
    }
}

void cancel_snes_bind_capture(HubModel& hub) {
    hub.snes_settings.capturing_player = -1;
    hub.snes_settings.capturing_bind = -1;
}

// Keyboard bind capture for the SNES page: one SDL scancode per button.
// Returns true when the event was consumed by the capture.
bool poll_snes_bind_capture(HubModel& hub, const SDL_Event& e) {
    auto& d = hub.snes_settings;
    if (!hub.show_snes_settings || d.capturing_player < 0 || d.capturing_bind < 0) return false;
    if (e.type != SDL_EVENT_KEY_DOWN) return false;
    if (e.key.repeat) return true;
    if (e.key.key == SDLK_ESCAPE) {
        cancel_snes_bind_capture(hub);
        return true;
    }
    const auto p = static_cast<size_t>(d.capturing_player);
    const auto b = static_cast<size_t>(d.capturing_bind);
    if (p < retcomm::SnesPlatformSettings::kMaxPlayers &&
        b < retcomm::SnesPlatformSettings::kButtonCount) {
        d.settings.kb_scancode[p][b] = static_cast<int>(e.key.scancode);
        d.dirty = true;
    }
    cancel_snes_bind_capture(hub);
    return true;
}

// Live pads plus any GUID a seat still points at (shown as disconnected).
void collect_snes_gamepads(HubModel& hub, std::vector<HubGamepadOpt>& out) {
    out.clear();
    auto push = [&](const char* guid, const char* name, bool live, SDL_JoystickID id) {
        if (!guid || !guid[0]) return;
        for (const auto& o : out)
            if (guid_eq_ci(o.guid, guid)) return;
        HubGamepadOpt o;
        std::snprintf(o.guid, sizeof(o.guid), "%s", guid);
        const char* nm = (name && name[0] && std::strcmp(name, "Gamepad") != 0) ? name : "Controller";
        std::snprintf(o.name, sizeof(o.name), "%s", nm);
        o.live = live;
        o.id = id;
        out.push_back(o);
    };
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
        for (int i = 0; i < n; ++i) {
            char guid_str[64]{};
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), guid_str,
                             static_cast<int>(sizeof(guid_str)));
            push(guid_str, SDL_GetGamepadNameForID(ids[i]), true, ids[i]);
        }
        SDL_free(ids);
    }
    const auto& s = hub.snes_settings.settings;
    for (int p = 0; p < retcomm::SnesPlatformSettings::kMaxPlayers; ++p) {
        if (s.player_src[static_cast<size_t>(p)] != 2) continue;
        push(s.player_guid[static_cast<size_t>(p)].c_str(), "Controller", false, 0);
    }
}

void draw_snes_player_source_combo(HubModel& hub, int p, const std::vector<HubGamepadOpt>& pads) {
    auto& s = hub.snes_settings.settings;
    const auto pi = static_cast<size_t>(p);
    const int src = std::clamp(s.player_src[pi], 0, 2);
    const char* preview = src == 0 ? "None" : (src == 1 ? "Keyboard" : "Gamepad");
    if (src == 2) {
        for (const auto& pad : pads)
            if (s.player_guid[pi] == pad.guid) preview = pad.name;
    }
    ImGui::SetNextItemWidth(-1.f);
    if (!ImGui::BeginCombo("##src", preview)) return;
    auto assign = [&](int new_src, const char* guid) {
        s.player_src[pi] = new_src;
        s.player_guid[pi] = (new_src == 2 && guid) ? guid : "";
        hub.snes_settings.dirty = true;
    };
    if (ImGui::Selectable("None", src == 0)) assign(0, nullptr);
    if (ImGui::Selectable("Keyboard", src == 1)) assign(1, nullptr);
    if (pads.empty()) {
        ImGui::BeginDisabled();
        ImGui::Selectable("(no gamepad connected)");
        ImGui::EndDisabled();
    }
    for (const auto& pad : pads) {
        bool claimed = false;
        for (int o = 0; o < retcomm::SnesPlatformSettings::kMaxPlayers; ++o) {
            if (o == p) continue;
            if (s.player_src[static_cast<size_t>(o)] == 2 &&
                s.player_guid[static_cast<size_t>(o)] == pad.guid)
                claimed = true;
        }
        char label[96];
        std::snprintf(label, sizeof(label), pad.live ? "%s" : "%s (disconnected)", pad.name);
        const bool sel = src == 2 && s.player_guid[pi] == pad.guid;
        if (claimed) ImGui::BeginDisabled();
        if (ImGui::Selectable(label, sel) && !claimed) assign(2, pad.guid);
        if (claimed) ImGui::EndDisabled();
    }
    ImGui::EndCombo();
}

// The renderer vocabulary snesrecomp offers, built by its own rules
// (snesrecomp runner/src/desktop/host_main.c, RendererEnumerate +
// RendererPretty): "auto" first, then the framework's native GL presenter,
// then every SDL render driver this build actually has — minus SDL's own
// opengl/opengles drivers, which are the native presenter's twins and would
// otherwise list OpenGL twice.
//
// Enumerated from the launcher's own SDL, which is the same SDL family the
// game links, so on this host the list is what the game will really find.
// A build that lacks a listed driver falls back to Auto with a warning on
// stderr rather than failing, so the setting is safe to carry between hosts.
struct SnesRendererOpt {
    std::string id;
    std::string label;
};

const std::vector<SnesRendererOpt>& snes_renderer_options() {
    static const std::vector<SnesRendererOpt> list = [] {
        auto pretty = [](const std::string& id) -> std::string {
            if (id == "direct3d") return "Direct3D 9";
            if (id == "direct3d11") return "Direct3D 11";
            if (id == "direct3d12") return "Direct3D 12";
            if (id == "vulkan") return "Vulkan";
            if (id == "metal") return "Metal";
            if (id == "gpu") return "SDL3_GPU";
            if (id == "software") return "Software";
            return id;
        };
        std::vector<SnesRendererOpt> v;
        v.push_back({"auto", "Auto"});
        v.push_back({"opengl", "OpenGL"});
        const int n = SDL_GetNumRenderDrivers();
        for (int i = 0; i < n; ++i) {
            const char* id = SDL_GetRenderDriver(i);
            if (!id || !id[0]) continue;
            const std::string sid = id;
            if (sid == "opengl" || sid == "opengles2" || sid == "opengles") continue;
            v.push_back({sid, pretty(sid)});
        }
        return v;
    }();
    return list;
}

// Empty Renderer follows the older OutputMethod key, the same fallback
// snesrecomp's RendererChoice() applies.
int snes_renderer_index(const retcomm::SnesPlatformSettings& s) {
    std::string want = s.renderer;
    if (want.empty()) {
        want = s.output_method == 2 ? "opengl" : (s.output_method == 1 ? "software" : "auto");
    }
    const auto& opts = snes_renderer_options();
    for (size_t i = 0; i < opts.size(); ++i)
        if (opts[i].id == want) return static_cast<int>(i);
    return 0;  // Auto
}

// Where each SNES button sits on assets/controllers/pad_snes.png, normalised
// in that art's full 720x400 canvas (the same convention psx_pad_hits uses, so
// the crop maths below is shared). Index order is the runner's Controls= order:
// Up Down Left Right Select Start A B X Y L R.
struct SnesPadHit {
    int button;
    float nx;
    float ny;
};

const SnesPadHit* snes_pad_hits(int* count) {
    static const SnesPadHit kHits[] = {
        {0, 0.296f, 0.438f},   // Up
        {1, 0.296f, 0.637f},   // Down
        {2, 0.240f, 0.538f},   // Left
        {3, 0.352f, 0.538f},   // Right
        {4, 0.458f, 0.540f},   // Select
        {5, 0.542f, 0.540f},   // Start
        {6, 0.757f, 0.538f},   // A   (blue, right)
        {7, 0.704f, 0.632f},   // B   (yellow, bottom)
        {8, 0.704f, 0.443f},   // X   (green, top)
        {9, 0.651f, 0.538f},   // Y   (red, left)
        {10, 0.275f, 0.268f},  // L   (left shoulder tab)
        {11, 0.725f, 0.268f},  // R   (right shoulder tab)
    };
    *count = static_cast<int>(sizeof(kHits) / sizeof(kHits[0]));
    return kHits;
}

// The pad silhouette sits inside large empty margins; zoom the UV rect to the
// controller so the panel is not mostly letterbox. Same treatment, and the same
// reason, as draw_psx_mapping_panel.
constexpr float kSnesCropU0 = 0.105f;
constexpr float kSnesCropV0 = 0.150f;
constexpr float kSnesCropU1 = 0.895f;
constexpr float kSnesCropV1 = 0.850f;

// The pad picture with a chip on every button. Clicking a chip starts a capture
// for that button, exactly as the list rows did — a seat's map is a physical
// thing, so it reads better on the shape of the pad than as twelve rows.
void draw_snes_pad_map(HubModel& hub, const Theme& th, int player, bool is_pad,
                       BoxartCache& boxart) {
    using retcomm::SnesPlatformSettings;
    auto& d = hub.snes_settings;
    auto& s = d.settings;
    const size_t pi = static_cast<size_t>(player);

    fs::path art = find_hub_asset_file("controllers", "pad_snes.png");
    const BoxartTexture* tex = art.empty() ? nullptr : boxart.get("snes:pad", art);

    ImGui::BeginChild("snes_map_panel", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(th.text_muted, is_pad ? "GAMEPAD BINDINGS" : "KEYBOARD BINDINGS");
    ImGui::TextColored(th.text_muted,
                       is_pad ? "Click a button to pick which gamepad control drives it."
                              : "Click a button, then press the key to bind (Esc cancels). "
                                "Chips light while a bound key is held.");
    ImGui::Separator();

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float avail_h = std::max(140.f, ImGui::GetContentRegionAvail().y - 4.f);
    const float crop_w = kSnesCropU1 - kSnesCropU0;
    const float crop_h = kSnesCropV1 - kSnesCropV0;
    const float full_aspect = (tex && tex->width > 0 && tex->height > 0)
                                  ? (static_cast<float>(tex->width) /
                                     static_cast<float>(tex->height))
                                  : (720.f / 400.f);
    const float aspect = full_aspect * (crop_w / std::max(0.01f, crop_h));

    constexpr float kEdgePad = 4.f;
    float img_w = std::max(1.f, avail_w - kEdgePad * 2.f);
    float img_h = img_w / aspect;
    if (img_h > avail_h - kEdgePad * 2.f) {
        img_h = std::max(1.f, avail_h - kEdgePad * 2.f);
        img_w = img_h * aspect;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float img_x = origin.x + (avail_w - img_w) * 0.5f;
    const float img_y = origin.y + (avail_h - img_h) * 0.5f;

    ImGui::Dummy(ImVec2(avail_w, avail_h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex && tex->gl_id) {
        dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(tex->gl_id)),
                     ImVec2(img_x, img_y), ImVec2(img_x + img_w, img_y + img_h),
                     ImVec2(kSnesCropU0, kSnesCropV0), ImVec2(kSnesCropU1, kSnesCropV1));
    } else {
        dl->AddRectFilled(ImVec2(img_x, img_y), ImVec2(img_x + img_w, img_y + img_h),
                          ImGui::ColorConvertFloat4ToU32(th.control));
        dl->AddText(ImVec2(img_x + 12.f, img_y + 12.f),
                    ImGui::ColorConvertFloat4ToU32(th.warn),
                    art.empty() ? "pad art missing (assets/controllers/pad_snes.png)"
                                : "pad art failed to load");
    }

    int hit_n = 0;
    const SnesPadHit* hits = snes_pad_hits(&hit_n);
    const float ui_scale = std::clamp(img_w / 570.f, 0.95f, 2.1f);
    const float chip_w = 60.f * ui_scale;
    const float chip_h = 18.f * ui_scale;

    int kb_nkeys = 0;
    const bool* kb_keys = is_pad ? nullptr : SDL_GetKeyboardState(&kb_nkeys);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.f * ui_scale, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f * ui_scale);
    ImGui::SetWindowFontScale(0.74f * ui_scale);
    for (int i = 0; i < hit_n; ++i) {
        const int b = hits[i].button;
        std::string bound;
        if (is_pad) {
            const std::string& tok = s.pad_controls[pi][static_cast<size_t>(b)];
            bound = tok.empty() ? SnesPlatformSettings::button_default_pad_token(b) : tok;
        } else {
            bound = retcomm::sdl_scancode_name(s.kb_scancode[pi][static_cast<size_t>(b)]);
        }

        bool live_pressed = false;
        if (!is_pad && kb_keys) {
            const int sc = s.kb_scancode[pi][static_cast<size_t>(b)];
            if (sc > 0 && sc < kb_nkeys) live_pressed = kb_keys[sc] != 0;
        }

        const float cx = img_x + ((hits[i].nx - kSnesCropU0) / crop_w) * img_w;
        const float cy = img_y + ((hits[i].ny - kSnesCropV0) / crop_h) * img_h;
        ImGui::SetCursorScreenPos(ImVec2(cx - chip_w * 0.5f, cy - chip_h * 0.5f));
        ImGui::PushID(b);
        const bool capturing =
            !is_pad && d.capturing_player == player && d.capturing_bind == b;
        const std::string chip =
            capturing ? "[ ... ]" : (bound.empty() ? SnesPlatformSettings::button_label(b) : bound);

        if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, th.accent);
        else if (live_pressed) ImGui::PushStyleColor(ImGuiCol_Button, th.good_button);
        else
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(th.control.x, th.control.y, th.control.z, 0.9f));
        if (ImGui::Button(chip.c_str(), ImVec2(chip_w, chip_h))) {
            if (is_pad) {
                ImGui::OpenPopup("##snes_tok");
            } else {
                d.capturing_hotkey = -1;
                d.capturing_player = player;
                d.capturing_bind = b;
            }
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s\nBound: %s%s", SnesPlatformSettings::button_label(b),
                              bound.empty() ? "(unbound)" : bound.c_str(),
                              live_pressed ? "\n(pressed)" : "");
        }
        // A pad bind is a token from a fixed vocabulary, not a key to press, so
        // it picks from a list rather than capturing input.
        if (is_pad && ImGui::BeginPopup("##snes_tok")) {
            ImGui::TextColored(th.text_muted, "%s", SnesPlatformSettings::button_label(b));
            ImGui::Separator();
            auto& tok = s.pad_controls[pi][static_cast<size_t>(b)];
            for (int t = 0; t < SnesPlatformSettings::pad_token_count(); ++t) {
                const char* name = SnesPlatformSettings::pad_token(t);
                if (ImGui::Selectable(name, tok == name)) {
                    tok = name;
                    d.dirty = true;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::SetWindowFontScale(1.f);
    ImGui::PopStyleVar(2);
    ImGui::EndChild();
}

// One seat's controller page, as a modal over the seat list — the same shape
// the PlayStation page uses, and for the same reasons: a three-up header row so
// the labels sit above their controls instead of stretching across the window,
// the pad picture inside a sized child so the chips cannot push anything below
// them, and a fixed footer bar for the profile actions.
void draw_snes_configure_modal(HubModel& hub, const Theme& th, BoxartCache& boxart) {
    using retcomm::SnesPlatformSettings;
    auto& d = hub.snes_settings;
    if (d.configuring_player < 0 || d.configuring_player >= SnesPlatformSettings::kMaxPlayers)
        return;
    const int player = d.configuring_player;
    const size_t pi = static_cast<size_t>(player);
    auto& s = d.settings;
    auto mark = [&] { d.dirty = true; };

    static char name_buf[64]{};
    static std::string selected;
    static std::string status;

    ImGui::OpenPopup("##snes_pad_cfg");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(1100.f, vp->WorkSize.x * 0.96f),
                                    std::min(860.f, vp->WorkSize.y * 0.94f)),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##snes_pad_cfg", nullptr, ImGuiWindowFlags_None)) return;

    char title[64];
    std::snprintf(title, sizeof(title), "CONTROLLER - PLAYER %d%s", player + 1,
                  player < 2 ? "" : "  (multitap seat)");
    ImGui::TextColored(th.accent, "%s", title);
    ImGui::SameLine();
    {
        constexpr float kCloseW = 80.f;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - kCloseW);
        if (ImGui::Button("Close", ImVec2(kCloseW, 0))) {
            cancel_snes_bind_capture(hub);
            d.configuring_player = -1;
            status.clear();
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
    }
    ImGui::Separator();
    ImGui::PushID(player);

    // The map is the seat's device, always: a seat drives one device, so a
    // control offering to show the other one only invites binding a map the
    // seat will never read.
    const bool is_pad = s.player_src[pi] != 1;

    // Labels above their controls, on shared baselines across the columns.
    if (ImGui::BeginTable("snes_cfg_top", 2,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(th.text_muted, "Input source");
        ImGui::TableNextColumn();
        ImGui::TextColored(th.text_muted, "Deadzone (all seats)");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        {
            std::vector<HubGamepadOpt> pads;
            collect_snes_gamepads(hub, pads);
            ImGui::SetNextItemWidth(-1.f);
            draw_snes_player_source_combo(hub, player, pads);
        }
        ImGui::TableNextColumn();
        {
            int pct = std::clamp((s.gamepad_deadzone * 100 + 16383) / 32767, 1, 100);
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::SliderInt("##seat_dz", &pct, 1, 100, "%d%%")) {
                s.gamepad_deadzone = std::clamp((pct * 32767) / 100, 1, 32767);
                mark();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "[GamepadMap] GamepadDeadzone \xe2\x80\x94 snesrecomp keeps one value for "
                    "every seat, so this is shared.");
            }
        }
        ImGui::EndTable();
    }

    // Body in its own sized child: the chips are placed with SetCursorScreenPos
    // and leave the cursor wherever the last one landed, so anything drawn after
    // them in the same window lands on top of the pad. The child contains that.
    const float footer_h = ImGui::GetFrameHeightWithSpacing() * 2.f + 14.f;
    const float body_h = std::max(220.f, ImGui::GetContentRegionAvail().y - footer_h);
    ImGui::BeginChild("snes_cfg_body", ImVec2(0, body_h), ImGuiChildFlags_None);
    draw_snes_pad_map(hub, th, player, is_pad, boxart);
    ImGui::EndChild();

    // Footer: profile actions, fixed widths so nothing is clipped at the edge.
    const auto profiles = retcomm::load_snes_pad_profiles(hub.paths);
    constexpr float kBtnW = 92.f;
    constexpr float kGap = 8.f;
    ImGui::SetNextItemWidth(200.f);
    const char* preview = selected.empty() ? "Profile\xe2\x80\xa6" : selected.c_str();
    if (ImGui::BeginCombo("##snes_profile", preview)) {
        if (profiles.empty()) ImGui::TextColored(th.text_muted, "(none saved yet)");
        for (const auto& pr : profiles) {
            if (ImGui::Selectable(pr.name.c_str(), pr.name == selected)) {
                selected = pr.name;
                std::snprintf(name_buf, sizeof(name_buf), "%s", pr.name.c_str());
                s.pad_controls[pi] = pr.pad_controls;
                for (int b = 0; b < SnesPlatformSettings::kButtonCount; ++b)
                    if (pr.kb_scancode[static_cast<size_t>(b)] > 0)
                        s.kb_scancode[pi][static_cast<size_t>(b)] =
                            pr.kb_scancode[static_cast<size_t>(b)];
                mark();
                status = "Applied " + pr.name;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, kGap);
    ImGui::SetNextItemWidth(180.f);
    hub_input_text_hint("##snes_pname", "profile name", name_buf, sizeof(name_buf));

    auto current = [&]() -> retcomm::SnesPadProfile {
        retcomm::SnesPadProfile pr;
        pr.name = name_buf;
        pr.pad_controls = s.pad_controls[pi];
        pr.kb_scancode = s.kb_scancode[pi];
        return pr;
    };

    ImGui::SameLine(0, kGap);
    ImGui::BeginDisabled(name_buf[0] == '\0');
    if (accent_button("Save", th, ImVec2(kBtnW, 0))) {
        std::string err;
        if (retcomm::save_snes_pad_profile(hub.paths, current(), &err)) {
            selected = name_buf;
            status = "Saved " + selected;
        } else {
            status = "Save failed: " + err;
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0, kGap);
    ImGui::BeginDisabled(selected.empty() || name_buf[0] == '\0' || selected == name_buf);
    if (ImGui::Button("Rename", ImVec2(kBtnW, 0))) {
        std::string err;
        if (retcomm::rename_snes_pad_profile(hub.paths, selected, name_buf, &err)) {
            status = "Renamed " + selected + " to " + name_buf;
            selected = name_buf;
        } else {
            status = "Rename failed: " + err;
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0, kGap);
    ImGui::BeginDisabled(selected.empty());
    if (danger_button("Delete", th, ImVec2(kBtnW, 0))) {
        std::string err;
        if (retcomm::delete_snes_pad_profile(hub.paths, selected, &err)) {
            status = "Deleted " + selected;
            selected.clear();
            name_buf[0] = '\0';
        } else {
            status = "Delete failed: " + err;
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0, kGap);
    if (ImGui::Button("Reset", ImVec2(kBtnW, 0))) {
        const SnesPlatformSettings def;
        s.pad_controls[pi] = def.pad_controls[pi];
        s.kb_scancode[pi] = def.kb_scancode[pi];
        s.apply_defaults_if_unset();
        mark();
        status = "Reset player " + std::to_string(player + 1) + " to defaults";
    }

    if (!status.empty()) ImGui::TextColored(th.text_muted, "%s", status.c_str());

    ImGui::PopID();
    ImGui::EndPopup();
}

// The Gamepads tab: five seats across the whole window, each opening its own
// controller page. Mirrors draw_psx_gamepads_panel — the two platform pages
// should not want different gestures for the same job.
void draw_snes_gamepads_panel(HubModel& hub, const Theme& th, float panel_h,
                              BoxartCache& boxart) {
    using retcomm::SnesPlatformSettings;
    auto& d = hub.snes_settings;
    auto& s = d.settings;
    auto mark = [&] { d.dirty = true; };

    ImGui::BeginChild("snes_gamepads", ImVec2(0, panel_h), ImGuiChildFlags_Borders,
                      page_wheel_flags(hub));

    ImGui::TextColored(th.text_muted, "CONTROLLERS");
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextWrapped(
        "Seats 1-5: the two native controller ports plus a Super Multitap. Each seat "
        "carries its own device, keyboard binds and pad map \xe2\x80\x94 open it with "
        "Configure.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 8));

    std::vector<HubGamepadOpt> pads;
    collect_snes_gamepads(hub, pads);

    const float gap = th.spacing_md;
    const float availw = ImGui::GetContentRegionAvail().x;
    const float pref = 280.f;
    int cols = static_cast<int>((availw + gap) / (pref + gap));
    cols = std::clamp(cols, 1, 3);
    float cardw = (availw - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    if (cardw < 1.f) cardw = availw;

    for (int p = 0; p < SnesPlatformSettings::kMaxPlayers; ++p) {
        if (p % cols) ImGui::SameLine(0, gap);
        else if (p) ImGui::Dummy(ImVec2(0, gap));

        ImGui::PushID(p);
        ImGui::BeginChild("scard", ImVec2(cardw, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::TextColored(th.text_muted, "PLAYER %d%s", p + 1, p < 2 ? "" : "  (multitap)");
        ImGui::Dummy(ImVec2(0, 4));
        draw_snes_player_source_combo(hub, p, pads);
        ImGui::Dummy(ImVec2(0, 4));

        // Configure beside a status light, the way the PlayStation seat cards
        // read. Whether the runner opens a pad for this seat follows the seat's
        // device — there is nothing a separate toggle could usefully disagree
        // with — so [GamepadMap] EnableGamepad<n> is derived on write.
        const float cw = ImGui::GetContentRegionAvail().x;
        const float half = (cw - th.spacing_sm) * 0.5f;
        constexpr float btnh = 32.f;
        if (ImGui::Button("Configure", ImVec2(half, btnh))) {
            cancel_snes_bind_capture(hub);
            d.capturing_hotkey = -1;
            d.configuring_player = p;
        }
        ImGui::SameLine(0, th.spacing_sm);
        {
            const int src = s.player_src[static_cast<size_t>(p)];
            const bool live =
                src == 2 && !s.player_guid[static_cast<size_t>(p)].empty() &&
                std::any_of(pads.begin(), pads.end(), [&](const HubGamepadOpt& g) {
                    return g.live && g.guid == s.player_guid[static_cast<size_t>(p)];
                });
            const char* st = live ? "connected" : (src == 1 ? "keyboard" : "not assigned");
            const bool on = live || src == 1;
            const float sw = 10.f + 8.f + ImGui::CalcTextSize(st).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (half - sw) * 0.5f));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                                 (btnh - ImGui::GetTextLineHeight()) * 0.5f);
            const ImVec2 dot_c = ImGui::GetCursorScreenPos();
            constexpr float r = 4.f;
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(dot_c.x + r, dot_c.y + ImGui::GetTextLineHeight() * 0.5f), r,
                ImGui::ColorConvertFloat4ToU32(on ? th.good : th.text_muted));
            ImGui::Dummy(ImVec2(r * 2.f + 6.f, 0));
            ImGui::SameLine(0, 0);
            ImGui::TextColored(on ? th.good : th.text_muted, "%s", st);
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::EndChild();
    // Begun at this scope, outside the seat-list child — same placement the
    // PlayStation page uses, and the OpenPopup that pairs with it lives in the
    // same function, so the popup id resolves.
    draw_snes_configure_modal(hub, th, boxart);
}

// Rewind's pad gesture (config.ini [Controller] RewindGesture). snesrecomp
// parses a '+'-joined spec of its own token names, so a capture has to emit
// those names rather than a bitmask the way the PlayStation chords do.
//
// The face-button mapping follows the runner's own shipped Controls= line,
// whose comment says it plainly: SNES A is the pad's B and vice versa. l3 / r3
// / back are pad-only controls the SNES pad never had, and snesrecomp accepts
// them by those names.
const char* snes_gesture_token(int sdl_button) {
    switch (sdl_button) {
        case SDL_GAMEPAD_BUTTON_EAST: return "a";
        case SDL_GAMEPAD_BUTTON_SOUTH: return "b";
        case SDL_GAMEPAD_BUTTON_NORTH: return "x";
        case SDL_GAMEPAD_BUTTON_WEST: return "y";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "l";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "r";
        case SDL_GAMEPAD_BUTTON_BACK: return "select";
        case SDL_GAMEPAD_BUTTON_START: return "start";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "l3";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "r3";
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "right";
        default: return nullptr;
    }
}

void cancel_snes_gesture_capture(HubModel& hub) {
    hub.snes_settings.capturing_gesture = -1;
    hub.snes_settings.rewind_gesture_mask = 0;
}

// Commits on RELEASE, like the PlayStation chords: a gesture is several buttons
// held at once, so sampling the first button-down would record "select" every
// time.
void poll_snes_gesture_capture(HubModel& hub) {
    auto& d = hub.snes_settings;
    if (d.capturing_gesture < 0) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        cancel_snes_gesture_capture(hub);
        return;
    }

    unsigned held = 0;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            SDL_Gamepad* pad = gamepad_handle_for_id(ids[i]);
            if (!pad) continue;
            // Union across pads: which controller the player grabbed is not
            // something they should have to tell us first.
            for (int b = 0; b < 21; ++b) {
                if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b)))
                    held |= (1u << b);
            }
        }
        SDL_free(ids);
    }

    if (held) {
        d.rewind_gesture_mask |= held;
        return;                       // still holding — keep accumulating
    }
    if (!d.rewind_gesture_mask) return;   // nothing pressed yet

    std::string spec;
    for (int b = 0; b < 21; ++b) {
        if (!((d.rewind_gesture_mask >> b) & 1u)) continue;
        const char* tok = snes_gesture_token(b);
        if (!tok) continue;           // a button snesrecomp cannot name
        if (!spec.empty()) spec += "+";
        spec += tok;
    }
    if (!spec.empty()) {
        if (d.capturing_gesture == 0) d.settings.rewind_gesture = spec;
        else d.settings.savestate_menu_gesture = spec;
        d.dirty = true;
    }
    cancel_snes_gesture_capture(hub);
}

void draw_snes_settings_panel(HubModel& hub, const Theme& th, BoxartCache& boxart) {
    using retcomm::SnesPlatformSettings;
    poll_snes_hotkey_capture(hub);
    poll_snes_gesture_capture(hub);
    auto& d = hub.snes_settings;
    auto& s = d.settings;
    auto mark = [&] { d.dirty = true; };

    ImGui::BeginChild("snes_settings", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("SUPER NINTENDO SETTINGS");
    ImGui::PopStyleColor();
    // Same tab control the PlayStation page carries: the seats are a page of
    // their own, not a column squeezed beside Display.
    const bool gamepads = d.gamepads_tab;
    constexpr float kTabW = 110.f;
    {
        const float right = ImGui::GetWindowContentRegionMax().x;
        const float wrap_x =
            ImGui::GetCursorPosX() + (right - ImGui::GetCursorPosX()) - kTabW - 12.f;
        const float desc_y = ImGui::GetCursorPosY();
        ImGui::PushTextWrapPos(wrap_x);
        ImGui::TextWrapped(
            "%s",
            gamepads
                ? "Controller seats for Super Nintendo titles. Assignments write "
                  "[Controller] SourceP<n> / GuidP<n> and [GamepadMap] EnableGamepad<n> "
                  "into the global config.ini, applied on install, update, and launch."
                : "Global Display, Audio, Input, and Hotkeys for Super Nintendo titles. "
                  "Saved prefs are merged into each game's config.ini / keybinds.ini on "
                  "install, update, and launch (unless excluded in Manage Game Data). "
                  "Per-game keys such as Widescreen and Shader are left untouched.");
        ImGui::PopTextWrapPos();
        ImGui::SameLine();
        ImGui::SetCursorPosY(desc_y);
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - kTabW));
        if (gamepads) {
            if (good_button("System", th, ImVec2(kTabW, 0))) {
                d.gamepads_tab = false;
                d.configuring_player = -1;
                cancel_snes_bind_capture(hub);
            }
        } else {
            if (good_button("Gamepads", th, ImVec2(kTabW, 0))) d.gamepads_tab = true;
        }
    }
    ImGui::Separator();

    const float footer_h = ImGui::GetFrameHeight() + 24.f;
    const float gap = 12.f;
    const float avail_x = ImGui::GetContentRegionAvail().x;
    const float col_w = std::max(240.f, (avail_x - gap) * 0.42f);
    const float panel_h = std::max(120.f, ImGui::GetContentRegionAvail().y - footer_h);

    if (gamepads) {
        draw_snes_gamepads_panel(hub, th, panel_h, boxart);
    } else {
    ImGui::BeginChild("snes_display_audio", ImVec2(col_w, panel_h), ImGuiChildFlags_Borders,
                      page_wheel_flags(hub));
    ImGui::TextColored(th.text_muted, "DISPLAY");
    ImGui::Separator();
    {
        static const char* kScaleLabels[] = {"1x", "2x", "3x", "4x", "5x", "6x"};
        static const int kScales[] = {1, 2, 3, 4, 5, 6};
        if (settings_combo_values("Window scale", "##ws", th, kScaleLabels, kScales, 6,
                                  &s.window_scale))
            mark();

        static const char* kSnesFs[] = {"Windowed", "Borderless", "Exclusive"};
        if (settings_combo("Fullscreen", "##fs", th, kSnesFs, 3, &s.fullscreen)) mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "[Graphics] Fullscreen. Borderless is a desktop-sized window; Exclusive "
                "takes the display.\nAn SDL3 build maps both to the same window flag, so "
                "they behave alike there.");
        }

        static const char* kAspect[] = {"4:3 (CRT)", "8:7 (Square pixels)", "1:1 (Square frame)"};
        if (settings_combo("Aspect", "##asp", th, kAspect, 3, &s.display_aspect)) mark();

        {
            const auto& opts = snes_renderer_options();
            std::vector<const char*> labels;
            labels.reserve(opts.size());
            for (const auto& o : opts) labels.push_back(o.label.c_str());
            int idx = snes_renderer_index(s);
            if (settings_combo("Renderer", "##renderer", th, labels.data(),
                               static_cast<int>(labels.size()), &idx)) {
                s.renderer = opts[static_cast<size_t>(idx)].id;
                mark();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "Auto lets SDL pick. OpenGL is snesrecomp's own presenter (its "
                    "shader presets and vsync switch).\n"
                    "The rest are SDL render drivers this machine has; a build without "
                    "one falls back to Auto.");
            }
        }

        if (settings_checkbox("Stretch to window", "##iar", th, &s.ignore_aspect_ratio)) mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("[Graphics] IgnoreAspectRatio — fill the window instead of "
                              "keeping the aspect above.");

        if (settings_checkbox("Linear filtering", "##lf", th, &s.linear_filtering)) mark();
        if (settings_checkbox("New PPU renderer", "##nr", th, &s.new_renderer)) mark();
        if (settings_checkbox("No sprite limits", "##nsl", th, &s.no_sprite_limits)) mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("Lift the per-scanline sprite limit (removes flicker; not faithful).");

        if (settings_checkbox("Frame blending", "##fb", th, &s.frame_blend)) mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "[Graphics] FrameBlend — average each presented frame with the previous "
                "one, so alternate-frame flicker reads as translucency the way it did on "
                "a CRT.");
        }

        if (settings_checkbox("VSync", "##vs", th, &s.vsync)) mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("[Graphics] VSync — driver vsync at present time. On by default.");

        {
            static const char* kRaLabels[] = {"Off", "1 frame", "2 frames", "3 frames"};
            static const int kRa[] = {0, 1, 2, 3};
            if (settings_combo_values("Run ahead", "##ra", th, kRaLabels, kRa, 4, &s.run_ahead))
                mark();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip(
                    "[General] RunAhead — hide local input latency by re-simulating ahead.\n"
                    "Each frame costs a whole extra emulated frame every presented frame.");
            }
        }

        if (settings_checkbox("Show perf in title bar", "##perf", th, &s.display_perf_title))
            mark();
        if (settings_checkbox("Autosave", "##as", th, &s.autosave)) mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("[General] Autosave — let the runner write SRAM back on its own.");

        // Local rewind. snesrecomp reads no config key for this — the runner
        // takes it from SNESRECOMP_REWIND — so Retro keeps the preference and
        // sets that variable when it launches a title.
        if (settings_checkbox("Rewind", "##rewind", th, &s.rewind_enabled)) mark();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(
                "Keep a ring of whole-machine snapshots so the Rewind hotkey can step "
                "back through them (on by default, ~16 MB).\n"
                "snesrecomp takes this from the environment, not config.ini, so it "
                "applies to titles launched from Retro \xe2\x80\x94 not to the game's exe "
                "started by hand.");
        }

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        ImGui::TextColored(th.text_muted, "AUDIO");
        if (settings_checkbox("Enable audio", "##ea", th, &s.enable_audio)) mark();
        {
            int vol = std::clamp(s.volume, 0, 100);
            settings_row("Volume", th, kSettingsCtrlW);
            if (ImGui::SliderInt("##vol", &vol, 0, 100, "%d%%")) {
                s.volume = vol;
                mark();
            }
        }
        {
            static const char* kFreqLabels[] = {"32040 Hz", "32000 Hz", "44100 Hz", "48000 Hz"};
            static const int kFreq[] = {32040, 32000, 44100, 48000};
            if (settings_combo_values("Sample rate", "##freq", th, kFreqLabels, kFreq, 4,
                                      &s.audio_freq))
                mark();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("32040 Hz is the SPC's native rate (no resampling).");
        }

    }
    ImGui::EndChild();

    ImGui::SameLine(0.f, gap);
    ImGui::BeginChild("snes_hotkeys", ImVec2(0, panel_h), ImGuiChildFlags_Borders,
                      page_wheel_flags(hub));
    ImGui::TextColored(th.text_muted, "HOTKEYS");
    ImGui::Separator();
    {
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Click a binding, then press a key (Esc cancels). These are the keyboard "
            "shortcuts the runner listens for while a game is running.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6));
        for (int i = 0; i < SnesPlatformSettings::kHotkeyCount; ++i) {
            ImGui::PushID(200 + i);
            const bool cap = d.capturing_hotkey == i;
            const std::string& cur = s.hotkeys[static_cast<size_t>(i)];
            const char* bl = cap ? "[ press... ]"
                                 : (cur.empty() ? SnesPlatformSettings::hotkey_default(i)
                                                : cur.c_str());
            if (settings_bind_button(SnesPlatformSettings::hotkey_label(i), th, bl, cap))
                d.capturing_hotkey = i;
            ImGui::PopID();
        }

        // Controller shortcuts, the same shape the PlayStation page uses: hold
        // the buttons together and release. snesrecomp has exactly one such
        // gesture — [Controller] RewindGesture — and reaches its save-state
        // menu from the keyboard only, so this is the whole section rather
        // than a list with rows that would write keys nothing reads.
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::Separator();
        ImGui::TextColored(th.text_muted, "CONTROLLER SHORTCUTS");
        ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
        ImGui::TextWrapped(
            "Click a binding, then hold the buttons together and release (Esc cancels). "
            "Lets a player reach these from the couch.");
        ImGui::PopStyleColor();
        {
            struct GestureRow {
                const char* label;
                const char* def;
                std::string* value;
            };
            const GestureRow rows[2] = {
                {"Rewind", "Select+R3", &s.rewind_gesture},
                {"Save-state menu", "Select+R", &s.savestate_menu_gesture},
            };
            for (int g = 0; g < 2; ++g) {
                ImGui::PushID(g);
                const bool cap = d.capturing_gesture == g;
                const std::string shown = cap ? "[ hold buttons... ]"
                                              : (rows[g].value->empty() ? rows[g].def
                                                                        : *rows[g].value);
                if (settings_bind_button(rows[g].label, th, shown.c_str(), cap)) {
                    d.capturing_hotkey = -1;
                    cancel_snes_bind_capture(hub);
                    d.capturing_gesture = g;
                    d.rewind_gesture_mask = 0;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip(
                        "Empty means the %s default; Clear writes \"none\" to unbind it.\n"
                        "A gesture needs at least two buttons \xe2\x80\x94 the runner refuses "
                        "one, so an ordinary press mid-fight cannot open it.",
                        rows[g].def);
                }
                ImGui::Dummy(ImVec2(0, 2));
                constexpr float kSmallW = 110.f;
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                              ImGui::GetWindowContentRegionMax().x -
                                                  kSmallW * 2.f - 8.f));
                if (ImGui::Button("Default", ImVec2(kSmallW, 0))) {
                    rows[g].value->clear();
                    cancel_snes_gesture_capture(hub);
                    mark();
                }
                ImGui::SameLine(0, 8);
                if (ImGui::Button("Clear", ImVec2(kSmallW, 0))) {
                    *rows[g].value = "none";
                    cancel_snes_gesture_capture(hub);
                    mark();
                }
                ImGui::Dummy(ImVec2(0, 6));
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    }

    ImGui::Dummy(ImVec2(0, 12));
    const float footer_y = ImGui::GetCursorPosY();
    constexpr float kResetW = 150.f;
    if (accent_button("Save", th, ImVec2(160, 0))) {
        std::string err;
        if (!hub.save_snes_settings(&err)) {
            hub.append_log("Super Nintendo settings save failed: " + err);
            hub.set_status("Save failed");
        } else {
            hub.show_toast("Saved!");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        hub.show_snes_settings = false;
        d.dirty = false;
        d.capturing_hotkey = -1;
        d.configuring_player = -1;
        cancel_snes_bind_capture(hub);
    }
    if (d.dirty) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(th.warn, "unsaved changes");
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(th.text_muted, "%s",
                       retcomm::snes_platform_settings_dir(hub.paths).string().c_str());
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - kResetW, footer_y));
    if (ImGui::Button("Reset to Default", ImVec2(kResetW, 0))) {
        d.capturing_hotkey = -1;
        d.configuring_player = -1;
        cancel_snes_bind_capture(hub);
        s.reset_system_to_defaults();
        mark();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Restore Display, Audio, Hotkeys, keyboard binds, and gamepad maps to\n"
                          "the snesrecomp defaults. Seat assignments are left unchanged.");
    }
    ImGui::EndChild();
}

// Activity console.
//
// Replaces the old docked ACTIVITY strip at the bottom of the window: it cost
// vertical space on every frame to show what is, almost always, nothing the
// player needs. Now it is summoned with ` (grave/tilde) as a translucent
// overlay over the top half of the window, the way a game console drops down.
//
// Lines are one per row rather than wrapped, so a range of them can be selected
// (click, shift-click, ctrl-click, ctrl+A) and copied with ctrl+C or the Copy
// button. Long lines scroll horizontally instead of reflowing, which keeps the
// selection rectangle aligned with what is on screen.
void draw_log_overlay(HubModel& hub, const Theme& th, SDL_Window* window) {
    static ImGuiSelectionBasicStorage selection;
    static bool auto_scroll = true;
    static int cached_count = -1;
    static float cached_width = 0.f;

    auto close_overlay = [&]() {
        hub.log_overlay_open = false;
        hub.hub_refocus_pending = true;
        request_page_focus(hub);
    };

    const ImGuiIO& io = ImGui::GetIO();
    // ` toggles. Never while a text field owns the keyboard, or the key would
    // be swallowed instead of typed.
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) {
        if (hub.log_overlay_open) {
            close_overlay();
        } else {
            hub.log_overlay_open = true;
            hub.log_overlay_focus_pending = true;
        }
    }
    // Escape closes, except while a selection is up: ClearOnEscape uses that
    // press to clear it, and one key should not do both at once.
    if (hub.log_overlay_open && selection.Size == 0 &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        close_overlay();

    constexpr float kSlideSeconds = 0.14f;
    const float target = hub.log_overlay_open ? 1.f : 0.f;
    const float step = io.DeltaTime > 0.f ? io.DeltaTime / kSlideSeconds : 1.f;
    if (hub.log_overlay_t < target) hub.log_overlay_t = std::min(target, hub.log_overlay_t + step);
    else if (hub.log_overlay_t > target) hub.log_overlay_t = std::max(target, hub.log_overlay_t - step);
    if (hub.log_overlay_t <= 0.f) return;
    const float inv = 1.f - hub.log_overlay_t;
    const float t = 1.f - inv * inv * inv; // ease-out cubic

    std::vector<retcomm::hub::LogLine> lines;
    {
        std::lock_guard<std::mutex> lock(hub.mu);
        lines = hub.log_lines;
    }
    const int count = static_cast<int>(lines.size());

    auto level_color = [&](retcomm::hub::LogLevel lv) -> ImVec4 {
        switch (lv) {
        case retcomm::hub::LogLevel::Accent: return th.accent;
        case retcomm::hub::LogLevel::Good: return th.good;
        case retcomm::hub::LogLevel::Warn: return th.warn;
        case retcomm::hub::LogLevel::Error: return ImVec4(0.95f, 0.35f, 0.40f, 1.f);
        case retcomm::hub::LogLevel::Info: break;
        }
        return th.text_muted; // no Theme::error token; Info stays muted
    };

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float h = std::max(160.f, vp->WorkSize.y * 0.5f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y - h * (1.f - t)));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
    if (hub.log_overlay_focus_pending) ImGui::SetNextWindowFocus();
    ImVec4 bg = th.background;
    bg.w = 0.88f * t; // translucent: the page stays readable underneath
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 10.f));
    ImGui::Begin("##activity_overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);

    // Bottom edge: the same neon rule the header uses, so the overlay reads as
    // part of the shell rather than a floating window.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        dl->AddRectFilledMultiColor(ImVec2(wp.x, wp.y + ws.y - 3.f), ImVec2(wp.x + ws.x, wp.y + ws.y),
                                    ImGui::ColorConvertFloat4ToU32(th.accent),
                                    ImGui::ColorConvertFloat4ToU32(th.good),
                                    ImGui::ColorConvertFloat4ToU32(th.good),
                                    ImGui::ColorConvertFloat4ToU32(th.accent));
    }

    auto copy_lines = [&](bool selected_only) {
        std::string clip;
        if (selected_only && selection.Size > 0) {
            std::vector<int> idx;
            idx.reserve(static_cast<size_t>(selection.Size));
            void* it = nullptr;
            ImGuiID id = 0;
            while (selection.GetNextSelectedItem(&it, &id)) idx.push_back(static_cast<int>(id));
            std::sort(idx.begin(), idx.end());
            for (int i : idx) {
                if (i < 0 || i >= count) continue;
                if (!clip.empty()) clip.push_back('\n');
                clip += lines[static_cast<size_t>(i)].text;
            }
        } else {
            // Everything, oldest first. The whole buffer is capped upstream.
            for (const auto& l : lines) {
                if (!clip.empty()) clip.push_back('\n');
                clip += l.text;
            }
        }
        ImGui::SetClipboardText(clip.empty() ? "(no activity yet)" : clip.c_str());
    };

    // Header row.
    ImGui::PushStyleColor(ImGuiCol_Text, th.text_muted);
    ImGui::TextUnformatted("ACTIVITY");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (selection.Size > 0)
        ImGui::TextColored(th.focus, "%d of %d line%s selected", selection.Size, count,
                           count == 1 ? "" : "s");
    else
        ImGui::TextColored(th.text_muted, "%d line%s  ·  ` closes  ·  click / shift-click to select",
                           count, count == 1 ? "" : "s");
    {
        const float pad = ImGui::GetStyle().FramePadding.x * 2.f;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const char* copy_label = selection.Size > 0 ? "Copy selected" : "Copy all";
        const float copy_w = ImGui::CalcTextSize(copy_label).x + pad;
        const float export_w = ImGui::CalcTextSize("Export").x + pad;
        const float close_w = ImGui::CalcTextSize("Close").x + pad;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                      ImGui::GetWindowContentRegionMax().x - copy_w - gap -
                                          export_w - gap - close_w));
        if (ImGui::SmallButton(copy_label)) copy_lines(true);
        ImGui::SameLine();
        bool file_busy = false;
        {
            std::lock_guard<std::mutex> lock(hub.file_pick_mu);
            file_busy = hub.file_pick_busy;
        }
        ImGui::BeginDisabled(file_busy || window == nullptr);
        if (ImGui::SmallButton("Export")) begin_export_activity_log(hub, window);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Close")) close_overlay();
    }
    ImGui::Separator();

    // Widest line decides the horizontal scroll extent. Recomputed when the
    // buffer grows (and once on open), not every frame.
    if (cached_count != count) {
        cached_count = count;
        cached_width = 0.f;
        for (const auto& l : lines)
            cached_width = std::max(cached_width, ImGui::CalcTextSize(l.text.c_str()).x);
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0.25f * t));
    ImGui::BeginChild("activity_scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (hub.log_overlay_focus_pending) {
        hub.log_overlay_focus_pending = false;
        ImGui::SetNavCursorVisible(true);
    }

    // Stick to the newest line until the reader scrolls up.
    const float prev_max = ImGui::GetScrollMaxY();
    if (prev_max > 1.f && ImGui::GetScrollY() < prev_max - 16.f) auto_scroll = false;

    if (count == 0) {
        ImGui::TextColored(th.text_muted, "(no activity yet)");
    } else {
        const float row_w = std::max(cached_width, ImGui::GetContentRegionAvail().x);
        ImGuiMultiSelectIO* ms_io = ImGui::BeginMultiSelect(
            ImGuiMultiSelectFlags_ClearOnEscape | ImGuiMultiSelectFlags_BoxSelect1d,
            selection.Size, count);
        selection.ApplyRequests(ms_io);

        ImGuiListClipper clipper;
        clipper.Begin(count);
        if (ms_io->RangeSrcItem != -1)
            clipper.IncludeItemByIndex(static_cast<int>(ms_io->RangeSrcItem));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& line = lines[static_cast<size_t>(i)];
                ImGui::SetNextItemSelectionUserData(i);
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_Text, level_color(line.level));
                ImGui::Selectable(line.text.c_str(), selection.Contains(static_cast<ImGuiID>(i)),
                                  ImGuiSelectableFlags_None, ImVec2(row_w, 0.f));
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }
        ms_io = ImGui::EndMultiSelect();
        selection.ApplyRequests(ms_io);

        // Ctrl+C copies the selection while the console has the keyboard.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, ImGuiInputFlags_RouteGlobal))
            copy_lines(true);
    }

    // SetScrollHereY needs an item; with a clipper there may be none, so drive
    // the scroll value directly.
    if (auto_scroll) ImGui::SetScrollY(ImGui::GetScrollMaxY());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 16.f) auto_scroll = true;

    ImGui::EndChild();
    ImGui::PopStyleColor(); // ChildBg
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(); // WindowBg
}

} // namespace

#if defined(RETCOMM_HUB_HAVE_PLAY)
// Direct mode (Retro-Runtime docs/HOST_LIFECYCLE.md §3): `--run-core` boots straight into a
// core in this window and exits when the player closes it. No library pages,
// no setup wizard -- the same host a standalone release is.
struct DirectPlay {
    bool active = false;
    retcomm::hub::PlayArgs args;
};

DirectPlay parse_direct_play(int argc, char** argv) {
    DirectPlay d;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_val = i + 1 < argc;
        if (a == "--run-core" && has_val) {
            d.active = true;
            d.args.core = argv[++i];
        } else if (a == "--rom" && has_val) d.args.rom = argv[++i];
        else if (a == "--title-dir" && has_val) d.args.title_dir = argv[++i];
        else if (a == "--tpak1-rom" && has_val) d.args.tpak_rom = argv[++i];
        else if (a == "--tpak1-save" && has_val) d.args.tpak_save = argv[++i];
        else if (a == "--no-gl") d.args.gl = false;
        else if (a == "--opt" && has_val) {
            const std::string kv = argv[++i];
            const auto eq = kv.find('=');
            if (eq != std::string::npos) d.args.options[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
    }
    return d;
}

int run_direct_play(SDL_Window* window, UiScale& ui, const DirectPlay& d, const HubModel& hub) {
    retcomm::hub::PlaySession play;
    const std::string stem = d.args.core.stem().string();
    const fs::path runner = hub.exe_dir / "retro-core-runner";
    const fs::path session = hub.paths.data_dir / "sessions" / stem;
    const fs::path saves = hub.paths.data_dir / "saves" / stem;
    std::string err;
    if (!play.start(d.args, runner, session, saves, &err)) {
        std::fprintf(stderr, "retro-hub: cannot start %s: %s\n", d.args.core.string().c_str(),
                     err.c_str());
        return 1;
    }
    std::fprintf(stderr, "retro-hub: running %s (session %s, saves %s)\n",
                 d.args.core.string().c_str(), session.string().c_str(), saves.string().c_str());
    bool running = true;
    while (running && !play.finished()) {
        hub_sync_open_gamepads();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            scale_mouse_event(e, ui.coords);
            if (!play.handle_event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                e.window.windowID == SDL_GetWindowID(window))
                running = false;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F11) {
                const bool fs_now = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                SDL_SetWindowFullscreen(window, !fs_now);
            }
        }
        play.tick();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        apply_ui_scale_frame(window, ui);
        ImGui::NewFrame();
        play.draw();
        ImGui::Render();
        int fb_w = 0, fb_h = 0;
        SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }
    play.shutdown();
    return 0;
}
#endif

int main(int argc, char** argv) {
    // `--play <title-id>`: start normally, press Play on that title, exit when
    // it closes. What `retcomm launch` hands a core title to, so a Steam
    // shortcut waits for the game.
    std::string play_title_id;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--play") play_title_id = argv[i + 1];
    }
#if defined(RETCOMM_HUB_HAVE_PLAY)
    const DirectPlay direct = parse_direct_play(argc, argv);
    if (direct.active && direct.args.rom.empty()) {
        std::fprintf(stderr, "retro-hub: --run-core needs --rom <image>\n");
        return 2;
    }
#endif

    // Steam only reads its screen-keyboard hint at startup.
    retcomm::hub::osk_configure_hints();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const char* glsl = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    // HIGH_PIXEL_DENSITY asks for a native-resolution backbuffer where the
    // platform scales windows for us (macOS retina, Wayland); on Windows and X11
    // it is a no-op. Either way the pixel size is the truth and UiScale turns it
    // back into the logical 1280x800 this UI is written against.
    SDL_Window* window =
        SDL_CreateWindow("Retro Launcher", 1280, 800,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                             SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Controller first: the D-pad / left stick move the focus ring, A activates,
    // B backs out. Mouse use hides the ring again (ConfigNavCursorVisibleAuto).
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    // RETCOMM_HUB_NAV_DEBUG=1 prints ImGui's own focus/nav event log to the
    // terminal — the only honest way to see where a controller's focus went.
    if (const char* nav_dbg = std::getenv("RETCOMM_HUB_NAV_DEBUG"); nav_dbg && nav_dbg[0] == '1') {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        g.DebugLogFlags |= ImGuiDebugLogFlags_EventFocus | ImGuiDebugLogFlags_EventNav |
                           ImGuiDebugLogFlags_OutputToTTY;
    }

    // The atlas has to be built before the first frame, and config.json is not
    // loaded yet (that needs hub.paths below), so start from the display's own
    // scale. A pinned config ui_scale is applied a few lines further down.
    UiScale ui = resolve_ui_scale(window, 0.f);
    load_hub_fonts(ui.px);

    const Theme th = retcomm::hub::crt_theme();
    retcomm::hub::apply_imgui_style(th);

    ImGui_ImplSDL3_InitForOpenGL(window, gl);
    // Any connected pad drives the hub, not only the first one SDL enumerated.
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    ImGui_ImplOpenGL3_Init(glsl);

    HubModel hub;
    // exe_dir has to come first: the portable root marker lives beside the
    // binary, and default_paths() can only honour it when told where that is.
    // The CLI already passes it (main.cpp); the hub used to resolve without it
    // and so silently ignored a portable build's own root.
    if (argc > 0 && argv[0] && argv[0][0] != '\0') {
        std::error_code ec;
        hub.exe_dir = fs::weakly_canonical(fs::path(argv[0]).parent_path(), ec);
        if (ec || hub.exe_dir.empty()) hub.exe_dir = fs::path(argv[0]).parent_path();
    }
    hub.paths = retcomm::default_paths(hub.exe_dir);
    hub.cfg = retcomm::load_app_config(hub.paths.config_path);
    retcomm::set_github_token(hub.cfg.github_token);

    // Now that config.json is in, honour a pinned ui_scale and give the window
    // the logical 1280x800 it was always meant to be — on a 150% display that
    // is 1920x1200 pixels, which no 1080p monitor can show.
    {
        const UiScale want = resolve_ui_scale(window, hub.cfg.ui_scale);
        if (want.px != ui.px) rebuild_hub_fonts(want.px);
        ui = want;
        fit_window_to_display(window, ui);
        // Borderless fullscreen when the user last left it on; fit first so
        // Restore (F11 again) lands on a sane windowed size.
        if (hub.cfg.fullscreen) SDL_SetWindowFullscreen(window, true);
        std::fprintf(stderr, "retro-hub: UI scale %.2f (window coords x%.2f, %s)\n",
                     static_cast<double>(ui.px), static_cast<double>(ui.coords),
                     hub.cfg.ui_scale > 0.f ? "pinned in config" : "from display");
    }

#if defined(RETCOMM_HUB_HAVE_PLAY)
    if (direct.active) {
        const int rc = run_direct_play(window, ui, direct, hub);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        hub_close_all_gamepads();
        SDL_GL_DestroyContext(gl);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return rc;
    }
#endif

    // First run: the wizard has not yet asked the user where Retro should keep
    // its files, so this launch must not create any of them. ensure_dirs() and
    // the catalog cache both land under data_dir — running them here is what
    // left a stray data folder at the default location that then collided with
    // whatever the user picked a moment later. complete_setup() does this work
    // once the location is settled.
    const bool setup_pending = !retcomm::hub_setup_completed(hub.paths, hub.exe_dir) &&
                               hub.cfg.library_root.empty();

    bool catalog_updated = false;
    if (!setup_pending) {
      try {
        retcomm::ensure_dirs(hub.paths);
        const auto sync = retcomm::maybe_auto_update_catalog(hub.paths, hub.cfg);
        if (!sync.ok && !sync.skipped)
            hub.append_log(std::string("catalog auto-update: ") + sync.message);
        else if (sync.ok && !sync.skipped) {
            hub.append_log(sync.message);
            catalog_updated = true;
        }
        const fs::path cat = retcomm::resolve_catalog_dir(fs::path(argv[0]).parent_path(), {},
                                                          &hub.paths);
        hub.catalog = retcomm::load_catalog(cat);
        hub.apply_core_titles();
        std::string cat_log = "Catalog: " + cat.string() + " (" +
                              std::to_string(hub.catalog.titles.size()) + " titles";
        if (!hub.catalog.release_tag.empty())
            cat_log += ", " + hub.catalog.release_tag;
        if (!hub.catalog.catalog_date.empty())
            cat_log += ", " + hub.catalog.catalog_date;
        cat_log += ")";
        hub.append_log(cat_log);
      } catch (const std::exception& e) {
        hub.append_log(std::string("catalog error: ") + e.what());
      }
    }
    hub.launcher_version = retcomm::retcomm_app_version();
    hub.refresh_rows(false);
    // Updates wipe exe-dir markers; if library_root is already set the user has
    // been through setup before, so adopt it and refresh the durable data-dir
    // marker rather than prompting again.
    if (setup_pending) {
        hub.open_setup(); // pre-fills any partial config.json
        hub.set_status("First-time setup — choose where Retro keeps its files");
    } else if (!retcomm::hub_setup_completed(hub.paths, hub.exe_dir)) {
        std::string marker_err;
        if (!retcomm::mark_hub_setup_completed(hub.paths, hub.exe_dir, &marker_err))
            hub.append_log("setup marker migrate failed: " + marker_err);
        hub.set_status("Ready");
    } else {
        hub.set_status("Ready");
    }
    // After a real catalog download, pull covers for titles missing from cache.
    if (catalog_updated) hub.start_job(HubJob::FetchBoxart);
    // …and bind any newly catalogued titles to ROMs the user already owns.
    hub.pending_auto_scan_new_titles =
        catalog_updated && hub.cfg.auto_scan_after_catalog_update;
    // Defer game + toolchain update checks until idle (after catalog boxart job).
    // Never while first-run setup is still open: every job worker opens with
    // ensure_dirs(), which would recreate the tree this launch is deliberately
    // leaving uncreated until the user picks a location.
    hub.pending_startup_update_check = !setup_pending && hub.cfg.check_updates_on_startup;

    retcomm::hub::BoxartCache boxart;
    // --play: requested once the loop is running; `play_seen` notes that it
    // actually started, so its end (or a failure to start) ends the hub.
    bool play_requested = false;
    bool play_seen = false;
    int exit_code = 0;
#if defined(RETCOMM_HUB_HAVE_PLAY)
    // A core title being played in this window (Retro-Runtime docs/HOST_LIFECYCLE.md "Running"):
    // while set, it owns events and drawing, and the library waits behind it.
    std::unique_ptr<retcomm::hub::PlaySession> play;
#endif
    bool running = true;
    while (running) {
        // Keep gamepads open so SDL3 delivers GAMEPAD_BUTTON / AXIS events.
        hub_sync_open_gamepads();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            scale_mouse_event(e, ui.coords);
#if defined(RETCOMM_HUB_HAVE_PLAY)
            if (play) {
                // The game has the controls: none of the library's shortcuts
                // (Start opens the drawer) may fire underneath it.
                if (!play->handle_event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_QUIT) running = false;
                if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                    e.window.windowID == SDL_GetWindowID(window))
                    running = false;
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F11) {
                    hub.cfg.fullscreen = !hub.cfg.fullscreen;
                    SDL_SetWindowFullscreen(window, hub.cfg.fullscreen);
                    retcomm::save_app_config(hub.paths.config_path, hub.cfg);
                }
                continue;
            }
#endif
            if (poll_snes_bind_capture(hub, e) || poll_psx_bind_capture(hub, e)) {
                // Still feed ImGui so the modal stays responsive, but skip
                // duplicate key handling for capture commits.
                ImGui_ImplSDL3_ProcessEvent(&e);
            } else {
                ImGui_ImplSDL3_ProcessEvent(&e);
            }
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                e.window.windowID == SDL_GetWindowID(window))
                running = false;
            // Regaining the OS focus re-initialises ImGui's nav onto the first
            // header item; a controller expects the ring on the content instead.
            if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED && !hub.drawer_open)
                request_page_focus(hub);
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F11) {
                hub.cfg.fullscreen = !hub.cfg.fullscreen;
                SDL_SetWindowFullscreen(window, hub.cfg.fullscreen);
                retcomm::save_app_config(hub.paths.config_path, hub.cfg);
            }
            // Start / Menu / Options opens the drawer, the way a console shell's
            // system button does. Not during first-run setup or while a settings
            // page holds unsaved edits.
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                e.gbutton.button == SDL_GAMEPAD_BUTTON_START && !hub.show_setup &&
                !any_settings_page_open(hub)) {
                toggle_nav_drawer(hub);
            }
        }
        tick_psx_bind_capture_poll(hub);
        if (hub.request_exit.load()) running = false;

        hub.apply_pending_folder_pick();
        hub.apply_pending_file_pick();
        if (hub.pending_add_core_title) {
            hub.pending_add_core_title = false;
            begin_file_pick(hub, window, retcomm::hub::FilePickKind::AddCoreTitle, {},
                            "rcore core sidecar", {"toml"}, /*allow_many=*/false);
        }

        // First run: the catalog could not be fetched before the wizard picked a
        // data folder, so do it the moment setup finishes.
        if (hub.pending_initial_catalog && !hub.show_setup && !hub.job_running.load()) {
            hub.pending_initial_catalog = false;
            hub.start_job(HubJob::RefreshCatalog);
        }
        // Run before the update check so new games appear as early as possible.
        // Both start jobs, so both wait for the setup wizard to close.
        if (hub.pending_auto_scan_new_titles && !hub.show_setup && !hub.job_running.load()) {
            hub.pending_auto_scan_new_titles = false;
            hub.auto_scan_for_new_titles("Catalog update");
        } else if (hub.pending_startup_update_check && !hub.show_setup &&
                   !hub.job_running.load()) {
            hub.pending_startup_update_check = false;
            if (hub.cfg.check_updates_on_startup) hub.start_job(HubJob::CheckUpdates);
        }
        // Legacy pending_launch (CheckLaunchUpdate now launches on its own worker).
        if (!hub.launch_running.load()) {
            std::string launch_id;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                launch_id = std::move(hub.pending_launch_title_id);
                hub.pending_launch_title_id.clear();
            }
            if (!launch_id.empty()) hub.start_job(HubJob::Launch, launch_id);
        }
        // Drain Install/Update queue when the main worker is free.
        if (!hub.job_running.load() && hub.queued_job_count() > 0)
            hub.start_next_queued_job();
        // After the queue drains, prune shared caches out-of-process (skipped
        // during each build so multi-title updates do not OOM the hub).
        if (!hub.job_running.load() && hub.queued_job_count() == 0)
            hub.maybe_run_deferred_cache_gc();

        // Follow the display: dragging onto a differently scaled monitor, or an
        // edit in Settings, re-rasterizes the atlas. Between frames, so it is
        // safe to drop the old fonts here.
        //
        // The saved config is the preference everywhere except inside the
        // Settings pane, where the combo previews live. Reading the draft
        // unconditionally used to clobber a pinned ui_scale with the draft's
        // zero-initialised "auto" on the first frame — so a config scale was
        // applied at startup and then thrown away before anything was drawn —
        // and left a cancelled preview in effect until the next launch.
        {
            const float pref = hub.show_settings ? hub.settings.ui_scale : hub.cfg.ui_scale;
            const UiScale want = resolve_ui_scale(window, pref);
            if (want.px != ui.px) {
                std::fprintf(stderr, "retro-hub: UI scale %.2f -> %.2f (window coords x%.2f)\n",
                             static_cast<double>(ui.px), static_cast<double>(want.px),
                             static_cast<double>(want.coords));
                rebuild_hub_fonts(want.px);
            }
            ui = want;
        }

#if defined(RETCOMM_HUB_HAVE_PLAY)
        // Start a requested core title, once nothing else is playing.
        if (!play) {
            std::optional<HubModel::PlayRequest> req;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                req.swap(hub.pending_play);
            }
            if (req) {
                retcomm::hub::PlayArgs args;
                args.core = req->core_library;
                args.rom = req->rom;
                args.title_dir = req->title_dir;
                const fs::path session = hub.paths.data_dir / "sessions" / req->title_id;
                const fs::path saves = hub.paths.data_dir / "saves" / req->title_id;
                auto p = std::make_unique<retcomm::hub::PlaySession>();
                std::string err;
                if (p->start(args, hub.exe_dir / "retro-core-runner", session, saves, &err)) {
                    play = std::move(p);
                    hub.append_log("Playing " + req->name + " through its core (session " +
                                   session.string() + ")");
                } else {
                    hub.append_log("Cannot start " + req->name + ": " + err);
                    hub.set_status("Cannot start " + req->name);
                }
            }
        }
        if (play) play->tick();
#endif
        if (!play_title_id.empty()) {
            if (!play_requested && !setup_pending) {
                play_requested = hub.start_job(HubJob::Launch, play_title_id);
                if (!play_requested) {
                    std::fprintf(stderr, "retro-hub: --play %s: could not start\n",
                                 play_title_id.c_str());
                    running = false;
                }
            }
#if defined(RETCOMM_HUB_HAVE_PLAY)
            if (play) play_seen = true;
            bool pending = false;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                pending = hub.pending_play.has_value();
            }
            if (play_requested && !play && !pending && !hub.launch_running.load()) {
                // Either the game closed, or it never started: say which.
                if (!play_seen) {
                    // The reason is the worker's last log line ("unknown
                    // title: …", "No matching ROM …"), not the status bar.
                    std::string why;
                    {
                        std::lock_guard<std::mutex> lock(hub.mu);
                        std::string l = hub.log;
                        while (!l.empty() && l.back() == '\n') l.pop_back();
                        const auto nl = l.rfind('\n');
                        why = nl == std::string::npos ? l : l.substr(nl + 1);
                        if (why.empty()) why = hub.status;
                    }
                    std::fprintf(stderr, "retro-hub: --play %s did not start: %s\n",
                                 play_title_id.c_str(), why.c_str());
                    exit_code = 1;
                }
                running = false;
            }
#endif
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        apply_ui_scale_frame(window, ui);
        ImGui::NewFrame();

#if defined(RETCOMM_HUB_HAVE_PLAY)
        const bool playing = play != nullptr;
        if (playing) {
            play->draw();
            if (play->finished()) {
                play->shutdown();
                play.reset();
                hub.set_status("Ready");
                request_page_focus(hub);
            }
        }
#else
        const bool playing = false;
#endif
        if (!playing) {
        if (hub.pending_open_mods) {
            hub.pending_open_mods = false;
            close_settings_pages(hub);
            hub.show_mods_page = true;
            hub.mods_focus_pending = true;
        }
        if (hub.pending_open_library) {
            hub.pending_open_library = false;
            // Prefill from the platform the user is browsing.
            if (hub.library_nav != retcomm::hub::LibraryNav::Platforms &&
                !hub.library_platform.empty()) {
                hub.library_import_platform = hub.library_platform;
                hub.scans_platform_filter = hub.library_platform;
            }
            close_settings_pages(hub);
            hub.show_library_panel = true;
        }

        // Slide the drawer before the page is drawn: the page needs the same
        // value to know whether it is behind the panel this frame.
        const float drawer_t = tick_nav_drawer(hub);
        const bool page_inert = hub.drawer_t > 0.f;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        if (hub.hub_refocus_pending && hub.drawer_t <= 0.f) {
            ImGui::SetNextWindowFocus();
            hub.hub_refocus_pending = false;
        }
        ImGui::Begin("##hub", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        // Everything under the drawer is inert while it is open — no hover, no
        // clicks, no nav ring wandering into the page behind the panel.
        //
        // This has to be BeginDisabled() rather than the flag it pushes:
        // PushItemFlag(ImGuiItemFlags_Disabled) sets the item flag without
        // raising g.DisabledStackSize, and a tooltip opened anywhere inside a
        // disabled region takes ImGui's BeginDisabledOverrideReenable() path,
        // whose End asserts that counter is still positive. The hub raises
        // tooltips on disabled items in ten places, so that was an abort
        // waiting for the first hover. DisabledAlpha is pinned to 1 for the
        // duration so nothing fades: the shading is the drawer's job, not a
        // wash over each widget.
        const float disabled_alpha_backup = ImGui::GetStyle().DisabledAlpha;
        if (page_inert) {
            ImGui::GetStyle().DisabledAlpha = 1.f;
            ImGui::BeginDisabled(true);
        }

        draw_marquee(hub, th, ImGui::GetContentRegionAvail().x);

        // The body owns the whole area below the header now: activity moved into
        // the ` overlay, so nothing is permanently parked at the bottom.
        ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);

        if (hub.show_mods_page) {
            ImGui::BeginChild("mods_page_host", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            draw_mods_page(hub, th);
            ImGui::EndChild();
        } else if (hub.show_library_panel) {
            ImGui::BeginChild("library_panel_host", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            draw_library_panel(hub, th, window);
            ImGui::EndChild();
        } else if (hub.show_settings) {
            ImGui::BeginChild("settings_host", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            draw_settings_panel(hub, th, window);
            ImGui::EndChild();
        } else if (hub.show_romm_settings) {
            ImGui::BeginChild("romm_settings_host", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            draw_romm_settings_panel(hub, th);
            ImGui::EndChild();
        } else if (hub.show_psx_settings) {
            ImGui::BeginChild("psx_settings_host", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            draw_psx_settings_panel(hub, th, boxart);
            ImGui::EndChild();
        } else if (hub.show_snes_settings) {
            ImGui::BeginChild("snes_settings_host", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            draw_snes_settings_panel(hub, th, boxart);
            ImGui::EndChild();
        } else if (hub.library_nav == retcomm::hub::LibraryNav::Detail) {
            // One title, full window: the page splits itself into info + actions.
            ImGui::BeginChild("title_page_host", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            draw_detail(hub, boxart, th, window);
            ImGui::EndChild();
        } else {
            // Home (platform cards) and the title grid each own the whole body.
            // Nothing is parked beside them; picking a card opens the next page.
            ImGui::BeginChild("home", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            draw_library(hub, boxart, th);
            ImGui::EndChild();
        }

        ImGui::EndChild(); // body
        if (page_inert) {
            ImGui::EndDisabled();
            ImGui::GetStyle().DisabledAlpha = disabled_alpha_backup;
        }

        draw_setup_wizard(hub, boxart, th, window);
        draw_setup_scan_prompt(hub, th);
        draw_data_root_dialog(hub, th, window);
        draw_nav_drawer(hub, th, drawer_t);

        draw_log_overlay(hub, th, window);

        // Import / scan toasts.
        {
            static std::string toast_text;
            static double toast_until = 0.0;
            if (hub.toast_pending.exchange(false)) {
                std::lock_guard<std::mutex> lock(hub.mu);
                toast_text = hub.toast_message;
                toast_until = ImGui::GetTime() + 4.5;
            }
            if (!toast_text.empty() && ImGui::GetTime() < toast_until) {
                const ImGuiViewport* tvp = ImGui::GetMainViewport();
                const ImVec2 ts = ImGui::CalcTextSize(toast_text.c_str(), nullptr, false, 420.f);
                const float pad = 14.f;
                const ImVec2 sz(std::min(440.f, ts.x + pad * 2.f), ts.y + pad * 2.f);
                // Bottom-right, above the collapsed Activity bar.
                const ImVec2 pos(tvp->WorkPos.x + tvp->WorkSize.x - sz.x - 16.f,
                                 tvp->WorkPos.y + tvp->WorkSize.y - sz.y - 48.f);
                ImGui::SetNextWindowPos(pos);
                ImGui::SetNextWindowSize(sz);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, th.background2);
                ImGui::PushStyleColor(ImGuiCol_Border, th.accent);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
                ImGui::Begin("##hub_toast", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoSavedSettings);
                ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + sz.x - pad * 2.f);
                ImGui::TextColored(th.text, "%s", toast_text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::End();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
            } else if (ImGui::GetTime() >= toast_until) {
                toast_text.clear();
            }
        }

        if (hub.show_install_root_prompt) {
            ImGui::OpenPopup("Install location###install_root_prompt");
            hub.show_install_root_prompt = false;
        }
        if (ImGui::BeginPopupModal("Install location###install_root_prompt", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            const std::string tid = hub.install_root_prompt_id;
            const TitleRow* prow = nullptr;
            for (const auto& r : hub.rows) {
                if (r.id == tid) {
                    prow = &r;
                    break;
                }
            }
            const auto roots = retcomm::effective_install_roots(hub.cfg, hub.paths);
            const bool move_only = hub.install_root_prompt_move;
            const fs::path& from_apps = hub.install_root_prompt_from_apps;
            const int current_idx = retcomm::find_install_root_index(roots, from_apps);
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped("%s", prow ? prow->name.c_str() : tid.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            if (move_only) {
                ImGui::TextWrapped(
                    "Choose where to move this game's install data (releases, build files, "
                    "and preserved saves/config).");
                if (!from_apps.empty()) {
                    ImGui::Dummy(ImVec2(0, 4));
                    ImGui::TextColored(th.text_muted, "Currently installed in:");
                    if (current_idx >= 0) {
                        const auto& cur = roots[static_cast<size_t>(current_idx)];
                        ImGui::TextWrapped("%s — %s",
                                           cur.label.empty() ? "Install" : cur.label.c_str(),
                                           cur.path.string().c_str());
                    } else {
                        ImGui::TextWrapped("%s", from_apps.string().c_str());
                    }
                }
            } else if (prow && (prow->installed || prow->install_dir_present ||
                                prow->has_preserved_state)) {
                ImGui::TextWrapped(
                    "Choose where to install this game. Picking a different location moves "
                    "existing install / preserved data there first.");
            } else {
                ImGui::TextWrapped("Choose where to install this game.");
            }
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 8));
            if (roots.empty()) {
                ImGui::TextColored(th.warn, "No install locations configured.");
            } else {
                for (int i = 0; i < static_cast<int>(roots.size()); ++i) {
                    ImGui::PushID(i);
                    const auto& e = roots[static_cast<size_t>(i)];
                    const bool is_current =
                        current_idx == i ||
                        retcomm::same_install_root_path(e.path, from_apps);
                    char label[1152];
                    std::snprintf(label, sizeof(label), "%s%s\n%s",
                                  e.label.empty() ? "Install" : e.label.c_str(),
                                  is_current ? "  (current)" : "", e.path.string().c_str());
                    if (ImGui::RadioButton(label, &hub.install_root_prompt_index, i)) {
                        // index updated by RadioButton
                    }
                    ImGui::PopID();
                }
            }
            ImGui::Dummy(ImVec2(0, 10));
            const bool can_confirm = !tid.empty() && !roots.empty();
            const bool job_busy = hub.job_running.load();
            const HubJob confirm_job = move_only ? HubJob::MoveInstall : HubJob::Install;
            const bool already_queued = hub.is_job_queued(confirm_job, tid);
            const bool job_active =
                job_busy && hub.job == confirm_job && hub.job_title_id == tid;
            const bool same_as_current =
                move_only && can_confirm && hub.install_root_prompt_index >= 0 &&
                hub.install_root_prompt_index < static_cast<int>(roots.size()) &&
                retcomm::same_install_root_path(
                    roots[static_cast<size_t>(hub.install_root_prompt_index)].path, from_apps);
            const char* confirm_lbl =
                job_active      ? (move_only ? "Moving…" : "Installing…")
                : already_queued ? "Queued"
                : same_as_current ? "Already here"
                : job_busy ? (move_only ? "Queue Move" : "Queue Install")
                           : "Confirm";
            ImGui::BeginDisabled(!can_confirm || job_active || already_queued ||
                                 same_as_current);
            if (good_button(confirm_lbl, th, ImVec2(-1, 0))) {
                hub.confirm_install_root_and_continue();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (same_as_current && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                                        ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("Pick a different location to move this install.");
            }
            if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
                hub.install_root_prompt_id.clear();
                hub.install_root_prompt_move = false;
                hub.install_root_prompt_from_apps.clear();
                hub.job_apps_dir.clear();
                ImGui::CloseCurrentPopup();
            }
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        if (hub.show_missing_rom_prompt) {
            ImGui::OpenPopup("ROM not found###missing_rom_prompt");
            hub.show_missing_rom_prompt = false;
        }
        if (ImGui::BeginPopupModal("ROM not found###missing_rom_prompt", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            const std::string tid = hub.missing_rom_prompt_id;
            const TitleRow* prow = nullptr;
            for (const auto& r : hub.rows) {
                if (r.id == tid) {
                    prow = &r;
                    break;
                }
            }
            const std::string plat = prow ? prow->platform : std::string{};
            const char* plat_label = plat.empty() ? "this platform" : platform_display_name(plat);
            // Always offer the same actions; RomM download is disabled until sync is set up.
            const bool romm_sync_ok = prow && prow->romm_ready;
            const bool romm_download_ok =
                romm_sync_ok && prow->has_rom_identity && !tid.empty();
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped("%s", prow ? prow->name.c_str() : tid.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextWrapped(
                "No verified ROM is in your library yet. Rescan %s for a matching dump, "
                "import the files, or download from RomM when sync is configured "
                "(multi-track discs need the full .cue + track set).",
                plat_label);
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            const bool job_busy = hub.job_running.load();
            const bool any_scan_queued = hub.has_queued_scan();
            const bool scan_active = job_busy && hub.job == HubJob::ScanRoms &&
                                     hub.job_platform_filter == plat;
            const bool scan_queued = hub.is_job_queued(HubJob::ScanRoms, {}, plat);
            char scan_label[96];
            if (scan_active)
                std::snprintf(scan_label, sizeof(scan_label), "Scanning…");
            else if (scan_queued)
                std::snprintf(scan_label, sizeof(scan_label), "Queued");
            else if (job_busy && !any_scan_queued)
                std::snprintf(scan_label, sizeof(scan_label), "Queue Rescan Library");
            else
                std::snprintf(scan_label, sizeof(scan_label), "Rescan Library");
            ImGui::BeginDisabled(tid.empty() || plat.empty() || scan_active || scan_queued ||
                                 any_scan_queued);
            if (good_button(scan_label, th, ImVec2(-1, 0))) {
                hub.scans_platform_filter = plat;
                hub.pending_scan_missing_rom_id = tid;
                hub.start_job(HubJob::ScanRoms);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            bool file_busy = false;
            {
                std::lock_guard<std::mutex> lock(hub.file_pick_mu);
                file_busy = hub.file_pick_busy;
            }
            ImGui::BeginDisabled(plat.empty() || file_busy || job_busy);
            if (ImGui::Button("Import ROM", ImVec2(-1, 0))) {
                const auto exts = rom_exts_for_platform(hub.catalog, plat);
                begin_file_pick(hub, window, retcomm::hub::FilePickKind::ImportRom, plat,
                                "ROM files", exts, /*allow_many=*/true, tid);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            const bool inst_active =
                job_busy && hub.job == HubJob::Install && hub.job_title_id == tid;
            const bool inst_queued = hub.is_job_queued(HubJob::Install, tid);
            const char* romm_lbl = inst_active  ? "Installing…"
                                   : inst_queued ? "Queued"
                                   : job_busy    ? "Queue Download from RomM"
                                                 : "Download from RomM";
            ImGui::BeginDisabled(!romm_download_ok || inst_active || inst_queued);
            if (romm_button(romm_lbl, th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::Install, tid, false, true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (!romm_download_ok &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                    ImGuiHoveredFlags_DelayNormal)) {
                if (!romm_sync_ok)
                    ImGui::SetTooltip("Configure RomM Sync Settings to enable download.");
                else
                    ImGui::SetTooltip("This title has no catalog ROM identity for RomM match.");
            }
            if (ImGui::Button("Cancel", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        if (hub.open_rom_folder_prompt_pending.exchange(false))
            ImGui::OpenPopup("ROM still missing###open_rom_folder_prompt");
        if (ImGui::BeginPopupModal("ROM still missing###open_rom_folder_prompt", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            std::string tid, plat;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                tid = hub.open_rom_folder_prompt_id;
                plat = hub.open_rom_folder_prompt_platform;
            }
            const TitleRow* prow = nullptr;
            for (const auto& r : hub.rows) {
                if (r.id == tid) {
                    prow = &r;
                    break;
                }
            }
            if (plat.empty() && prow) plat = prow->platform;
            const char* plat_label = plat.empty() ? "this platform" : platform_display_name(plat);
            const bool romm_sync_ok = prow && prow->romm_ready;
            const bool romm_download_ok =
                romm_sync_ok && prow->has_rom_identity && !tid.empty();
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped("%s", prow ? prow->name.c_str() : tid.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextWrapped(
                "Still no verified dump for this title under %s. Rescan again, import the "
                "files, or download from RomM when sync is configured.",
                plat_label);
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            const bool job_busy = hub.job_running.load();
            const bool any_scan_queued = hub.has_queued_scan();
            const bool scan_active = job_busy && hub.job == HubJob::ScanRoms &&
                                     hub.job_platform_filter == plat;
            const bool scan_queued = hub.is_job_queued(HubJob::ScanRoms, {}, plat);
            char scan_label[96];
            if (scan_active)
                std::snprintf(scan_label, sizeof(scan_label), "Scanning…");
            else if (scan_queued)
                std::snprintf(scan_label, sizeof(scan_label), "Queued");
            else if (job_busy && !any_scan_queued)
                std::snprintf(scan_label, sizeof(scan_label), "Queue Rescan Library");
            else
                std::snprintf(scan_label, sizeof(scan_label), "Rescan Library");
            ImGui::BeginDisabled(tid.empty() || plat.empty() || scan_active || scan_queued ||
                                 any_scan_queued);
            if (good_button(scan_label, th, ImVec2(-1, 0))) {
                hub.scans_platform_filter = plat;
                hub.pending_scan_missing_rom_id = tid;
                hub.start_job(HubJob::ScanRoms);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            bool file_busy = false;
            {
                std::lock_guard<std::mutex> lock(hub.file_pick_mu);
                file_busy = hub.file_pick_busy;
            }
            ImGui::BeginDisabled(plat.empty() || file_busy || job_busy);
            if (ImGui::Button("Import ROM", ImVec2(-1, 0))) {
                const auto exts = rom_exts_for_platform(hub.catalog, plat);
                begin_file_pick(hub, window, retcomm::hub::FilePickKind::ImportRom, plat,
                                "ROM files", exts, /*allow_many=*/true, tid);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            const bool inst_active =
                job_busy && hub.job == HubJob::Install && hub.job_title_id == tid;
            const bool inst_queued = hub.is_job_queued(HubJob::Install, tid);
            const char* romm_lbl = inst_active  ? "Installing…"
                                   : inst_queued ? "Queued"
                                   : job_busy    ? "Queue Download from RomM"
                                                 : "Download from RomM";
            ImGui::BeginDisabled(!romm_download_ok || inst_active || inst_queued);
            if (romm_button(romm_lbl, th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::Install, tid, false, true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (!romm_download_ok &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                    ImGuiHoveredFlags_DelayNormal)) {
                if (!romm_sync_ok)
                    ImGui::SetTooltip("Configure RomM Sync Settings to enable download.");
                else
                    ImGui::SetTooltip("This title has no catalog ROM identity for RomM match.");
            }
            if (ImGui::Button("Cancel", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        if (hub.launch_update_prompt_pending.exchange(false))
            ImGui::OpenPopup("Update available###launch_update_prompt");
        if (ImGui::BeginPopupModal("Update available###launch_update_prompt", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            std::string tid, from, to, name;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                tid = hub.launch_update_prompt_id;
                from = hub.launch_update_from;
                to = hub.launch_update_to;
            }
            for (const auto& r : hub.rows) {
                if (r.id == tid) {
                    name = r.name;
                    break;
                }
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped("%s", name.empty() ? tid.c_str() : name.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextWrapped(
                "A newer release is available.\n\nInstalled: %s\nLatest: %s\n\n"
                "Update before playing?",
                from.empty() ? "?" : from.c_str(), to.empty() ? "?" : to.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            const bool job_busy = hub.job_running.load();
            const bool launch_busy = hub.launch_running.load();
            // Update needs the main worker — disabled while Install/etc. runs.
            ImGui::BeginDisabled(job_busy || launch_busy || tid.empty());
            if (good_button("Update", th, ImVec2(-1, 0))) {
                hub.start_job(HubJob::Update, tid);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (job_busy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Wait for the current install/job to finish, or play "
                                  "without updating.");
            }
            // Play uses launch_worker — OK while another title installs.
            ImGui::BeginDisabled(launch_busy || tid.empty());
            if (ImGui::Button("Play Without Updating", ImVec2(-1, 0))) {
                hub.start_job(HubJob::Launch, tid);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Cancel", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        // Update prompts in order: launcher → toolchain → games.
        // Avoid opening the next modal on the same frame (IsPopupOpen can lag OpenPopup).
        const bool open_launcher_update = hub.launcher_update_prompt_pending.exchange(false);
        if (open_launcher_update) ImGui::OpenPopup("Retro update###launcher_update");
        if (ImGui::BeginPopupModal("Retro update###launcher_update", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            std::string cur, latest;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                cur = hub.launcher_current_version;
                latest = hub.launcher_latest_tag;
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.f);
            ImGui::TextWrapped(
                "A newer Retro Launcher release is available.\n\n"
                "Installed: %s\nLatest: %s\n\n"
                "Update now? The app will download the package and restart.",
                cur.empty() ? "?" : cur.c_str(),
                latest.empty() ? "?" : latest.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            // Never gate these on job_running — CheckUpdates used to arm this modal
            // mid-job and left Update greyed out for the whole game/toolchain scan.
            if (accent_button("Update Retro", th, ImVec2(160, 0))) {
                hub.cancel_prefetch_updates();
                hub.discard_followup_update_prompts();
                hub.start_job(HubJob::SelfUpdate);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Later", ImVec2(120, 0))) {
                hub.release_deferred_followup_updates();
                ImGui::CloseCurrentPopup();
            }
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        const bool launcher_self_updating =
            hub.request_exit.load() ||
            (hub.job_running.load() && hub.job == HubJob::SelfUpdate);
        // Outside-click dismiss is "Later" — release deferred toolchain/game prompts.
        {
            static bool launcher_modal_was_open = false;
            const bool launcher_modal_open =
                open_launcher_update || ImGui::IsPopupOpen("Retro update###launcher_update");
            if (launcher_modal_was_open && !launcher_modal_open && !launcher_self_updating)
                hub.release_deferred_followup_updates();
            launcher_modal_was_open = launcher_modal_open;
        }
        const bool launcher_blocking =
            launcher_self_updating || open_launcher_update ||
            ImGui::IsPopupOpen("Retro update###launcher_update") ||
            hub.launcher_update_prompt_pending.load();

        const bool open_toolchain_update =
            !launcher_blocking && hub.toolchain_prompt_pending.exchange(false);
        if (open_toolchain_update) ImGui::OpenPopup("Toolchain update###toolchain_update");
        if (ImGui::BeginPopupModal("Toolchain update###toolchain_update", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            std::string cur, latest;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                cur = hub.toolchain_current_version;
                latest = hub.toolchain_latest_tag;
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.f);
            ImGui::TextWrapped(
                "A newer portable toolchain (cmake-clang-v1) is available.\n\n"
                "Installed: %s\nLatest: %s\n\n"
                "Update now? Builds that use the shared Retro toolchain cache "
                "will pick up the new pack.",
                cur.empty() ? "?" : cur.c_str(),
                latest.empty() ? "?" : latest.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy);
            if (accent_button("Update Toolchain", th, ImVec2(160, 0))) {
                hub.start_job(HubJob::UpdateToolchain);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Later", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndDisabled();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        const bool toolchain_blocking =
            open_toolchain_update || ImGui::IsPopupOpen("Toolchain update###toolchain_update") ||
            hub.toolchain_prompt_pending.load() ||
            (hub.job_running.load() && hub.job == HubJob::UpdateToolchain);

        const bool open_game_updates =
            !launcher_blocking && !toolchain_blocking &&
            hub.game_updates_prompt_pending.exchange(false);
        if (open_game_updates) ImGui::OpenPopup("Game updates###game_updates");
        if (ImGui::BeginPopupModal("Game updates###game_updates", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            int n = 0;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                n = hub.game_updates_prompt_count;
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.f);
            if (n == 1) {
                ImGui::TextWrapped(
                    "1 installed game has an update available.\n\n"
                    "Queue it to download/install when the worker is free, or dismiss "
                    "for now.");
            } else {
                ImGui::TextWrapped(
                    "%d installed games have updates available.\n\n"
                    "Queue all updates to run one after another, or dismiss for now.",
                    n);
            }
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10));
            if (good_button("Queue All Updates", th, ImVec2(-1, 0))) {
                hub.queue_all_updates();
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::Button("Not Right Now", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        if (hub.orphan_prompt_pending.exchange(false))
            ImGui::OpenPopup("Unlisted installs###orphan_cleanup");
        if (ImGui::BeginPopupModal("Unlisted installs###orphan_cleanup", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            std::vector<retcomm::OrphanInstall> orphans;
            {
                std::lock_guard<std::mutex> lock(hub.mu);
                orphans = hub.pending_orphans;
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 420.f);
            ImGui::TextWrapped(
                "These local installs are no longer in the catalog. Remove them to free "
                "disk space? Library ROMs and managed saves_root files are not deleted.");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 8));
            ImGui::BeginChild("orphan_list", ImVec2(420, 140), ImGuiChildFlags_Borders);
            for (const auto& o : orphans) {
                std::string line = o.title_id;
                if (o.title_id != o.dir_name) line += " (" + o.dir_name + ")";
                if (!o.tag.empty()) line += " @" + o.tag;
                if (o.has_preserved_only) line += " [preserved saves only]";
                ImGui::BulletText("%s", line.c_str());
            }
            if (orphans.empty()) ImGui::TextDisabled("(none)");
            ImGui::EndChild();
            ImGui::Dummy(ImVec2(0, 10));
            const bool busy = hub.job_running.load();
            ImGui::BeginDisabled(busy || orphans.empty());
            if (accent_button("Remove (keep saves)", th, ImVec2(180, 0))) {
                hub.start_job(HubJob::CleanupOrphans);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove + delete saves", ImVec2(180, 0))) {
                hub.start_job(HubJob::CleanupOrphansPurge);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Keep", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
            close_modal_on_outside_click();
            ImGui::EndPopup();
        }

        ImGui::End();
        } // !playing

        // Keep any spawned keyboard in step with ImGui's text-input state. The
        // hint SDL reads inside SDL_StartTextInput() is set earlier, when the
        // pad arms the field, because EndFrame() (inside Render) is where the
        // backend makes that call.
        {
            const std::string osk_note =
                retcomm::hub::osk_sync(window, ImGui::GetIO().WantTextInput);
            if (!osk_note.empty()) hub.append_log(osk_note);
        }

        ImGui::Render();
        int fb_w = 0, fb_h = 0;
        SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(th.background.x, th.background.y, th.background.z, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

#if defined(RETCOMM_HUB_HAVE_PLAY)
    // Before the GL context goes: the session owns a texture and a runner.
    play.reset();
#endif

    // Self-update / hard-reset apply scripts wait on this PID. Prefer a fast
    // exit over a graceful join that can hang on prefetch/launch workers and
    // leave the apply bat stuck until timeout.
    if (hub.request_exit.load()) {
        hub.cancel_prefetch_updates();
        if (hub.prefetch_worker.joinable()) hub.prefetch_worker.detach();
        if (hub.worker.joinable()) hub.worker.detach();
        if (hub.launch_worker.joinable()) hub.launch_worker.detach();
        if (hub.steam_worker.joinable()) hub.steam_worker.detach();
        hub_close_all_gamepads();
        SDL_Quit();
#if defined(_WIN32)
        ::ExitProcess(0);
#else
        std::_Exit(0);
#endif
    }

    hub.join_worker();
    boxart.destroy_all();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    hub_close_all_gamepads();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
