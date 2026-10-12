#include "hub/hub_overlay.hpp"

#include "imgui_internal.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace retcomm::hub::overlay {
namespace {

// The shell's body this frame (begin_body), for where the ring lands first.
ImRect g_nav_body(0.f, 0.f, 0.f, 0.f);

// The canvas's palette: near-black glass, violet selection, cyan focus.
constexpr ImU32 kPanelBg = IM_COL32(8, 10, 14, 200);
constexpr ImU32 kCardBg = IM_COL32(255, 255, 255, 13);
constexpr ImU32 kLine = IM_COL32(255, 255, 255, 26);
constexpr ImU32 kText = IM_COL32(233, 237, 248, 255);
constexpr ImU32 kMuted = IM_COL32(153, 163, 188, 255);
constexpr ImU32 kAccent = IM_COL32(139, 92, 246, 245);
constexpr ImU32 kAccent2 = IM_COL32(109, 63, 224, 225);
constexpr ImU32 kFocus = IM_COL32(34, 229, 240, 255);
constexpr ImU32 kGood = IM_COL32(52, 211, 153, 255);
constexpr ImU32 kWarn = IM_COL32(245, 165, 36, 255);
constexpr ImU32 kBad = IM_COL32(255, 154, 163, 255);

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

}  // namespace

bool enabled() {
    static const bool on = [] {
        if (const char* e = std::getenv("RETCOMM_OVERLAY_UI"); e && *e) return e[0] != '0';
#if defined(__ANDROID__)
        return true;
#else
        return false;
#endif
    }();
    return on;
}

Metrics metrics(const ImVec2& display) {
    Metrics m;
    m.compact = display.y < 640.f;
    if (m.compact) {
        m.top = 52.f;
        m.legend = 52.f;
        m.item_h = 46.f;
        m.pad = 20.f;
    }
    m.rail_w = std::clamp(display.x * 0.28f, 220.f, 312.f);
    return m;
}

void begin(const char* id, float t, bool solid, bool keep_back) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    const float alpha = (solid ? 0.96f : 0.58f) * std::clamp(t, 0.f, 1.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, alpha));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse |
                     (keep_back ? ImGuiWindowFlags_NoBringToFrontOnFocus : 0));
    // Begin has taken what it needs from these; the pages and children drawn in
    // the window keep the hub's own padding, rounding and border.
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

void end() { ImGui::End(); }

std::string clock_text() {
    std::string out;
    int pct = -1;
    if (SDL_GetPowerInfo(nullptr, &pct) != SDL_POWERSTATE_NO_BATTERY && pct >= 0)
        out = std::to_string(pct) + "%  ";
    SDL_Time now = 0;
    SDL_DateTime dt{};
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &dt, true)) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d:%02d", dt.hour, dt.minute);
        out += buf;
    }
    return out;
}

namespace {
ImTextureID g_mark_tex = 0;
// The mark: the mascot image, rounded; a plain tile until it has loaded.
void draw_mark(ImDrawList* dl, ImVec2 a, float mk) {
    const ImVec2 b(a.x + mk, a.y + mk);
    if (g_mark_tex) dl->AddImageRounded(g_mark_tex, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, mk * 0.26f);
    else dl->AddRectFilled(a, b, kAccent, mk * 0.26f);
}
} // namespace

void set_mark_texture(ImTextureID tex) { g_mark_tex = tex; }

void top_bar(const Metrics& m, const std::string& crumb, const std::string& sub,
             const std::string& status) {
    const ImVec2 p0 = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + m.top), IM_COL32(0, 0, 0, 115));
    const float cy = p0.y + m.top * 0.5f;
    // The mark.
    const float mk = m.compact ? 28.f : 34.f;
    const ImVec2 a(p0.x + m.pad, cy - mk * 0.5f);
    draw_mark(dl, a, mk);
    // Crumb and subtitle.
    ImGui::SetWindowFontScale(1.15f);
    std::string up = crumb;
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const ImVec2 cs = ImGui::CalcTextSize(up.c_str());
    const float tx = a.x + mk + 14.f;
    dl->AddText(ImVec2(tx, cy - cs.y * 0.5f), kText, up.c_str());
    ImGui::SetWindowFontScale(1.f);
    if (!sub.empty()) {
        const ImVec2 ss = ImGui::CalcTextSize(sub.c_str());
        dl->AddText(ImVec2(tx + cs.x + 14.f, cy - ss.y * 0.5f), kMuted, sub.c_str());
    }
    // Right side: the status pill, then battery and clock.
    float rx = p0.x + w - m.pad;
    const std::string clk = clock_text();
    const ImVec2 cts = ImGui::CalcTextSize(clk.c_str());
    rx -= cts.x;
    dl->AddText(ImVec2(rx, cy - cts.y * 0.5f), kMuted, clk.c_str());
    if (!status.empty()) {
        const ImVec2 ss = ImGui::CalcTextSize(status.c_str());
        const float pw = ss.x + 26.f, ph = ss.y + 10.f;
        rx -= 16.f + pw;
        const ImVec2 b0(rx, cy - ph * 0.5f);
        dl->AddRectFilled(b0, ImVec2(b0.x + pw, b0.y + ph), IM_COL32(255, 255, 255, 18), ph * 0.5f);
        dl->AddRect(b0, ImVec2(b0.x + pw, b0.y + ph), kLine, ph * 0.5f);
        dl->AddText(ImVec2(b0.x + 13.f, b0.y + 5.f), kText, status.c_str());
    }
}

