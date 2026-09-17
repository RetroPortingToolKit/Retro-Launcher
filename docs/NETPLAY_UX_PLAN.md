# Netplay UX plan — hub-hosted lobbies

Branch `netplay-ux`. Status: **phase 0 built and verified (2026-09-13); phases
1–4 not started.** Written 2026-09-13 from a read of the hub, recomp-ui,
recomp-net, and recomp-net-server as they stand. File:line references are to
those trees on that date.

## 1. Goal

A new **Netplay page** in the Retro hub that replaces the in-game lobby for
hub-launched titles:

- **Full page.** It fills the whole window like the settings pages do, not a
  panel beside the library.
- **Discord login only.** No guest names, no LAN or direct-IP mode. Signing in
  is the gate to the page.
- **Game dropdown.** Lists the installed titles that support netplay. Picking
  one hot-swaps the whole page to that game's rooms at the installed version:
  lobby list, players online, server chat.
- **Room = chat + player list + lobby table**, laid out and behaving like
  recomp-ui's browser and room views (§4).
- **Hosted rooms carry the dropdown too.** The host can change which game the
  room is running; guests follow or leave.

One requirement in the brief was cut off after "A". §9 has a placeholder for
it.

## 2. What exists today

### Hub (`retcomm-launcher`)

- Pages are an `if/else` chain of bools inside the one full-viewport window
  (`src/hub/hub_main.cpp:7016-7075`). A full page is an `else if` branch that
  calls a `draw_*_panel` filling the `body` child. `draw_romm_settings_panel`
  (`hub_main.cpp:4345`) is the smallest template. The top bar
  (`draw_marquee`, `hub_main.cpp:978`) swaps its buttons per mode.
- A netplay data model exists and is unused: `NetplayView`,
  `NetplayRoomRow`, `NetplaySlot`, `NetplayLobbyState`,
  `NetplayLaunchRequest` (`src/hub/hub_model.hpp:243-307`), plus per-title
  mirrors `TitleRow::netplay_*` (`hub_model.hpp:233-241`) filled in
  `refresh_rows` (`hub_model.cpp:437-447`).
- Config already has `netplay.{lobby_url, display_name, prefer_ice}` and the
  default `ws://netplay.retcomm.net:8765` (`include/retcomm/config.hpp:21-30`).
- `LaunchMode::Netplay` exists and refuses to run
  (`src/launch/launch.cpp:764-770`). The env hook a handoff would use is
  `LaunchPlan::env`, applied with `setenv` in the child (`launch.cpp:697`).
- Networking: libcurl easy API only, protocol allowlist `http,https`
  (`src/install/http.cpp:78`), nlohmann/json vendored. **No WebSocket client
  anywhere.** Background work follows `HubModel::start_job` (worker thread,
  `mu` mutex, atomics drained on the UI thread; `hub_model.cpp:996`).

### recomp-ui (the reference UX)

- Five full-screen views drawn by `launcher_imgui.cpp:5729-9700`; the UI is
  pure polling over a callback table the game implements
  (`recomp_launcher.h:331-723`). No callbacks into the UI, one `pump` per
  frame, count/get pairs re-read every frame.
- Browser: lobby table `Lobby | Players | Spectators | Latency | Join`
  (`:9671`), no sorting, double-click joins; "Players online" panel
  (`Player | Where`, `:9198`); "Server Chat" band across the bottom.
- Room: seat table `grip | Slot | Player | Status | Latency | Kick`
  (`:8585`), drag to reorder/swap seats, host-only kick and PLAY; chat panel
  shared with server chat (`draw_chat_panel`, `:8794`). Room is entered and
  left purely by seat state each frame (`:12405`), never by an explicit call.
- Status is one persistent string per surface in `th.warn`, no toasts.
  Names are refused, never masked. Chat lines come only from the backend
  ring; the UI never appends its own.
- Discord sign-in is already a full-screen view with `account_*` callbacks
  (`recomp_launcher.h:596-660`); no "continue as guest".
- `docs/HOST_NETPLAY.md` is the authoritative UX/protocol note and should be
  read in full before building §4.

