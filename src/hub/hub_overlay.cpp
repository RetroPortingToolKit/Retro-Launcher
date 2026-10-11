#include "hub/hub_overlay.hpp"

#include "imgui_internal.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace retcomm::hub::overlay {
namespace {

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
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + mk, a.y + mk), kAccent, kGood, kGood, kAccent);
    const ImVec2 rs = ImGui::CalcTextSize("R");
    dl->AddText(ImVec2(a.x + (mk - rs.x) * 0.5f, a.y + (mk - rs.y) * 0.5f),
                IM_COL32(6, 16, 26, 255), "R");
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
            const bool pressed = ImGui::InvisibleButton("##item", ImVec2(w, m.item_h),
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

int tab_bar(const Metrics& m, const std::vector<std::string>& tabs, int selected, bool enabled,
            const std::string& status) {
    const ImVec2 p0 = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + m.top), IM_COL32(0, 0, 0, 115));
    const float cy = p0.y + m.top * 0.5f;
    const float mk = m.compact ? 28.f : 34.f;
    const ImVec2 a(p0.x + m.pad, cy - mk * 0.5f);
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + mk, a.y + mk), kAccent, kGood, kGood, kAccent);
    const ImVec2 rs = ImGui::CalcTextSize("R");
    dl->AddText(ImVec2(a.x + (mk - rs.x) * 0.5f, a.y + (mk - rs.y) * 0.5f),
                IM_COL32(6, 16, 26, 255), "R");

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
        if (ImGui::InvisibleButton("##tab", ImVec2(tw, h), ImGuiButtonFlags_EnableNav))
            clicked = static_cast<int>(i);
        const bool on = static_cast<int>(i) == selected;
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
    const bool pressed = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
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
