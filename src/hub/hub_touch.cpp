#include "hub/hub_touch.hpp"

#include "imgui.h"
#include "imgui_internal.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace retcomm::hub {
namespace {

// Logical units a finger has to travel before a press becomes a drag: about
// Android's 8dp touch slop at the hub's Android scale.
constexpr float kSlop = 10.f;
// Held this long without leaving the slop, a press is a press (long-press).
constexpr Uint64 kLongPressNs = 400'000'000;
// A release slower than this (logical units / s) just stops; faster flings.
constexpr float kFlingMin = 120.f;
constexpr float kFlingStop = 20.f;
// Exponential decay of the fling speed, per second.
constexpr float kFlingFriction = 5.f;
// Space kept between the edited text line and the top of the keyboard.
constexpr float kImeMargin = 16.f;

enum class Gesture {
    Idle,
    Pending,      // finger down inside the slop: nothing has reached ImGui yet
    Scrolling,    // dragged past the slop over a scrollable window
    Passthrough,  // an ordinary press: ImGui has it and sees the finger move
};

struct DragScroll {
    Gesture state = Gesture::Idle;
    Uint64 press_ns = 0;
    ImVec2 press{};   // where the finger went down (ImGui coordinates)
    ImVec2 finger{};  // where it is now
    bool released = false;  // lifted mid-scroll; touch_frame() settles it
    ImGuiID window = 0;
    ImGuiAxis axis = ImGuiAxis_Y;
    float origin = 0.f;       // finger position on `axis` when the scroll began
    float base_scroll = 0.f;  // window scroll on `axis` when the scroll began
    float scroll = 0.f;       // last scroll position we asked for
    float velocity = 0.f;     // scroll units / s, smoothed
    bool flinging = false;
};

DragScroll g_drag;
// Logical units the frame is slid up by for the keyboard (touch_before_render).
float g_pan = 0.f;
// On-screen keyboard height, logical units, for the frame being built.
float g_ime_h = 0.f;

#if defined(__ANDROID__)
// Pixels, from LauncherActivity's insets listener on the UI thread.
std::atomic<int> g_ime_px{0};
#endif

// The window a press at `pos` lands in, as ImGui would hover it: nothing when a
// modal blocks it.
ImGuiWindow* window_at(ImVec2 pos) {
    ImGuiWindow* hovered = nullptr;
    ImGuiWindow* under_moving = nullptr;
    ImGui::FindHoveredWindowEx(pos, false, &hovered, &under_moving);
    if (ImGuiWindow* modal = ImGui::GetTopMostAndVisiblePopupModal())
        if (hovered && !ImGui::IsWindowWithinBeginStackOf(hovered, modal)) return nullptr;
    return hovered;
}

// The window itself, or the nearest parent a child window would hand a mouse
// wheel to, that can scroll on `axis`.
ImGuiWindow* scrollable_ancestor(ImGuiWindow* w, ImGuiAxis axis) {
    for (; w; w = w->ParentWindow) {
        if (w->ScrollMax[axis] > 0.f && !(w->Flags & ImGuiWindowFlags_NoScrollWithMouse)) return w;
        if (!(w->Flags & ImGuiWindowFlags_ChildWindow)) return nullptr;
    }
    return nullptr;
}

void set_scroll(ImGuiWindow* w, ImGuiAxis axis, float v) {
    if (axis == ImGuiAxis_X) ImGui::SetScrollX(w, v);
    else ImGui::SetScrollY(w, v);
}

// Hand the held-back press to ImGui where it happened, then catch it up with
// the finger. ImGui's input trickling spreads these over frames in order.
void deliver_press(ImGuiIO& io) {
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(g_drag.press.x, g_drag.press.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    if (g_drag.finger.x != g_drag.press.x || g_drag.finger.y != g_drag.press.y)
        io.AddMousePosEvent(g_drag.finger.x, g_drag.finger.y);
    g_drag.state = Gesture::Passthrough;
}

// A lifted finger is not hovering anything: without this the last item touched
// stays highlighted, and its tooltip stays up, until the next touch.
void park_cursor(ImGuiIO& io) {
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

void begin_scroll_if_dragged(ImGuiIO& io) {
    const ImVec2 d(g_drag.finger.x - g_drag.press.x, g_drag.finger.y - g_drag.press.y);
    if (std::fabs(d.x) < kSlop && std::fabs(d.y) < kSlop) return;
    const ImGuiAxis axis = std::fabs(d.y) >= std::fabs(d.x) ? ImGuiAxis_Y : ImGuiAxis_X;
    ImGuiWindow* w = scrollable_ancestor(window_at(g_drag.press), axis);
    if (!w) {
        deliver_press(io);
        return;
    }
    g_drag.state = Gesture::Scrolling;
    g_drag.window = w->ID;
    g_drag.axis = axis;
    g_drag.origin = g_drag.finger[axis];
    g_drag.base_scroll = w->Scroll[axis];
    g_drag.scroll = g_drag.base_scroll;
    g_drag.velocity = 0.f;
    park_cursor(io);  // the content moves; the press point should not light up
}

void run_scroll(ImGuiIO& io) {
    ImGuiWindow* w = ImGui::FindWindowByID(g_drag.window);
    if (!w) {
        g_drag.state = Gesture::Idle;
        return;
    }
    const ImGuiAxis axis = g_drag.axis;
    const float next = std::clamp(g_drag.base_scroll - (g_drag.finger[axis] - g_drag.origin), 0.f,
                                  w->ScrollMax[axis]);
    set_scroll(w, axis, next);
    if (io.DeltaTime > 0.f)
        g_drag.velocity = g_drag.velocity * 0.6f + (next - g_drag.scroll) / io.DeltaTime * 0.4f;
    g_drag.scroll = next;
    if (g_drag.released) {
        g_drag.released = false;
        g_drag.state = Gesture::Idle;
        g_drag.flinging = std::fabs(g_drag.velocity) >= kFlingMin;
    }
}

void run_fling(ImGuiIO& io) {
    ImGuiWindow* w = ImGui::FindWindowByID(g_drag.window);
    if (!w) {
        g_drag.flinging = false;
        return;
    }
    const float max = w->ScrollMax[g_drag.axis];
    const float next = std::clamp(g_drag.scroll + g_drag.velocity * io.DeltaTime, 0.f, max);
    set_scroll(w, g_drag.axis, next);
    g_drag.velocity *= std::exp(-kFlingFriction * io.DeltaTime);
    if (next == g_drag.scroll || std::fabs(g_drag.velocity) < kFlingStop)
        g_drag.flinging = false;
    g_drag.scroll = next;
}

}  // namespace

void touch_configure_hints() {
#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
}

bool touch_translate_event(SDL_Event& e, bool ui_has_pointer) {
    if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) && e.key.key == SDLK_AC_BACK) {
        e.key.key = SDLK_ESCAPE;
        e.key.scancode = SDL_SCANCODE_ESCAPE;
        return false;
    }