### recomp-net-server

- Rust/axum, JSON over WebSocket, one socket per player; spec
  `docs/WS_LOBBY.md`. Ops: `hello, list, create, join, leave, set_ready,
  chat, server_chat, set_match_caps, set_host_endpoint, start, kick, move,
  seat_move, seat_swap_*, close, signal, mod_*, get_turn_credentials,
  set_blocks, automatch_*, chat_report` (`src/ws_lobby.rs:1571-1706`).
- Rooms are keyed by a server-minted `lobby_id`; `game_name` and
  `game_version` are **exact-match join gates**, as are `disc_fp` and the
  host's `match_caps.mods`. Server chat and players-online are scoped per
  **title**, not version (`ws_lobby.rs:4632`).
- `start` allocates a fresh `session_id`, opens the UDP input relay and
  broadcasts `launch{session_id, host_endpoint, guest_endpoint,
  relay_endpoint, slots[], match_caps, …}` (`ws_lobby.rs:3443-3475`). Online
  always goes through the relay ("sfu"); LAN is only a client-side beacon.
- Discord OAuth is implemented: `POST /auth/discord/start`,
  `GET /auth/discord/callback`, `POST /auth/discord/poll`, browserless relogin
  via `/auth/challenge` + `/auth/session` with a device key
  (`src/discord_auth.rs`). `hello{session}` verifies the JWT and the server
  then owns the display name (`ws_lobby.rs:1578-1602`).
- **Two gaps.** `DISCORD_REQUIRED` / `GUEST_CAN_*` are parsed and documented
  but `guest_refusal` is never called from the lobby (`identity.rs:223-265`).
  And **a room cannot change its game after creation**: no op exists and
  `Lobby.game_name/game_version` are never reassigned.

### recomp-net

- C11, no WebSocket lobby client (`README.md:24-30`). The only WS client is
  psxrecomp's `runtime/src/psx_lobby_client.c` (~3600 lines, singleton,
  `ws://` only, hand-rolled RFC 6455 in `lobby_ws/rnet_ws.c`).
- `src/auth/rnet_auth.c` implements the whole Discord client flow (worker
  thread, plain HTTP/1.1 to the lobby host, opens the browser safely) and is
  reusable as-is by a C++ hub. Plaintext HTTP only.

## 3. Constraints carried in from the doctrine

From `recomp-ai-rules/NETPLAY.md` and `SHIPPING.md`, restated here because
each one shapes a UI decision:

- **Settle session-wide requirements up front** (game version, firmware, mod
  set) and have every peer confirm. A peer that cannot meet them **aborts the
  launch**, it never substitutes. The room must show what is settled and why
  a guest cannot ready up.
- **Version identity is exact and machine-checked.** The value sent as
  `game_version` must be the pin compiled into the installed binary, not the
  catalog string. A mismatch is the "peers cannot see each other" symptom.
- **Mods are cleared, not merged**, for a netplay launch. The hub-launched
  game must resolve to the host's plan, and phase 1 ships vanilla-only (§7).
- **Never pause a peer to synchronize.** Nothing in the hub may hold a game
  process; the hub only hands off and then observes.
- **Scope honestly.** Anything below marked *unbuilt* stays marked in this
  file next to the feature, not in a roadmap elsewhere.

## 4. UX specification

### 4.1 Entry and page states

- *Amended 2026-09-13:* the hub shell is being rebuilt controller-first
  (hamburger drawer on the left, a "Home" card page in the centre, F11
  fullscreen, gamepad navigation). Netplay enters through that shell: a
  **Netplay** card on Home and a drawer item, not a top-bar button. The page
  still sets `hub.show_netplay` and fills the body. While open, the header
  shows **Back to Home** on the right and the signed-in handle (or "Sign in")
  beside it, the way recomp-ui's header shows the effective name.
- The page has three states, one struct (`NetplayLobbyState.view` extended):
  `SignIn → Browser → Room`. Room is entered and left by seat state, exactly
  like recomp-ui (`launcher_imgui.cpp:12405`): if the client is seated and the
  view is not Room, switch to Room; if unseated and in Room, switch to
  Browser. No Back button in Room; **Leave Lobby** in the footer is the only
  way out.