namespace {

// A dark rounded child behind rail and panel.
bool glass_child(const char* id, ImVec2 size, ImGuiChildFlags cf = 0,
                 ImGuiWindowFlags wf = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(kPanelBg));
    ImGui::PushStyleColor(ImGuiCol_Border, v4(kLine));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
    const bool open = ImGui::BeginChild(id, size, cf | ImGuiChildFlags_Borders, wf);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    return open;
}

}  // namespace

int rail(const Metrics& m, const char* id, const std::vector<RailItem>& items,
         bool* focus_first) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float h = vp->Size.y - m.top - m.legend - 12.f;
    ImGui::SetCursorScreenPos(ImVec2(vp->Pos.x + m.pad, vp->Pos.y + m.top + 6.f));
    int activated = -1;
    if (glass_child(id, ImVec2(m.rail_w, h), ImGuiChildFlags_NavFlattened,
                    ImGuiWindowFlags_NoScrollbar)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 4.f));
        bool first = true;
        for (size_t i = 0; i < items.size(); ++i) {
            const RailItem& it = items[i];
            if (it.divider_before) {
                ImGui::Dummy(ImVec2(0.f, 4.f));
                const ImVec2 p = ImGui::GetCursorScreenPos();
                dl->AddLine(ImVec2(p.x + 8.f, p.y), ImVec2(p.x + m.rail_w - 24.f, p.y), kLine);
                ImGui::Dummy(ImVec2(0.f, 6.f));
            }
            const float w = ImGui::GetContentRegionAvail().x;
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::PushID(static_cast<int>(i));
            if (first && it.enabled && focus_first && *focus_first) {
                ImGui::SetKeyboardFocusHere();
                ImGui::SetNavCursorVisible(true);
                *focus_first = false;
            }
            if (it.enabled) first = false;
            ImGui::BeginDisabled(!it.enabled);
            const bool pressed = nav_button("##item", ImVec2(w, m.item_h),
                                                        ImGuiButtonFlags_EnableNav);
            ImGui::EndDisabled();
            const bool hot = it.enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
            const ImVec2 p1(p0.x + w, p0.y + m.item_h);
            if (hot) {
                dl->AddRectFilled(p0, p1, kAccent, 10.f);
                if (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible)
                    dl->AddRect(p0, p1, kFocus, 10.f, 0, 2.f);
            }
            const ImU32 col = !it.enabled ? IM_COL32(153, 163, 188, 110)
                              : hot       ? IM_COL32(255, 255, 255, 255)
                              : it.danger ? kBad
                                          : IM_COL32(208, 215, 234, 255);
            const ImVec2 ts = ImGui::CalcTextSize(it.label.c_str());
            dl->AddText(ImVec2(p0.x + 18.f, p0.y + (m.item_h - ts.y) * 0.5f), col,
                        it.label.c_str());
            if (!it.value.empty()) {
                const ImVec2 vs = ImGui::CalcTextSize(it.value.c_str());
                dl->AddText(ImVec2(p1.x - 16.f - vs.x, p0.y + (m.item_h - vs.y) * 0.5f),
                            hot ? IM_COL32(255, 255, 255, 230) : kMuted, it.value.c_str());
            }
            if (pressed && it.enabled) activated = static_cast<int>(i);
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    return activated;
}

void nav_mark_tab(bool selected);

int tab_bar(const Metrics& m, const std::vector<std::string>& tabs, int selected, bool enabled,
            const std::string& status) {
    const ImVec2 p0 = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + m.top), IM_COL32(0, 0, 0, 115));
    const float cy = p0.y + m.top * 0.5f;
    const float mk = m.compact ? 28.f : 34.f;
    const ImVec2 a(p0.x + m.pad, cy - mk * 0.5f);
    draw_mark(dl, a, mk);

    int clicked = -1;
    const float h = m.compact ? 34.f : 40.f;
    float x = a.x + mk + 18.f;
    ImGui::PushID("##tabs");
    ImGui::BeginDisabled(!enabled);
    for (size_t i = 0; i < tabs.size(); ++i) {
        const ImVec2 ts = ImGui::CalcTextSize(tabs[i].c_str());
        const float tw = ts.x + (m.compact ? 28.f : 40.f);
        ImGui::SetCursorScreenPos(ImVec2(x, cy - h * 0.5f));
        ImGui::PushID(static_cast<int>(i));
        const bool tab_hit = nav_button("##tab", ImVec2(tw, h), ImGuiButtonFlags_EnableNav);
        const bool on = static_cast<int>(i) == selected;
        nav_mark_tab(on);
        if (tab_hit) clicked = static_cast<int>(i);
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        const ImVec2 b0 = ImGui::GetItemRectMin(), b1 = ImGui::GetItemRectMax();
        if (on) dl->AddRectFilled(b0, b1, kAccent, h * 0.5f);
        else dl->AddRectFilled(b0, b1, IM_COL32(255, 255, 255, hot ? 40 : 18), h * 0.5f);
        if (!on) dl->AddRect(b0, b1, kLine, h * 0.5f);
        if (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible)
            dl->AddRect(b0, b1, kFocus, h * 0.5f, 0, 2.f);
        dl->AddText(ImVec2(b0.x + (tw - ts.x) * 0.5f, b0.y + (h - ts.y) * 0.5f),
                    on ? IM_COL32(255, 255, 255, 255) : IM_COL32(208, 215, 234, 255),
                    tabs[i].c_str());
        ImGui::PopID();
        x += tw + 8.f;
    }
    ImGui::EndDisabled();
    ImGui::PopID();

    // Right side: the clock, and a job line left of it.
    float rx = p0.x + w - m.pad;
    const std::string clk = clock_text();
    const ImVec2 cts = ImGui::CalcTextSize(clk.c_str());
    rx -= cts.x;
    dl->AddText(ImVec2(rx, cy - cts.y * 0.5f), kMuted, clk.c_str());
    if (!status.empty()) {
        std::string shown = status;
        const float room = rx - 16.f - x;
        while (shown.size() > 4 && ImGui::CalcTextSize(shown.c_str()).x > room) shown.pop_back();
        if (room > 80.f) {
            const ImVec2 ss = ImGui::CalcTextSize(shown.c_str());
            dl->AddText(ImVec2(rx - 16.f - ss.x, cy - ss.y * 0.5f), kWarn, shown.c_str());
        }
    }
    return clicked;
}