    ImVec2 pos;
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (e.motion.which != SDL_TOUCH_MOUSEID) return false;
        pos = ImVec2(e.motion.x, e.motion.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.which != SDL_TOUCH_MOUSEID || e.button.button != SDL_BUTTON_LEFT) return false;
        pos = ImVec2(e.button.x, e.button.y);
        break;
    default:
        return false;
    }
    if (!ui_has_pointer) {
        // A game owns the window: its events, untouched.
        g_drag = DragScroll{};
        return false;
    }
    pos.y += g_pan;  // the screen shows ImGui's frame slid up by g_pan

    ImGuiIO& io = ImGui::GetIO();
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        g_drag.flinging = false;
        g_drag.released = false;
        g_drag.state = Gesture::Pending;
        g_drag.press_ns = e.button.timestamp;
        g_drag.press = g_drag.finger = pos;
        // Hover where the finger is, so the item about to be tapped lights up.
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(pos.x, pos.y);
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        g_drag.finger = pos;
        if (g_drag.state == Gesture::Pending) {
            begin_scroll_if_dragged(io);
        } else if (g_drag.state != Gesture::Scrolling) {
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(pos.x, pos.y);
        }
        return true;
    }
    // Lifted.
    g_drag.finger = pos;
    switch (g_drag.state) {
    case Gesture::Pending:  // a tap
        deliver_press(io);
        [[fallthrough]];
    case Gesture::Passthrough:
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        g_drag.state = Gesture::Idle;
        break;
    case Gesture::Scrolling:
        g_drag.released = true;
        break;
    case Gesture::Idle:
        break;
    }
    park_cursor(io);
    return true;
}