### 4.2 Sign-in

- One card, centred: Discord mark, "Sign in with Discord", a muted line
  naming the lobby server, and a status line. No guest path.
- Click → `account_login_begin` (browser opens) → poll state each frame.
  `WAITING` disables the button and shows "Finish signing in in your
  browser…"; `FAILED` shows the server's error text; `SIGNED_IN` advances to
  Browser automatically.
- On page open, if a device key is stored, a silent relogin runs first and
  the card is disabled for up to 5 s (recomp-ui does the same on its mode
  page, `:9430-9450`).
- Sign out lives in the top bar handle's menu, and drops to SignIn.

### 4.3 Browser

Layout, top to bottom, mirroring `draw_netplay` (`launcher_imgui.cpp:9580`):

1. **Toolbar row.** Game dropdown (left, 320 px), then the version chip for
   the installed build (`v0.12.3-alpha`), then **Host Lobby** and
   **Refresh**. When the selected title is installed at a version other than
   the catalog's `netplay.game_version`, the chip turns `th.warn` with a
   tooltip; the row list still works because the server keys on what we
   send.
2. **Status line** in `th.warn`, cleared on success (recomp-ui `:9646`).
3. **Body**: lobby table left; "Players online" panel (300 px) right when
   width allows (≥ 880 px, same rule as recomp-ui `:9619`).
4. **Server Chat** band (230 px) across the bottom.

**Game dropdown.** Items are `TitleRow` entries with `netplay_supported &&
installed`, labelled `name — installed tag`. Uninstalled netplay titles are
listed disabled at the bottom with "Install to play online" so the list
explains itself. Selecting an item:

- sets `filter_catalog_id`, `game_name`, `game_version` (the binary's pin,
  §5.5) on the state;
- if seated as a guest, prompts "Leave *Room* to switch games?" (Leave /
  Stay); if seated as host, this is the host switch of §4.5;
- re-sends `hello`-scope and `list{game_name, game_version}` so the room
  list, players-online and server chat all swap. Unfiltered periodic
  `lobby_list` pushes are filtered client-side by the same pair.

**Lobby table.** Columns and behaviour copied from recomp-ui `:9671-9722`:
`Lobby | Players | Spectators | Latency | Join`, 32 px rows, no sorting,
full-row selectable, double-click joins, per-row Join button, `[locked]`
suffix for password rooms, "No lobbies yet — host one." when empty. Join is
disabled with a tooltip when the selected title has no preferred disc/ROM in
the library (the equivalent of recomp-ui's disc gate).

**Players online.** `Player | Where` (`Hosting` / `In lobby` / `Browsing`),
`(you)` marker, blocked accounts omitted, from the `players[]` array the
server includes in `lobby_list`.

**Server chat.** The shared chat panel (§4.6) bound to `server_chat`. Scope
is per title, so switching the dropdown clears and refills it.

**Host Lobby modal.** Room name (32 chars / 63 bytes, refused not masked),
optional password, max players (2..`netplay_max_slots`), allow spectators.
Fields for the selected game's `match_caps_schema` (psx-v1: aspect, turbo
loads, BIOS HLE, fast boot, auto-skip FMV, language) as in recomp-ui's
`Lobby Settings`. Create → `create{game_name, game_version, max_slots,
password}` then `set_match_caps`.

### 4.4 Room

Layout mirrors `draw_lobby` (`launcher_imgui.cpp:8954`): seats left, chat
right (380 px), stacking below 920 px.

**Header strip** carries the room name, the **game dropdown** (host only;
guests see a static label), the version chip, and the mod summary
("Mods: vanilla match" in phase 1).

