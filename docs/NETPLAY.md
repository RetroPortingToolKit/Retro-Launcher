# Netplay page

The drawer's **Netplay** opens a full page. It is a lobby system for every
game, laid out after recomp-ui's netplay menus.

Code:
- the page: `src/hub/hub_netplay.cpp`;
- the lobby client: `src/netplay/netplay_client.cpp`;
- Discord sign-in: `netplay_account.cpp`;
- LAN discovery: `netplay_lan.cpp`, tested by `tests/netplay_lan_test.cpp`.

The protocol is recomp-net-server's `docs/WS_LOBBY.md`.

**Starting a match** (2026-09-30, branch `feat/netplay-handoff`):
- **PSX and SNES library games.** The host's PLAY starts the game on every
  seated player's machine. The game takes the match from a launch record,
  with everything it needs settled by the game itself:
  `NETPLAY_HANDOFF.md`. Only builds with the support can do this, and today
  none is released. For any other build, PLAY says to update the game.
- **Direct mode** (one core title): `NETPLAY_DIRECT.md`.

What follows describes the browser and room, which work for every game.

## Pages

These mirror recomp-ui's `LNG_VIEW_NETPLAY_MODE`, `_SIGNIN`, `_NETPLAY` and
`_LOBBY`, and use its wording where it has one.

### Mode

There are two cards.

**LAN / Direct IP.** "No account, no lobby server."

**Online Netplay.**
- When the server offers Discord and you are not signed in, it leads to
  Sign in.
- A saved sign-in being checked holds the card for up to 5 s. After that you
  can go on as a guest.

The header carries two buttons:
- the name button, which opens Player Name;
- `< Back` to this page.

The hub's own header Back leaves the page. The connection and any seat stay
up.

### Sign in (Discord)

It uses `DiscordAccount` on a worker thread:
1. `/auth/discord/start` opens the browser.
2. The page polls until you finish.
3. The device key is stored in `<data>/netplay/account.key`.

The next time the page opens, it signs in silently with that key. After 10 s of
waiting it offers **Retry Discord Sign In**, which cancels the old wait.
Player Name shows who is signed in and has **Sign out**. There is no guest path
on this page, as in recomp-ui.

### Online browser: every game

- **Game filter.** The first entry is **All games**. Then come the netplay
  games installed here, labelled with the version your build sends.
- **Lobbies.** The columns are `Lobby | Game | Players | Spectators | Latency |
  Join`; the Game column appears only for All games.
  - Rows are 32 px. Double-click a row, or press its Join button, to join.
  - `[locked]` rooms ask for a password.
- **Join is enabled only when you can sit down.** The server matches
  `game_name` and `game_version` exactly, so the room's game must be installed
  here at the room's version. Otherwise the tooltip says what is missing.
- **Players online.** The columns are `Player | Where`, with `(you)`. Blocked
  accounts are hidden.
- **Server chat.**
  - The server scopes it per game, so it works only with a game picked.
  - With All games, it says to pick one.
  - Players online follows the same scope.
- **Footer.** **Host Lobby** opens a modal with these fields: game, lobby
  name, max players, spectators, and password. The footer also has
  **Network Settings** (the lobby URL) and **Refresh**.

**How "All games" works.** A connection that never names a game gets every
room and every player. Once it names one, by filter, create or join, the
server keeps that title for the connection. So:
- switching the filter back to All games reconnects;
- leaving a room you hosted or joined from All games reconnects too.

### Room

- **Seats.** The columns are `grip | Slot | Player | Status | Latency | Kick`,
  with a separate spectators table when there are spectators.
- **Dragging a seat.**
  - The host drags anyone (`move`).
  - A player drags themself to an open seat (`seat_move`), or onto another
    player (`seat_swap_request`).
  - The other player gets **Swap seats?** with Swap / Keep my seat. Keep my
    seat declines further asks for 30 s.
- **Kick** is for the host only, and never on the host's row.
- **Chat.** Shared with server chat: lines come only from the server's echo.
  Right-click a name for **Ignore**, **Block**, **Report Message…** and the
  moderation list.
  - Blocks are sent with `set_blocks` on every connection.
  - The list is kept in `<data>/netplay/moderation.json`.
- **Footer.** **Leave Lobby** (the host closes the room), **Settings** /
  **Room Info**, and PLAY (see the top of this page).

### LAN / Direct IP

**Discovery.**
- Rooms from both LAN protocols games use today are listed, for every game:
  - **recomp-net's `RNETBC1` beacon.** The hub listens on UDP 48777.
    Online hosts' same-LAN announces are skipped.
  - **psxrecomp's `MOTK1 BROWSE` → `BEACON`.** The hub broadcasts to ports
    7777–7808 every 3 s.
- **Join Direct** asks one address with `MOTK1 BROWSE`. A PlayStation host
  answers while its waiting room is open. A recomp-net host answers only a
  join, and the dialog says so.

**Joining and hosting stay disabled** (Host Lobby, per-row Join), each with the
reason. In both LAN designs the host's game process *is* the lobby, and a seat
belongs to the guest's game. If the hub sat in a room, it would hold what the
game needs.

## Checked (2026-09-29)

- **Unit tests.**
  - `netplay_lan`: both beacon parsers; a LanBrowser hearing a fake
    recomp-net announce and a fake PSX host over loopback; a Direct IP probe
    answered, and one timing out.
  - The rest of ctest passes.
- **Rendered offscreen** (SDL offscreen + GL, `glReadPixels`), with the real
  page code against the live `ws://netplay.retcomm.net:8765`:
  - the mode page;
  - All games with 2 players online and a 35 ms round trip;
  - the Host Lobby modal;
  - the LAN view: the fake RNETBC1 room, and the fake PSX host found by a
    real broadcast browse;
  - a real room, created locked as "Retro UI test (ignore)" and closed after
    a few seconds, with its seat table and echoed chat.
- **Two processes.** A second process browsing All games listed the first's
  room with its game. Join was enabled because that build is installed here,
  and the host showed as Hosting. With the Tomba! filter the room was absent.
- **Not checked:**
  - a Discord sign-in, which needs a person at the browser;
  - joining a room;
  - seat moves and swaps between two people;
  - reports;
  - the page inside the running hub on a real screen.
