#pragma once

// The hub's Netplay page (docs/NETPLAY.md), behind the drawer's "Netplay".
// Laid out after recomp-ui's netplay menus: a mode page (LAN / Direct IP or
// Online Netplay), Discord sign-in, the lobby browser, and the room. Unlike
// recomp-ui it is not one game's: the browser lists every game's rooms, with
// a filter of the games installed here.
//
// Starting a match is not built: no game can yet take a session negotiated
// outside it, so PLAY says so instead of sending `start`.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SDL_Window;

namespace retcomm::hub {

struct HubModel;
struct Theme;

// A match the lobby server launched, as this peer takes part in it.
struct NetplayLaunch {
    bool host = false;
    int seat = 0;                // this player's lobby seat = session seat
    int slots = 2;               // session seats (the lobby's max_slots)
    std::uint32_t occupied = 0;  // bit i = seat i has a player
    std::uint32_t session_id = 0;
    std::string transport;       // "sfu" (the server relays) | "host" (the host does)
    std::string relay_endpoint;  // sfu
    std::string host_endpoint;   // host
    std::uint16_t host_port = 0; // host: the LOCAL port it holds (NAT-PMP may map another)
    bool tpak = false;           // a Transfer Pak lobby
    // Transfer Pak lobbies: every seat's cartridge (this machine's copy of
    // it) and that seat's save as the room exchanged it. Empty = no pak.
    std::array<std::string, 4> tpak_rom, tpak_save;
};

// One player's Transfer Pak, as brought to a match: which of the supported
// cartridges (hub_core_settings.hpp supported_gb_rom; -1 = none) and the
// battery save's bytes.
struct NetplayPak {
    int cart = -1;
    std::string save;       // the bytes
    std::string note;       // why none, when cart is -1
};

// retro-core-runner's --net-* for a launch (Retro-Runtime docs/CORE_RUNNER.md,
// "Netplay"). Empty, with *error, for a transport this hub cannot join.
std::vector<std::string> netplay_runner_args(const NetplayLaunch& l, std::string* error);

// Direct mode: the page is one game's (docs/NETPLAY_DIRECT.md) and PLAY runs
// the match here, through `launch`.
struct NetplayScope {
    std::string title;          // what the page calls it
    std::string game_name;      // the lobby's wire name
    std::string pin;            // the lobby's wire version: this build, exactly
    int max_slots = 4;
    bool tpak_supported = false;
    // "" when all three Transfer Pak cartridges are set and check out.
    std::function<std::string()> tpak_problem;
    // Start the match; "" when it started, else why not.
    std::function<std::string(const NetplayLaunch&)> launch;
    // True while a match runs here (its runner holds the game port).
    std::function<bool()> match_running;
    // Transfer Pak lobbies: this player's pak (their first seat's Transfer
    // Pak), this machine's file for cartridge i, and where received saves go.
    std::function<NetplayPak()> local_pak;
    std::function<std::string(int)> cart_rom;
    std::string pak_dir;
};

// Fills the page body. Owns the lobby connection, sign-in and LAN discovery,
// which keep running when the page is left (a seat is kept) until shutdown.
// `scope` locks it to one game (Direct mode); null = every game (the library).
void draw_netplay_page(HubModel& hub, const Theme& th, SDL_Window* window,
                       const NetplayScope* scope = nullptr);
// Leaves any room and stops every netplay thread. Call before the hub exits.
void netplay_shutdown();

} // namespace retcomm::hub