**Seat table** copied from `draw_lobby_seats` (`:8557`) and
`draw_lobby_seat_row` (`:7393`): `grip | Slot | Player | Status | Latency |
Kick`, 42 px rows. Status is `Host` / `Connected` / `Waiting` / `Watching`.
Latency is local-to-seat, never on the local row. Kick is host-only, never
on the host row, disabled with a reason tooltip otherwise. Drag-and-drop with
payload `NETPLAY_MEMBER_SLOT`: host drag → `move`, self drag to empty seat →
`seat_move`, self drag onto a player → `seat_swap_request` and the occupant
gets the Swap / Keep-my-seat modal with the 30 s auto-decline. Every refusal
writes one sentence to the room status line; a silent no-op is a bug.

**Ready.** Guests get a Ready toggle in the footer; the host's ready is
implicit. The server clears everyone's ready on any membership change and on
a host game switch; the UI just re-reads it.

**Footer** (`draw_lobby_footer`, `:9062`): `Leave Lobby` (red) · `Room
Settings` (guests: `Room Info`) · right-aligned neon **PLAY** for the host,
else "Waiting for the host to start…". PLAY is enabled when two or more
players are seated and no peer is unready; the tooltip names the blocker.

**Chat** is the shared panel bound to room `chat`.

### 4.5 Host switches the game

The brief's distinctive feature, and the one with no server support today.

Behaviour: the host picks another installed title in the room's dropdown.
The room stays open, keeps its name, password and members' sockets, and the
server re-validates every guest against the new `game_name`,
`game_version` and `disc_fp`. Each guest's hub answers automatically from
its own installed list:

- installed at the same pin → stays seated, ready cleared, status line
  "Host switched to *Tomba!* v0.12.3";
- installed at a different pin or not installed → the hub leaves with a
  status line naming the reason ("You don't have Tomba! installed"), and the
  room shows the seat as open.

Protocol (new, in recomp-net-server): host-only `set_game{game_name,
game_version, disc_fp}` → server updates the lobby, clears ready, broadcasts
`lobby_update` with a `game_changed` marker, and expects `game_ack{ok}` from
each guest within 10 s; non-ack or `ok:false` → `kicked{reason:
"game_changed"}` and the seat opens. `match_caps` is reset to the host's
defaults for the new schema. Until this ships the room dropdown is
**unbuilt** and the host's only path is Close + Host again; the UI must say
so in the dropdown's tooltip rather than silently recreate the room.

### 4.6 Chat panel (one implementation, two bindings)

Copied from `draw_chat_panel` (`:8794`) and `ChatPanelIo` (`:8786`):

- Bordered scroll child (min 80 px) + input (`InputTextWithHint`,
  `EnterReturnsTrue`, 256 bytes) + 72 px Send. Enter keeps focus.
- The backend ring is the only source of lines. Send returns accepted /
  refused; the line appears when the server echoes it.
- System lines (`from` empty) muted, no name. Local name in `th.good`,
  others in `th.accent`. Ignored accounts' lines dropped silently.
- Autoscroll only when the newest `seq` is unseen, so a reader scrolled up
  stays put.
- Right-click on a name → Ignore / Block / Report (phase 4; the menu is
  present but disabled with "Coming later" until then, so the affordance
  matches recomp-ui).

### 4.7 Status, errors, and the rest

- One status string per surface, `th.warn`, cleared on success. No toasts on
  this page.
- Server `error{code}` maps to sentences in one table
  (`version_mismatch`, `game_mismatch`, `disc_mismatch`, `full`,
  `need_password`, `bad_password`, `name_rejected`, `not_host`, …), reusing
  recomp-ui's wording (`launcher_imgui.cpp:7182-7189`).
- Disconnect: banner "Reconnecting to lobby server…" with the retry count;
  after the third failure a Retry button. Being in a room when the socket
  drops means the room is gone; land on Browser with the reason.
- Window resize: the page uses the same breakpoints as recomp-ui (880 px for
  the online panel, 920 px for the room split) so the two look the same at
  the same size.

## 5. Architecture in the hub

### 5.1 Files