void begin_body(const Metrics& m, const char* id) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = vp->Size.x - m.pad * 2.f;
    const float h = vp->Size.y - m.top - m.legend - 12.f;
    g_nav_body = ImRect(vp->Pos.x + m.pad, vp->Pos.y + m.top + 6.f, vp->Pos.x + m.pad + w, vp->Pos.y + m.top + 6.f + h);
    ImGui::SetCursorScreenPos(ImVec2(vp->Pos.x + m.pad, vp->Pos.y + m.top + 6.f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
}

void end_body() { ImGui::EndChild(); }

bool list_row(const Metrics& m, const char* id, const char* label, const std::string& value,
              bool enabled) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = m.item_h + 8.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = nav_button("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool hot = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    const ImVec2 p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, hot ? IM_COL32(139, 92, 246, 150) : kCardBg, 10.f);
    dl->AddRect(p0, p1, kLine, 10.f);
    if (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible)
        dl->AddRect(p0, p1, kFocus, 10.f, 0, 2.f);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p0.x + 18.f, p0.y + (h - ts.y) * 0.5f),
                enabled ? kText : IM_COL32(153, 163, 188, 110), label);
    const ImVec2 cs = ImGui::CalcTextSize(">");
    dl->AddText(ImVec2(p1.x - 18.f - cs.x, p0.y + (h - cs.y) * 0.5f), kMuted, ">");
    if (!value.empty()) {
        const ImVec2 vs = ImGui::CalcTextSize(value.c_str());
        dl->AddText(ImVec2(p1.x - 36.f - cs.x - vs.x, p0.y + (h - vs.y) * 0.5f), kMuted,
                    value.c_str());
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.f, 6.f));
    return pressed && enabled;
}

void card_frame(ImVec2 a, ImVec2 b, bool hot, bool ring, float rounding) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, hot ? IM_COL32(139, 92, 246, 120) : kCardBg, rounding);
    dl->AddRect(a, b, kLine, rounding);
    if (ring) dl->AddRect(a, b, kFocus, rounding, 0, 2.5f);
}

namespace {

std::string g_row_info;

// Rows are one line: the description of the row in focus is read from
// row_info() and shown by the page in a strip, rather than under every row.
float row_height(const Metrics& m) { return m.item_h + 8.f; }

void draw_row_label(ImDrawList* dl, ImVec2 p0, float h, const char* label, bool enabled) {
    const ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p0.x + 18.f, p0.y + (h - ls.y) * 0.5f), enabled ? kText : IM_COL32(153, 163, 188, 110),
                label);
}

// "override", right-aligned to `right`, for a value that differs from the layer
// beneath it (one game's own value over every game's).
void draw_override_tag(ImDrawList* dl, float right, ImVec2 p0, float h) {
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * 0.74f;
    const char* t = "OVERRIDE";
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, t);
    const ImVec2 a(right - ts.x - 20.f, p0.y + (h - ts.y) * 0.5f - 4.f);
    const ImVec2 b(right, a.y + ts.y + 8.f);
    dl->AddRectFilled(a, b, IM_COL32(245, 165, 36, 36), 12.f);
    dl->AddRect(a, b, IM_COL32(245, 165, 36, 115), 12.f);
    dl->AddText(font, fs, ImVec2(a.x + 10.f, a.y + 4.f), IM_COL32(255, 212, 138, 255), t);
}

void note_info(bool hot, const char* description) {
    if (hot && description && *description) g_row_info = description;
}

} // namespace

const std::string& row_info() { return g_row_info; }
void row_info_reset() { g_row_info.clear(); }

bool setting_row(const Metrics& m, const char* id, const char* label, const std::string& value,
                 const char* description, bool enabled, bool overridden) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = row_height(m);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = nav_button("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool hot = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    note_info(hot, description);
    const ImVec2 p1(p0.x + w, p0.y + h);
    card_frame(p0, p1, hot, ImGui::IsItemFocused() && ImGui::GetIO().NavVisible);
    draw_row_label(dl, p0, h, label, enabled);
    const ImVec2 cs = ImGui::CalcTextSize(">");
    dl->AddText(ImVec2(p1.x - 18.f - cs.x, p0.y + (h - cs.y) * 0.5f), kMuted, ">");
    float left = p1.x - 36.f - cs.x;
    if (!value.empty()) {
        // A value too long for the row loses its start (the end of a path is the part
        // that says which one it is), so it never runs into the label or the chevron.
        const float value_w = std::min(w * 0.5f, ImGui::CalcTextSize(value.c_str()).x + 8.f);
        std::string shown = value;
        if (ImGui::CalcTextSize(shown.c_str()).x > value_w) {
            while (shown.size() > 4 && ImGui::CalcTextSize(("\xE2\x80\xA6" + shown).c_str()).x > value_w) {
                size_t cut = 1;
                while (cut < shown.size() && (static_cast<unsigned char>(shown[cut]) & 0xC0) == 0x80) ++cut;
                shown.erase(0, cut);
            }
            shown = "\xE2\x80\xA6" + shown;
        }
        const ImVec2 vs = ImGui::CalcTextSize(shown.c_str());
        dl->AddText(ImVec2(left - vs.x, p0.y + (h - vs.y) * 0.5f), hot ? kText : kMuted, shown.c_str());
        left -= vs.x;
    }
    if (overridden) draw_override_tag(dl, left - 14.f, p0, h);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.f, 6.f));
    return pressed && enabled;
}

