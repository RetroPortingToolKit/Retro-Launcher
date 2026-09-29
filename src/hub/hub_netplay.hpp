#pragma once

// The hub's Netplay page (docs/NETPLAY.md), behind the drawer's "Netplay".
// Laid out after recomp-ui's netplay menus: a mode page (LAN / Direct IP or
// Online Netplay), Discord sign-in, the lobby browser, and the room. Unlike
// recomp-ui it is not one game's: the browser lists every game's rooms, with
// a filter of the games installed here.
//
// Starting a match is not built: no game can yet take a session negotiated
// outside it, so PLAY says so instead of sending `start`.

struct SDL_Window;

namespace retcomm::hub {

struct HubModel;
struct Theme;

// Fills the page body. Owns the lobby connection, sign-in and LAN discovery,
// which keep running when the page is left (a seat is kept) until shutdown.
void draw_netplay_page(HubModel& hub, const Theme& th, SDL_Window* window);
// Leaves any room and stops every netplay thread. Call before the hub exits.
void netplay_shutdown();

} // namespace retcomm::hub
