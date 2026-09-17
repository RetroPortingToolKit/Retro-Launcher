#pragma once

#include <string>

struct SDL_Window;

namespace retcomm::hub {

// Where a text field's keyboard comes from when the player has a pad in hand
// and no keyboard in reach.
enum class OskBackend {
    None,      // nothing to raise — a physical keyboard is the only way in
    Steam,     // Steam's Big Picture / Deck keyboard, raised by SDL
    Platform,  // the OS's own touch keyboard, raised by SDL
    Spawned,   // a desktop OSK program we start and stop ourselves
};

const char* osk_backend_name(OskBackend b);

// Before SDL_Init: Steam only reads SDL_HINT_ENABLE_STEAM_SCREEN_KEYBOARD at
// startup, and on a Deck it is Steam itself that normally sets it.
void osk_configure_hints();

// After SDL_Init: what this host can actually raise, and why not if it cannot.
OskBackend osk_backend();
const std::string& osk_unavailable_reason();

// A pad is about to open a text field. Called on the frame the button is
// pressed, one frame before the field activates, because SDL only reads
// SDL_HINT_ENABLE_SCREEN_KEYBOARD inside SDL_StartTextInput().
void osk_arm_for_pad();

// Once a frame, before ImGui::Render() (which is where ImGui's SDL3 backend
// calls SDL_StartTextInput). Raises or dismisses the keyboard to match ImGui's
// text-input state. Returns a line worth logging, or "" when nothing changed.
std::string osk_sync(SDL_Window* window, bool want_text_input);

}  // namespace retcomm::hub