void value_row(const Metrics& m, const char* id, const char* label, const std::string& value) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = row_height(m);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::Dummy(ImVec2(w, h));
    const ImVec2 p1(p0.x + w, p0.y + h);
    card_frame(p0, p1, false, false);
    draw_row_label(dl, p0, h, label, true);
    const ImVec2 vs = ImGui::CalcTextSize(value.c_str());
    dl->PushClipRect(ImVec2(p0.x + w * 0.35f, p0.y), p1, true);
    dl->AddText(ImVec2(std::max(p0.x + w * 0.35f, p1.x - 18.f - vs.x), p0.y + (h - vs.y) * 0.5f), kMuted,
                value.c_str());
    dl->PopClipRect();
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.f, 6.f));
}

void meter_row(const Metrics& m, const char* id, const char* label, float fraction, bool good,
               const std::string& text) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = row_height(m);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::Dummy(ImVec2(w, h));
    const ImVec2 p1(p0.x + w, p0.y + h);
    card_frame(p0, p1, false, false);
    draw_row_label(dl, p0, h, label, true);
    const float bar_w = std::min(w * 0.4f, 380.f);
    float right = p1.x - 18.f;
    if (!text.empty()) {
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        dl->AddText(ImVec2(right - ts.x, p0.y + (h - ts.y) * 0.5f), kMuted, text.c_str());
        right -= ts.x + 14.f;
    }
    const ImVec2 b0(right - bar_w, p0.y + h * 0.5f - 5.f), b1(right, p0.y + h * 0.5f + 5.f);
    dl->AddRectFilled(b0, b1, IM_COL32(255, 255, 255, 36), 5.f);
    const float f = std::clamp(fraction, 0.f, 1.f);
    if (f > 0.f)
        dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * f, b1.y), good ? kGood : kAccent, 5.f);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.f, 6.f));
}

bool toggle_row(const Metrics& m, const char* id, const char* label, bool* on,
                const char* description, bool overridden) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = row_height(m);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool pressed = nav_button("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    note_info(hot, description);
    const ImVec2 p1(p0.x + w, p0.y + h);
    card_frame(p0, p1, hot, ImGui::IsItemFocused() && ImGui::GetIO().NavVisible);
    draw_row_label(dl, p0, h, label, true);
    // The switch.
    constexpr float kTrackW = 52.f, kTrackH = 28.f;
    const ImVec2 t0(p1.x - 18.f - kTrackW, p0.y + (h - kTrackH) * 0.5f);
    const ImVec2 t1(t0.x + kTrackW, t0.y + kTrackH);
    dl->AddRectFilled(t0, t1, *on ? kAccent : IM_COL32(255, 255, 255, 40), kTrackH * 0.5f);
    const float knob_x = *on ? t1.x - kTrackH * 0.5f : t0.x + kTrackH * 0.5f;
    dl->AddCircleFilled(ImVec2(knob_x, t0.y + kTrackH * 0.5f), kTrackH * 0.5f - 3.f,
                        IM_COL32(255, 255, 255, 255));
    if (overridden) draw_override_tag(dl, t0.x - 16.f, p0, h);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.f, 6.f));
    if (pressed) *on = !*on;
    return pressed;
}

bool stepper_row(const Metrics& m, const char* id, const char* label, int* v, int lo, int hi,
                 int step, const char* unit, const char* description, bool overridden) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = row_height(m);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::Dummy(ImVec2(w, h));
    const ImVec2 p1(p0.x + w, p0.y + h);
    card_frame(p0, p1, ImGui::IsMouseHoveringRect(p0, p1), false);
    draw_row_label(dl, p0, h, label, true);
    note_info(ImGui::IsMouseHoveringRect(p0, p1), description);
    bool changed = false;
    char val[48];
    std::snprintf(val, sizeof val, "%d%s", *v, unit ? unit : "");
    const float bh = 44.f;
    float x = p1.x - 14.f - 56.f;
    const float y = p0.y + (h - bh) * 0.5f;
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (action_button("+", false, 56.f) && *v < hi) {
        *v = std::min(hi, *v + step);
        changed = true;
    }
    note_info(ImGui::IsItemFocused(), description);
    const ImVec2 vs = ImGui::CalcTextSize(val);
    x -= 8.f + 100.f;
    dl->AddText(ImVec2(x + (100.f - vs.x) * 0.5f, p0.y + (h - vs.y) * 0.5f), kText, val);
    x -= 8.f + 56.f;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (action_button("-", false, 56.f) && *v > lo) {
        *v = std::max(lo, *v - step);
        changed = true;
    }
    note_info(ImGui::IsItemFocused(), description);
    ImGui::PopItemFlag();
    if (overridden) draw_override_tag(dl, x - 14.f, p0, h);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.f, 6.f));
    return changed;
}

