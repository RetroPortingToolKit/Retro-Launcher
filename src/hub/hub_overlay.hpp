#pragma once

// The overlay look: a flat, semi-transparent dark layer over what is behind it,
// a top bar, a rail of choices on the left, panels of cards, and a legend of
// button hints along the bottom. Drawing helpers only -- no hub state, so the
// library menu (hub_main.cpp) and the in-game quick menu (hub_play.cpp) share
// them and look the same.
//
// The design is the "Retro Overlay Redesign" canvas: one surface, summoned
// with L3 + R3 held, controller first, touch usable.

#include "imgui.h"

#include <SDL3/SDL_events.h>

#include <string>
#include <vector>

namespace retcomm::hub::overlay {

// Whether the hub draws its menus as the overlay. On by default on Android;
// RETCOMM_OVERLAY_UI=1 or 0 overrides it on any platform. Read once.
bool enabled();

// Sizes for this display. A short screen (a phone in landscape) gets a smaller
// top bar, legend and rows.
struct Metrics {
    float top = 64.f;
    float legend = 68.f;
    float rail_w = 312.f;
    float item_h = 54.f;
    float pad = 32.f;
    bool compact = false;
};
Metrics metrics(const ImVec2& display);

struct RailItem {
    std::string label;
    std::string value;        // muted, right-aligned
    bool danger = false;      // Close game, Leave room
    bool enabled = true;
    bool divider_before = false;
};

struct Hint {
    std::string key;          // "A", "B", "Start", "L3 + R3"
    std::string what;
};

// A borderless window over the whole viewport, filled with the flat scrim
// (alpha follows `t`, 0..1, for the open/close fade). Always pair with end().
// `solid` makes the scrim opaque enough to hide a busy page behind it.
void begin(const char* id, float t, bool solid = false);
void end();

// The top bar: the mark, "CRUMB  sub" on the left, battery and clock on the
// right. `status` is an extra pill ("Paused", "Live 24 ms"), empty for none.
void top_bar(const Metrics& m, const std::string& crumb, const std::string& sub,
             const std::string& status = {});

// The rail. Returns the index activated this frame (A, Enter, or a tap), or -1.
// `focus_first` asks the nav ring to land on the first enabled item (set it on
// the frame the overlay opens); it is cleared once applied.
int rail(const Metrics& m, const char* id, const std::vector<RailItem>& items,
         bool* focus_first);

// A panel of cards to the right of the rail, from x to the right edge. Always
// pair with end_panel() when it returns true.
bool begin_panel(const Metrics& m, const char* id, float x);
void end_panel();

// Cards inside a panel. One card per begin_card/end_card; `eyebrow` is the
// small heading.
void begin_card(const char* id, const char* eyebrow, float width = 0.f);
void end_card();
void kv(const char* label, const std::string& value, bool good = false);
void muted(const char* text);              // wrapped muted text
void banner(const char* text);             // amber notice across a panel

// The legend along the bottom edge.
void legend(const Metrics& m, const std::vector<Hint>& left, const std::vector<Hint>& right);

// "82%  9:41", or "9:41" when there is no battery.
std::string clock_text();

// True the frame the two stick clicks of any pad have been held together for
// `hold_ns` (400 ms by default). Feed it every SDL event; it fires once per
// press of the chord.
class ChordHold {
public:
    explicit ChordHold(unsigned long long hold_ns = 400'000'000ull) : hold_ns_(hold_ns) {}
    void on_event(const SDL_Event& e);
    bool poll();
private:
    unsigned long long hold_ns_;
    unsigned long long since_ns_ = 0;
    bool l3_ = false, r3_ = false, fired_ = false;
};

}  // namespace retcomm::hub::overlay