| File | Job |
|---|---|
| `src/hub/hub_netplay.cpp/.hpp` | All drawing for the page: `draw_netplay_page`, sign-in card, browser, room, chat panel, modals. Added to both `add_executable(retcomm-hub …)` calls. |
| `src/netplay/lobby_client.cpp/.hpp` | WebSocket lobby client: connection state machine, JSON op encode/decode, snapshot of rooms/members/chat/players-online, error ring. Lives in `retcomm_core` so the CLI can drive it for tests. |
| `src/netplay/ws.cpp/.hpp` | Thin RFC 6455 transport (§5.3). |
| `src/netplay/account.cpp/.hpp` | Discord session: wraps a vendored copy of `recomp-net/src/auth/rnet_auth.c` (C, compiled into `retcomm_core`) behind a small C++ interface. |
| `src/netplay/launch_record.cpp/.hpp` | Builds the settled launch record (§5.6) from the server's `launch` op and writes it for the game. |

The existing `NetplayLobbyState` / `NetplayRoomRow` / `NetplaySlot` in
`hub_model.hpp` are extended rather than replaced: add chat rings,
players-online, server-chat ring, `game_version_pin`, `account` state,
connection state, and the host-create draft's `match_caps` fields. `prefer_ice`
in config is removed (there is no choice to make: online is always the
relay).

### 5.2 Threading

Same shape as recomp-ui, which the reference layouts assume: the UI thread
never blocks and only reads snapshots.

- `lobby_client` owns one worker thread per connection: DNS, TCP, TLS,
  upgrade, then a read loop. Outgoing ops are queued from the UI thread.
- The worker mutates a `Snapshot` under a mutex; the UI copies it once per
  frame (`lobby_client.snapshot()`), the way `HubModel::mu` guards `rows`.
- No `HubJob` for the lobby: it is a long-lived connection, not a job. It
  does not touch `job_running`, so installs and updates keep working while
  the page is open.
- `account` already runs its own thread inside `rnet_auth.c`; the UI polls
  `state()` per frame, matching `account_state()` in recomp-ui.

### 5.3 WebSocket transport

Decision: **libcurl's WebSocket API** (`CURLOPT_CONNECT_ONLY = 2L`,
`curl_ws_send` / `curl_ws_recv`), because curl is already linked, it gives
`wss://` for free, and the server operator expects TLS at a reverse proxy.
*Verified in phase 0:* `src/netplay/netplay_ws.cpp` connects, sends, and
receives against the live server; the host's curl 8.21 lists `ws`/`wss`.
Today only plain `ws://netplay.retcomm.net:8765` exists — the nginx front on
443 serves the Discord callback and answers 404 for `/ws` and `/auth/*`.

- Lift the protocol allowlist in `src/install/http.cpp:78` for this handle
  only (`ws,wss`), not globally.
- Configure-time check: `curl_version_info()` must list `ws`/`wss`; the
  vcpkg manifest gains the `websockets` feature on `curl`; the CI Linux
  image's curl must be ≥ 8.11 (WebSocket left experimental there). If the
  host curl lacks it, the page shows "This build has no WebSocket support"
  and nothing else breaks.
- Fallback if that check fails on a platform we ship: port psxrecomp's
  `lobby_ws/rnet_ws.c` (plain `ws://` only) behind the same `ws.hpp`. Decide
  after the phase-0 spike, not before.

### 5.4 Discord session

- *Built in phase 0:* the hub does the flow itself over libcurl
  (`src/netplay/netplay_account.cpp`, `http_post_json`, in-memory SHA-256 and
  HMAC in `src/core/hash.cpp`), so nothing from `rnet_auth.c` is vendored and
  an `https://` server needs no client change. Start and poll were exercised
  against the live server; the browser half and the stored-key relogin still
  need a real sign-in by a person.
- Until the lobby fronts TLS the session JWT crosses the wire in clear on
  port 8765. The device key never does (only its HMAC), so a sniffed session
  is worth 45 minutes, not the account. Worth closing before phase 2.
- Device key file: `<data_dir>/netplay/account.key`, mode 0600, via
  `rnet_account_set_secret_path`. The JWT is never persisted; it is re-minted
  by `/auth/challenge` + `/auth/session` on each start.
- `hello{session, display_name}` on every connect. If the server answers
  `session_invalid`, drop to SignIn (do **not** continue as a guest, even
  though the server would let us).