void open_picker(const char* id) { ImGui::OpenPopup(id); }

int picker(const Metrics& m, const char* id, const char* title,
           const std::vector<PickerChoice>& choices, int current) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = std::min(620.f, vp->Size.x * 0.92f);
    const float max_h = vp->Size.y * 0.86f;
    ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->GetCenter().y), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0.f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.f), ImVec2(w, max_h));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(8, 10, 14, 250));
    ImGui::PushStyleColor(ImGuiCol_Border, v4(kLine));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 150));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f, 14.f));
    int result = -1;
    const bool open = ImGui::BeginPopupModal(id, nullptr,
                                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    if (!open) return -1;
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextUnformatted(title);
    ImGui::SetWindowFontScale(1.f);
    ImGui::Dummy(ImVec2(0.f, 6.f));
    const bool first_frame = ImGui::IsWindowAppearing();
    ImGui::BeginChild("##choices", ImVec2(0.f, std::min(max_h - 140.f, static_cast<float>(choices.size()) * (m.item_h + 8.f) + 4.f)),
                      ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
    for (size_t i = 0; i < choices.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (first_frame && static_cast<int>(i) == std::max(0, current)) {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNavCursorVisible(true);
        }
        const float rw = ImGui::GetContentRegionAvail().x;
        const float rh = m.item_h + (choices[i].detail.empty() ? 0.f : 14.f);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const bool pressed = nav_button("##c", ImVec2(rw, rh), ImGuiButtonFlags_EnableNav);
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        const ImVec2 p1(p0.x + rw, p0.y + rh);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        card_frame(p0, p1, hot, ImGui::IsItemFocused() && ImGui::GetIO().NavVisible, 10.f);
        const ImVec2 ls = ImGui::CalcTextSize(choices[i].label.c_str());
        const float ly = choices[i].detail.empty() ? p0.y + (rh - ls.y) * 0.5f : p0.y + 10.f;
        dl->AddText(ImVec2(p0.x + 16.f, ly), kText, choices[i].label.c_str());
        if (!choices[i].detail.empty())
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.88f, ImVec2(p0.x + 16.f, ly + ls.y + 1.f),
                        kMuted, choices[i].detail.c_str());
        if (static_cast<int>(i) == current) {
            const ImVec2 cs = ImGui::CalcTextSize("selected");
            dl->AddText(ImVec2(p1.x - 16.f - cs.x, p0.y + (rh - cs.y) * 0.5f), kGood, "selected");
        }
        if (pressed) {
            result = static_cast<int>(i);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0.f, 4.f));
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0.f, 6.f));
    if (action_button("Cancel", false, 140.f) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        result = -2;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return result;
}

namespace {

// The controls drawn this frame and last, for Left / Right (nav_frame_begin).
struct NavItem {
    ImGuiID id = 0;
    ImRect r;
    ImGuiWindow* win = nullptr;
    ImGuiWindow* root = nullptr;
    ImGuiID scope = 0;
    bool visible = true;
    bool is_tab = false, tab_selected = false; // a tab of the shell's tab bar, and the open one
};
std::vector<NavItem> g_nav_cur, g_nav_prev;
ImGuiID g_nav_scroll_to = 0;
ImGuiID g_nav_last_focus = 0; // the control that last had the ring, for when ImGui drops it
bool g_nav_moved = false;

// After an item was just added: remember it, and scroll it into view if the focus
// was just sent to it.
void nav_register() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.CurrentItemFlags & ImGuiItemFlags_Disabled) return;
    ImGuiWindow* w = g.CurrentWindow;
    NavItem n;
    n.id = ImGui::GetItemID();
    n.r = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    n.win = w;
    n.root = w->RootWindowForNav;
    n.scope = g.CurrentFocusScopeId;
    n.visible = ImGui::IsItemVisible(); // a control scrolled out of sight is still a place to go
    if (ImGui::IsItemFocused()) g_nav_last_focus = n.id;
    g_nav_cur.push_back(n);
    if (n.id == g_nav_scroll_to) {
        ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
        g_nav_scroll_to = 0;
    }
}

} // namespace

bool nav_moved() { return g_nav_moved; }

void nav_mark_tab(bool selected) {
    if (g_nav_cur.empty()) return;
    g_nav_cur.back().is_tab = true;
    g_nav_cur.back().tab_selected = selected;
}