void touch_prepare_frame(SDL_Window* window, float coords) {
    int w = 0, h = 0, px_h = 0;
    if (!SDL_GetWindowSize(window, &w, &h) || !SDL_GetWindowSizeInPixels(window, nullptr, &px_h))
        return;
    SDL_Rect safe{};
    if (SDL_GetWindowSafeArea(window, &safe) && safe.w > 0 && safe.h > 0 && coords > 0.f) {
        auto* vp = static_cast<ImGuiViewportP*>(ImGui::GetMainViewport());
        vp->BuildWorkInsetMin.x += static_cast<float>(safe.x) / coords;
        vp->BuildWorkInsetMin.y += static_cast<float>(safe.y) / coords;
        vp->BuildWorkInsetMax.x += static_cast<float>(w - safe.x - safe.w) / coords;
        vp->BuildWorkInsetMax.y += static_cast<float>(h - safe.y - safe.h) / coords;
    }
#if defined(__ANDROID__)
    // io.DisplaySize is already the logical size for this frame.
    const float logical_per_px = px_h > 0 ? ImGui::GetIO().DisplaySize.y / px_h : 0.f;
    g_ime_h = static_cast<float>(g_ime_px.load(std::memory_order_relaxed)) * logical_per_px;
#endif
}

void touch_apply_style(ImGuiStyle& style) {
#if defined(__ANDROID__)
    // Reach a little past each item's drawn edge: neighbours sit ItemSpacing
    // (8) apart, so 4 a side meets in the middle without overlapping.
    style.TouchExtraPadding = ImVec2(4.f, 4.f);
#else
    (void)style;
#endif
}

void touch_frame() {
    ImGuiIO& io = ImGui::GetIO();
    if (g_drag.flinging) run_fling(io);
    if (g_drag.state == Gesture::Scrolling) run_scroll(io);
    if (g_drag.state == Gesture::Pending && SDL_GetTicksNS() - g_drag.press_ns >= kLongPressNs)
        deliver_press(io);
}

void touch_before_render(ImDrawData* draw_data) {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiPlatformImeData& ime = g.PlatformImeData;  // this frame's, until NewFrame
    float pan = 0.f;
    if (draw_data && g_ime_h > 0.f && ime.WantVisible) {
        const float visible_bottom = draw_data->DisplaySize.y - g_ime_h;
        pan = std::max(0.f, ime.InputPos.y + ime.InputLineHeight + kImeMargin - visible_bottom);
    }
    g_pan = pan;
    // The GL backend projects DisplayPos..DisplayPos+DisplaySize onto the
    // framebuffer and offsets scissor rects by DisplayPos, so this one shift
    // moves every vertex and clip rect together.
    if (draw_data) draw_data->DisplayPos.y += pan;
}

}  // namespace retcomm::hub

#if defined(__ANDROID__)
// LauncherActivity.nativeImeInset: the keyboard's bottom inset in pixels, 0
// when it is hidden.
extern "C" JNIEXPORT void JNICALL
Java_org_retroportingtoolkit_launcher_LauncherActivity_nativeImeInset(JNIEnv*, jclass, jint bottom) {
    retcomm::hub::g_ime_px.store(bottom > 0 ? bottom : 0, std::memory_order_relaxed);
}
#endif
