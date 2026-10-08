#pragma once

union SDL_Event;
struct SDL_Window;
struct ImDrawData;
struct ImGuiStyle;

namespace retcomm::hub {

// Touchscreen behaviour the hub's ImGui UI does not get from SDL's emulated
// mouse on its own. Everything here keys off SDL_TOUCH_MOUSEID events or an
// Android-only signal, so a desktop mouse sees no difference.
//
// A touch press is held back until the gesture says what it is: lifted inside
// the slop it is a tap (press and release reach ImGui together); dragged past
// the slop over a window that scrolls that way it scrolls the window, and the
// widget under the finger never sees it; held still, or dragged where nothing
// scrolls, it becomes an ordinary press, so sliders, drag-and-drop and
// long-press tooltips still work.

// Before SDL_Init: keep Android's Back button from finishing the activity, and
// hold the hub to landscape (SDL otherwise requests every orientation for a
// resizable window, overriding the manifest).
void touch_configure_hints();

// Every SDL event, after scale_mouse_event() and before ImGui or a play
// session sees it. Android's Back key becomes Escape, so it backs out of
// whatever B / Escape already backs out of. When `ui_has_pointer` (no game
// owns the window), touch mouse events are fed to ImGui here; returns true
// when the event was consumed and must not reach ImGui's SDL backend.
bool touch_translate_event(SDL_Event& e, bool ui_has_pointer);

// After the frame's scale is installed, before ImGui::NewFrame(): shrink the
// main viewport's work area to the window's safe area (status bar, camera
// cutout) and note the on-screen keyboard's height. `coords` is SDL window
// coordinates per logical unit.
void touch_prepare_frame(SDL_Window* window, float coords);

// Larger reactive boxes on Android, where a fingertip is the pointer.
void touch_apply_style(ImGuiStyle& style);

// After ImGui::NewFrame(), before any window is submitted: drives the drag
// scroll and its fling, and turns a long hold into a press.
void touch_frame();

// After ImGui::Render(), before the draw data is rendered: while the on-screen
// keyboard would cover the text field being edited, slide the whole frame up
// far enough to show it (Android's adjustPan), without relayout.
void touch_before_render(ImDrawData* draw_data);

}  // namespace retcomm::hub