void nav_frame_begin() {
    g_nav_prev.swap(g_nav_cur);
    g_nav_cur.clear();
    g_nav_moved = false;
    if (!enabled()) return;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (!g.NavMoveSubmitted || g.ActiveId != 0) return;
    const ImGuiDir d = g.NavMoveDir;
    if (d != ImGuiDir_Left && d != ImGuiDir_Right && d != ImGuiDir_Up && d != ImGuiDir_Down) return;
    // From the control that has the ring, or, when ImGui has just dropped it (with a gamepad it
    // clears the focus whenever the box it remembers is not inside the window's visible area),
    // from the one that had it.
    const NavItem* cur = nullptr;
    const ImGuiID from = g.NavId != 0 ? g.NavId : g_nav_last_focus;
    if (from != 0)
        for (const NavItem& n : g_nav_prev)
            if (n.id == from) cur = &n;
    if (g.NavId != 0 && !cur) return; // a control that is not one of ours: ImGui moves it as it always did

    const NavItem* best = nullptr;
    float best_score = FLT_MAX;
    if (!cur) {
        // Nothing has the ring yet: it lands on the first control of the page (top, then
        // left), not wherever ImGui's first scoring happens to fall.
        for (const NavItem& n : g_nav_prev) {
            if (!n.visible || n.r.Min.y < g_nav_body.Min.y - 4.f || n.r.Max.y > g_nav_body.Max.y + 4.f) continue;
            const float score = n.r.Min.y + 0.001f * n.r.Min.x;
            if (score < best_score) {
                best_score = score;
                best = &n;
            }
        }
        if (!best) return; // no page of ours: ImGui's
    } else if (d == ImGuiDir_Left || d == ImGuiDir_Right) {
        // The neighbour in the same row.
        const float dir = d == ImGuiDir_Right ? 1.f : -1.f;
        for (const NavItem& n : g_nav_prev) {
            if (n.id == cur->id || n.root != cur->root || !n.visible) continue;
            if (dir * (n.r.GetCenter().x - cur->r.GetCenter().x) <= 1.f) continue;
            const float overlap = std::min(n.r.Max.y, cur->r.Max.y) - std::max(n.r.Min.y, cur->r.Min.y);
            if (overlap < 0.35f * std::min(n.r.GetHeight(), cur->r.GetHeight())) continue; // another row
            const float gap = std::max(0.f, dir > 0.f ? n.r.Min.x - cur->r.Max.x : cur->r.Min.x - n.r.Max.x);
            const float score = gap + 0.5f * std::fabs(n.r.GetCenter().y - cur->r.GetCenter().y);
            if (score < best_score) {
                best_score = score;
                best = &n;
            }
        }
    } else {
        // Row by row: Down goes to the NEXT row below (the nearest band of controls), and of
        // that row to the control nearest across, so a row of filter chips is always passed
        // on the way to what is under it, and back. A control scrolled out of sight still
        // counts, so a long list is walked, not skipped.
        const float dir = d == ImGuiDir_Down ? 1.f : -1.f;
        auto gap_of = [&](const NavItem& n) {
            return std::max(0.f, dir > 0.f ? n.r.Min.y - cur->r.Max.y : cur->r.Min.y - n.r.Max.y);
        };
        auto below = [&](const NavItem& n) {
            if (n.id == cur->id || n.root != cur->root) return false;
            if (dir * (n.r.GetCenter().y - cur->r.GetCenter().y) <= 1.f) return false;
            const float overlap_y = std::min(n.r.Max.y, cur->r.Max.y) - std::max(n.r.Min.y, cur->r.Min.y);
            return overlap_y < 0.35f * std::min(n.r.GetHeight(), cur->r.GetHeight()); // not the same row
        };
        float min_gap = FLT_MAX;
        for (const NavItem& n : g_nav_prev)
            if (below(n)) min_gap = std::min(min_gap, gap_of(n));
        if (min_gap < FLT_MAX)
            for (const NavItem& n : g_nav_prev) {
                if (!below(n) || gap_of(n) > min_gap + 0.6f * std::min(n.r.GetHeight(), cur->r.GetHeight())) continue;
                const float overlap_x = std::min(n.r.Max.x, cur->r.Max.x) - std::max(n.r.Min.x, cur->r.Min.x);
                float score = std::fabs(n.r.GetCenter().x - cur->r.GetCenter().x);
                if (overlap_x > 0.f) score -= 100000.f; // straight below beats off to the side
                if (score < best_score) {
                    best_score = score;
                    best = &n;
                }
            }
    }
    // Into the tab row the ring goes to the open tab, not the tab nearest across.
    if (best && best->is_tab && !best->tab_selected && (d == ImGuiDir_Up || d == ImGuiDir_Down))
        for (const NavItem& n : g_nav_prev)
            if (n.is_tab && n.tab_selected) {
                best = &n;
                break;
            }
    // The four directions on these controls are ours, found or not: ImGui's would pick a
    // control on another row, or skip the page.
    ImGui::NavMoveRequestCancel();
    if (!best) return;
    ImGuiWindow* w = best->win;
    ImGui::SetFocusID(best->id, w);
    ImGui::SetNavFocusScope(best->scope);
    // The window-relative box ImGui scores the next move from (and, with a gamepad, tests
    // against the visible area: a wrong one clears the focus every press).
    w->NavRectRel[ImGuiNavLayer_Main] = ImGui::WindowRectAbsToRel(w, best->r);
    g.NavCursorVisible = true;
    g_nav_scroll_to = best->id;
    g_nav_moved = true;
}

bool nav_button(const char* id, ImVec2 size, ImGuiButtonFlags flags) {
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.f, 0.f, 0.f, 0.f));
    const bool pressed = ImGui::InvisibleButton(id, size, flags);
    ImGui::PopStyleColor();
    nav_register();
    return pressed;
}

bool action_button(const char* label, bool primary, float width) {
    ImGui::PushStyleColor(ImGuiCol_Button, primary ? v4(kAccent) : ImVec4(1.f, 1.f, 1.f, 0.09f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? v4(kAccent2) : ImVec4(1.f, 1.f, 1.f, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(kAccent2));
    ImGui::PushStyleColor(ImGuiCol_Border, v4(kLine));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18.f, 10.f));
    const bool hit = ImGui::Button(label, ImVec2(width, 0.f));
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    nav_register();
    return hit;
}

bool begin_panel(const Metrics& m, const char* id, float x) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float left = vp->Pos.x + x;
    const float w = vp->Pos.x + vp->Size.x - m.pad - left;
    const float h = vp->Size.y - m.top - m.legend - 12.f;
    ImGui::SetCursorScreenPos(ImVec2(left, vp->Pos.y + m.top + 6.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.f, 16.f));
    const bool open = glass_child(id, ImVec2(w, h), ImGuiChildFlags_NavFlattened,
                                  ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    if (!open) ImGui::EndChild();
    return open;
}