- Settings page gains a "Netplay" block: lobby server URL, signed-in handle
  with Sign out, and the display-name field is removed (the server owns the
  handle for signed-in players).

### 5.5 Version pin and disc fingerprint

Two values the hub must produce exactly as the game would:

- **`game_version`** must be the binary's compile-time pin. Source, in
  order: the build stamp recorded by `build_title` (`InstallRecord.tag` is
  `build-<ref>`, normalised with `normalize_netplay_version`), cross-checked
  against `netplay.game_version` in the catalog. *Unbuilt:* a `--print-pin`
  flag in the runtimes so the hub can read the pin out of the shipped binary
  as SHIPPING.md asks; until then a mismatch between record and catalog
  disables Host and Join with a tooltip.
- **`disc_fp`** is the SHA-256 of the disc TOC as computed by the psxrecomp
  runtime. The hub does not have that code. *Unbuilt:* expose it as
  `psxrecomp_cli.py disc-fp <cue>` in the SDK (the launcher already invokes
  that CLI) and cache the result per library entry. SNES titles send no
  `disc_fp`.

### 5.6 Launch handoff

Today every peer launches from **inside** the game's own launcher after
`fill_launch`. With the hub owning the lobby, the game must boot straight
into a session it did not negotiate:

- The hub keeps its WebSocket **open for the whole match**. The server keeps
  the room alive as long as members' sockets are, so when the game exits the
  hub is still seated and the room is ready for a rematch. This is the
  behaviour that makes hosted rooms persistent across games.
- On `launch`, the hub writes a JSON file
  `<install>/netplay/launch-<session_id>.json` containing the fields of
  `RecompLauncherCSettings.netplay_launch` (`recomp_launcher.h:253-322`):
  `session_id, local_slot, input_player, bind_hostport, peer_hostport,
  relay_endpoint, input_delay, input_prediction, max_slots, player_count,
  occupied_mask, force_input_relay, rollback, lobby_kind, is_spectator,
  spectator_wire_slot, guest_memcard, host_spectates, slot_port[]`, plus the
  settled `match_caps` and firmware choice.
- `LaunchMode::Netplay` becomes real: it stages `disc.cfg`/`bios.cfg`/
  `settings.toml` like Default, adds `--no-launcher`, and sets env
  `RECOMP_NETPLAY_LAUNCH=<file>`. Mods are **cleared** for this launch
  (`MODS.md` §7); phase 1 hosts publish an empty mod plan so guests never
  hit `need_mods`.
- Input delay and prediction are computed by the hub exactly as recomp-ui's
  `np_lobby_start` does (`launcher_imgui.cpp:7782`: delay from the max peer
  RTT, prediction `4 + delay` clamped 6..16). The RTT comes from `ping`
  round-trips through the lobby socket in phase 2; *unbuilt:* per-peer RTT
  via the relay probe.
- Game-side (recomp-ui + psxrecomp/snesrecomp runtimes, separate repos): read
  `RECOMP_NETPLAY_LAUNCH`, validate `session_id`/version, skip the in-game
  lobby, and start the session as if `fill_launch` had returned the record.
  On exit, exit normally; soft-return-to-lobby is not used because the hub
  holds the room.

## 6. Server changes (recomp-net-server)

1. **Enforce the Discord policy.** Call `guest_refusal` from the `hello`
   branch (`ws_lobby.rs:1578`) and at the top of `handle_create`,
   `handle_chat`, `handle_server_chat`. The documented intent leaves `list`
   and `join` open to guests (`identity.rs:251-256`); this plan needs a
   decision (§9) on whether `DISCORD_REQUIRED` should also refuse `hello`
   outright. The hub itself never connects without a session either way.
2. **`set_game` op** with the ack/evict handshake of §4.5, and `lobby_list`
   rows gaining the new values immediately.
3. Optional: honour `game_version` in the periodic `lobby_list` pushes so
   clients filter less. Not required; the hub filters locally.

Nothing changes for LAN: it is client-side only and the hub simply never
uses the beacon, `lan_endpoints`, or the exact-port path.

## 7. Scope by phase

