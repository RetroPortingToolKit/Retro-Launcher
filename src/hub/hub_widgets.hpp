#pragma once

// The hub's styled buttons, shared by every page (hub_main.cpp, hub_netplay.cpp).

#include "hub/hub_theme.hpp"
#include "imgui.h"

namespace retcomm::hub {

// Primary actions: muted violet fill.
inline bool accent_button(const char* label, const Theme& th, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, th.accent_button);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.accent_button_hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, th.accent_button_active);
    ImGui::PushStyleColor(ImGuiCol_Text, th.accent_text);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

// Play / success actions — muted green fill; bright th.good stays for status text.
inline bool good_button(const char* label, const Theme& th, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, th.good_button);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th.good_button_hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, th.good_button_active);
    ImGui::PushStyleColor(ImGuiCol_Text, th.good_button_text);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

// Destructive actions — muted red fill (mirrors good_button contrast).
inline bool danger_button(const char* label, const Theme& /*th*/,
                          const ImVec2& size = ImVec2(0, 0)) {
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

} // namespace retcomm::hub