void end_panel() { ImGui::EndChild(); }

void begin_card(const char* id, const char* eyebrow, float width) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(kCardBg));
    ImGui::PushStyleColor(ImGuiCol_Border, v4(kLine));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 12.f));
    ImGui::BeginChild(id, ImVec2(width, 0.f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (eyebrow && *eyebrow) {
        std::string up = eyebrow;
        for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        ImGui::PushStyleColor(ImGuiCol_Text, v4(kMuted));
        ImGui::SetWindowFontScale(0.8f);
        ImGui::TextUnformatted(up.c_str());
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.f, 2.f));
    }
}

void end_card() { ImGui::EndChild(); }

void kv(const char* label, const std::string& value, bool good) {
    ImGui::PushStyleColor(ImGuiCol_Text, v4(kMuted));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    const float w = ImGui::CalcTextSize(value.c_str()).x;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - w);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(good ? kGood : kText));
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
}

void muted(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, v4(kMuted));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void banner(const char* text) {
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 28.f);
    const ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, w - 28.f);
    ImGui::PopTextWrapPos();
    const ImVec2 p1(p0.x + w, p0.y + ts.y + 22.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(245, 165, 36, 30), 10.f);
    dl->AddRect(p0, p1, IM_COL32(245, 165, 36, 115), 10.f);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.f, p0.y + 11.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.85f, 0.63f, 1.f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 28.f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 10.f));
}

namespace {

float draw_hint(ImDrawList* dl, ImVec2 at, const Hint& h, bool measure_only) {
    const ImVec2 ks = ImGui::CalcTextSize(h.key.c_str());
    const float kw = std::max(ks.y + 6.f, ks.x + 16.f);
    const float kh = ks.y + 6.f;
    const ImVec2 ws = ImGui::CalcTextSize(h.what.c_str());
    const float total = kw + 9.f + ws.x;
    if (!measure_only) {
        const ImVec2 b0(at.x, at.y - kh * 0.5f);
        const float r = h.key.size() <= 1 ? kh * 0.5f : 8.f;
        dl->AddRectFilled(b0, ImVec2(b0.x + kw, b0.y + kh), IM_COL32(255, 255, 255, 36), r);
        dl->AddRect(b0, ImVec2(b0.x + kw, b0.y + kh), IM_COL32(255, 255, 255, 66), r);
        dl->AddText(ImVec2(b0.x + (kw - ks.x) * 0.5f, at.y - ks.y * 0.5f), kText, h.key.c_str());
        dl->AddText(ImVec2(b0.x + kw + 9.f, at.y - ws.y * 0.5f), IM_COL32(208, 215, 234, 255),
                    h.what.c_str());
    }
    return total;
}

}  // namespace

void legend(const Metrics& m, const std::vector<Hint>& left, const std::vector<Hint>& right) {
    const ImVec2 p0 = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();
    const float h = ImGui::GetWindowHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p0.x, p0.y + h - m.legend), ImVec2(p0.x + w, p0.y + h),
                      IM_COL32(0, 0, 0, 115));
    const float cy = p0.y + h - m.legend * 0.5f;
    float x = p0.x + m.pad;
    for (const Hint& hint : left) x += draw_hint(dl, ImVec2(x, cy), hint, false) + 26.f;
    float total = 0.f;
    for (const Hint& hint : right) total += draw_hint(dl, ImVec2(0, 0), hint, true) + 26.f;
    float rx = p0.x + w - m.pad - (total - 26.f);
    for (const Hint& hint : right) rx += draw_hint(dl, ImVec2(rx, cy), hint, false) + 26.f;
}

void ChordHold::on_event(const SDL_Event& e) {
    if (e.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.type != SDL_EVENT_GAMEPAD_BUTTON_UP) return;
    const bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    if (e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK) l3_ = down;
    else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) r3_ = down;
    else return;
    if (l3_ && r3_) {
        if (since_ns_ == 0) since_ns_ = SDL_GetTicksNS();
    } else {
        since_ns_ = 0;
        fired_ = false;
    }
}

bool ChordHold::poll() {
    if (!(l3_ && r3_) || fired_ || since_ns_ == 0) return false;
    if (SDL_GetTicksNS() - since_ns_ < hold_ns_) return false;
    fired_ = true;
    return true;
}

}  // namespace retcomm::hub::overlay