Each phase ends with the verification listed; per the workspace rules, a
screenshot precedes any claim about what is on screen, two hubs are two
processes, and whether a match *plays* correctly is Alex's verdict.

**Phase 0 — spike (no UI). Done 2026-09-13.** `retcomm netplay probe` and
`retcomm netplay selftest` (`src/main.cpp`), on `src/netplay/netplay_ws.cpp`,
`netplay_client.cpp`, `netplay_account.cpp`. Verified: digest vectors match;
guest hello/list against the live server; `--title tomba-psx` resolves the
wire scope from the installed build's pin (`0.12.3-alpha`, not the catalog's
`0.12.0-alpha`); two separate processes — one hosting a room, one listing the
same scope — see each other's room, players-online row and echoed chat;
Discord start/poll answer (URL issued, poll pends). *Still open from the exit
criteria:* a room created by psxrecomp's in-game launcher seen by the CLI and
vice versa, and a completed Discord sign-in — both need a person at the
browser/game.

**Phase 1 — page, sign-in, browser.** Netplay button, full page, sign-in
card, game dropdown, lobby table, players online, server chat, Host Lobby
modal creating a room the in-game clients can join. No Room view yet beyond
"you are seated" and Leave. Verification: screenshots at 1280×800 and 880 px
wide against recomp-ui's `LNG_DEMO_LOBBY=browse` render of the same data.

**Phase 2 — room and launch.** Seat table with drag/swap/kick, room chat,
ready, host PLAY, launch record, `LaunchMode::Netplay`, game-side
`RECOMP_NETPLAY_LAUNCH` support in recomp-ui and psxrecomp for one PSX title
(Tomba, netplay `supported: true`). Verification: two hubs on two accounts on
one machine, one PSX title, match starts on both and the hubs are still
seated when both games exit. Gameplay sync verdict: Alex.

**Phase 3 — host switches game.** Server `set_game` + ack/evict; room
dropdown enabled; guest auto-follow/auto-leave. Verification: host switches
between two installed netplay titles with one guest that has both and one
that has one.

**Phase 4 — parity and polish.** Ignore/block/report and `set_blocks`
sync, spectators, password rooms, reconnect banner, SNES titles
(`snes-v1` caps, no `disc_fp`), PSX-Link rooms. *Deferred and marked
unbuilt in the UI:* automatch, mod plans and mod transfer (needs the ICE
side-channel), per-peer RTT before start.

## 8. Risks

- **WebSocket in curl.** If any shipped platform's curl lacks it, the
  fallback is a plain-`ws://` client and the operator must keep a non-TLS
  listener. Settled in phase 0.
- **Cross-repo coupling.** Phases 2 and 3 need coordinated releases of
  recomp-net-server, recomp-ui, psxrecomp and the launcher. Each game-side
  change must be backwards compatible: a game started without
  `RECOMP_NETPLAY_LAUNCH` behaves exactly as today.
- **Pin mismatches.** Until `--print-pin` exists the hub trusts the build
  record. A wrong pin shows up as rooms that never appear, the documented
  failure. The version chip and tooltip exist so it is at least visible.
- **Discord account per tester.** Two-process testing needs two Discord
  identities; the server's `DISCORD_GUILD_ID` gate, if enabled, needs both in
  the guild.

## 9. Open questions for Alex

1. The brief ends with "A" — what was the rest of that requirement?
2. Guest policy on the server: refuse `hello` without a session, or keep
   `list`/`join` open to guests from in-game launchers and only gate create
   and chat (the current documented design)?
3. Does the in-game recomp-ui netplay UI stay for games launched outside the
   hub, or is it retired once the hub owns lobbies? It decides how much of
   `psx_lobby_client.c` remains load-bearing.
4. Will `netplay.retcomm.net` front TLS for the lobby and `/auth`? Today the
   443 front only proxies the Discord callback; the client already handles
   `wss://`/`https://` when it does.
5. Is phase 1 vanilla-only (no mod plan) acceptable for the first release?
6. Where should the pin and disc fingerprint helpers live: SDK CLI (my
   suggestion, reuses the launcher's existing invocation path) or runtime
   flags?