namespace retcomm::hub::overlay {

namespace {

struct PadKey {
    ImGuiKey key;
    int button;        // SDL_GamepadButton, or -1
    int axis;          // SDL_GamepadAxis, or -1
    int sign;          // -1 / +1 half of the axis; 0 = a trigger (0..max)
};

// What the SDL backend polls, less the left stick (turned into the d-pad below).
constexpr PadKey kPadKeys[] = {
    {ImGuiKey_GamepadStart, SDL_GAMEPAD_BUTTON_START, -1, 0},
    {ImGuiKey_GamepadBack, SDL_GAMEPAD_BUTTON_BACK, -1, 0},
    {ImGuiKey_GamepadFaceLeft, SDL_GAMEPAD_BUTTON_WEST, -1, 0},
    {ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST, -1, 0},
    {ImGuiKey_GamepadFaceUp, SDL_GAMEPAD_BUTTON_NORTH, -1, 0},
    {ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH, -1, 0},
    {ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT, -1, 0},
    {ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, -1, 0},
    {ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP, -1, 0},
    {ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN, -1, 0},
    {ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, -1, 0},
    {ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, -1, 0},
    {ImGuiKey_GamepadL3, SDL_GAMEPAD_BUTTON_LEFT_STICK, -1, 0},
    {ImGuiKey_GamepadR3, SDL_GAMEPAD_BUTTON_RIGHT_STICK, -1, 0},
    {ImGuiKey_GamepadL2, -1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0},
    {ImGuiKey_GamepadR2, -1, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0},
    {ImGuiKey_GamepadRStickLeft, -1, SDL_GAMEPAD_AXIS_RIGHTX, -1},
    {ImGuiKey_GamepadRStickRight, -1, SDL_GAMEPAD_AXIS_RIGHTX, +1},
    {ImGuiKey_GamepadRStickUp, -1, SDL_GAMEPAD_AXIS_RIGHTY, -1},
    {ImGuiKey_GamepadRStickDown, -1, SDL_GAMEPAD_AXIS_RIGHTY, +1},
};
constexpr int kPadKeyCount = static_cast<int>(sizeof kPadKeys / sizeof kPadKeys[0]);
// The left stick's four directions follow the d-pad's four (kPadKeys[6..9]).
constexpr int kStickDirs = 4;

}  // namespace

void pad_nav_frame(bool block) {
    static std::vector<std::pair<SDL_JoystickID, SDL_Gamepad*>> open_pads;
    static bool stick_on[kStickDirs] = {};
    static bool held[kPadKeyCount + kStickDirs] = {};   // down while the capture ended
    ImGuiIO& io = ImGui::GetIO();

    // The pads currently attached, opened once and kept.
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (auto it = open_pads.begin(); it != open_pads.end();) {
        bool still = false;
        for (int i = 0; i < count; ++i) still |= ids && ids[i] == it->first;
        if (still) {
            ++it;
        } else {
            SDL_CloseGamepad(it->second);
            it = open_pads.erase(it);
        }
    }
    for (int i = 0; ids && i < count; ++i) {
        bool have = false;
        for (const auto& [id, pad] : open_pads) have |= id == ids[i];
        if (!have)
            if (SDL_Gamepad* g = SDL_OpenGamepad(ids[i])) open_pads.emplace_back(ids[i], g);
    }
    SDL_free(ids);
    if (open_pads.empty()) {
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        return;
    }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    if (!(io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad)) return;

    // The raw values, merged over every pad.
    float raw[kPadKeyCount] = {};
    float lx = 0.f, ly = 0.f; // the stick, -1..1, whichever pad pushes furthest
    for (const auto& [id, pad] : open_pads) {
        for (int k = 0; k < kPadKeyCount; ++k) {
            const PadKey& pk = kPadKeys[k];
            float v = 0.f;
            if (pk.button >= 0) {
                v = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(pk.button)) ? 1.f : 0.f;
            } else {
                const float a = static_cast<float>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(pk.axis)));
                if (pk.sign == 0) v = std::clamp(a / 32767.f, 0.f, 1.f);
                else {
                    const float d = pk.sign < 0 ? -a : a;
                    v = std::clamp((d - 8000.f) / (32767.f - 8000.f), 0.f, 1.f);
                }
            }
            raw[k] = std::max(raw[k], v);
        }
        const float x = static_cast<float>(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX)) / 32768.f;
        const float y = static_cast<float>(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY)) / 32768.f;
        if (std::fabs(x) > std::fabs(lx)) lx = x;
        if (std::fabs(y) > std::fabs(ly)) ly = y;
    }
    // The stick as four directions, with a gap between engaging and releasing so
    // a stick resting near the edge does not flutter. The stronger axis wins, so a
    // diagonal moves one way, not both.
    const bool horizontal = std::fabs(lx) >= std::fabs(ly);
    const float level[kStickDirs] = {
        horizontal && lx < 0 ? -lx : 0.f, horizontal && lx > 0 ? lx : 0.f,
        !horizontal && ly < 0 ? -ly : 0.f, !horizontal && ly > 0 ? ly : 0.f};
    for (int d = 0; d < kStickDirs; ++d)
        stick_on[d] = stick_on[d] ? level[d] > 0.35f : level[d] > 0.55f;

    // What is down, for the capture latch.
    bool active[kPadKeyCount + kStickDirs];
    for (int k = 0; k < kPadKeyCount; ++k) active[k] = raw[k] > 0.3f;
    for (int d = 0; d < kStickDirs; ++d) active[kPadKeyCount + d] = std::fabs(d < 2 ? lx : ly) > 0.3f;
    bool any_held = false;
    for (int i = 0; i < kPadKeyCount + kStickDirs; ++i) {
        if (block) held[i] = held[i] || active[i];
        else held[i] = held[i] && active[i];
        any_held |= held[i];
    }
    const bool silent = block || any_held;

    for (int k = 0; k < kPadKeyCount; ++k) {
        float v = raw[k];
        // Dpad keys (6..9) also take the stick's four directions, in the order
        // left, right, up, down.
        if (k >= 6 && k < 6 + kStickDirs && stick_on[k - 6]) v = 1.f;
        if (silent) v = 0.f;
        io.AddKeyAnalogEvent(kPadKeys[k].key, v > 0.1f, v);
    }
    // The stick itself is never handed on: it would scroll the window under it.
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, false, 0.f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, false, 0.f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, false, 0.f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, false, 0.f);
}

}  // namespace retcomm::hub::overlay
