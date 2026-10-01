# Netplay from the library: the hub starts a game into its match

**Status: built on branches, not merged or released (2026-09-30).** This page
is the contract between the hub and the engines whose games it starts
(psxrecomp, snesrecomp), through recomp-ui.

## The ask (Alex, 2026-09-30)

The library's Netplay page refused to start matches for PSX and SNES games
("open the game in its own app (Direct mode)"). It should launch the game
directly into the match.

**Ruling.** The hub holds the lobby seat and hands the game a launch record
(the plan's §5.6 in `NETPLAY_UX_PLAN.md`). Rejected alternative: handing the
seat to the game's own in-game lobby.

## The rule this is built on

**The hub settles nothing.** Everything a game's own lobby would send or work
out comes from the game, never from a copy in the hub. That covers:

- a PSX disc's `disc_fp`;
- the BIOS offer and the settled match BIOS;
- a SNES game's mod offer and its host mod gate;
- every match cap.

A copy in the hub would drift from the engine and desync matches. The rules
behind this are `recomp-ai-rules/NETPLAY.md` §4: settle up front, abort
rather than degrade, clear mods.

## The two calls a game answers

### 1. `<game> --launcher --netplay-query <request>`

The game answers where its launcher would open, so it has resolved everything
the launcher would be seeded with. It writes `"<request>.answer"` and exits;
stdout carries the game's log.

Every top-level request key is `handoff_`-prefixed. The engines' small JSON
readers take a key's first occurrence, and server messages carried inside the
request have keys of their own (`"op"`).

| request | answer |
|---|---|
| `{"handoff_query":"identity", "handoff_disc": D}` | `join_fields`: what the game's create/join add (PSX `disc_fp`, SNES `mod_offer`); `ready`: its `set_ready` message (PSX: with `bios_offer`); `start`: its `start` message with the player's default caps. A hub-hosted room publishes `start.match_caps` at create |
| `{"handoff_query":"settle", handoff_player_id, handoff_lobby_url, handoff_seat, handoff_room}` | `start`: the start message the host sends. PSX settles the BIOS from every seat's offer. SNES applies its mod gate. Otherwise `error`: why no match can start |

Every answer also carries `"v":1`, `"engine"` and `"handoff":1`.

The hub sends `join_fields`, `ready` and `start` verbatim.

### 2. `RECOMP_NETPLAY_LAUNCH=<record> <game> --launcher`

The record holds four keys:
- `handoff_player_id`;
- `handoff_lobby_url`;
- `handoff_seat`: the `created`/`joined` reply;
- `handoff_room`: the latest `lobby_update`;

and `handoff_launch`: the server's `launch`. All are verbatim (`netplay_handoff.cpp`, `handoff_record`).

What happens, in order:
1. recomp-ui (`recomp_launcher_run_window`) reads the variable and unsets it,
   so the record is used once.
2. It calls the game's `ingest_launch`. The engine replays the messages
   through `handle_server_json`, the code that reads them off its own socket.
3. It calls `fill_launch`, then the mod provider's `commit_netplay`. Mods are
   cleared, not merged.
4. It returns LAUNCH with no window.

It writes `"<record>.status"`: `{"ok":true}`, or `{"ok":false,"why":…}` with
QUIT. QUIT means the game never starts an unsettled match.

A handed-off match ends the process. The hub holds the room, so the game does
not soft-return into a lobby:
- psx: `g_netplay_from_lobby` is 0 for a handoff;
- SNES ports with a rematch path: the return-to-lobby branch is skipped.

### How the hub knows a build can do this

It reads the executable for the literal `--netplay-query`
(`launch_supports_netplay_handoff`). It never runs the executable to find
out, because a build without the support would not understand the flag and
would open its own launcher.

An AppImage's `AppRun` is a script, so its payload's `usr/bin` is read.

## Pieces

| repo | branch | what |
|---|---|---|
| recomp-ui | `feat/netplay-handoff` (bba18b4) | `RecompLauncherCNetplayCallbacks.ingest_launch` (appended); `RECOMP_LAUNCHER_HAS_NETPLAY_HANDOFF`; the handoff in `launcher_ng_capi.c`; `tests/launcher_netplay_handoff_test.c` |
| psxrecomp | `feat/netplay-handoff` (c555c97a) | `psx_lobby_ingest_record` / `_replay_room` / `_ready_message` / `_start_message` (the existing builders, factored out); `ae_np_settle_start_caps` shared by in-game PLAY and the query; `--netplay-query`; handoff ends the process |
| snesrecomp | `feat/netplay-handoff` (fc88129) | the same in `snes_lobby_client.c` (with `snes_lobby_start_gate`, the host mod gate); `snes_host_lobby_answer_query`; `--netplay-query` in `host_main.c` |
| GundamWingEndlessDuelSNESRecomp | `feat/netplay-handoff` (6036994) | its own `main.c` runs the launcher: `--netplay-query`, and a handed-off match skips the soft return |
| retcomm-launcher | `feat/netplay-handoff` | `LaunchMode::Netplay`; `netplay_handoff.{hpp,cpp}`; the library room's Join / Host / PLAY / launch; `tests/netplay_handoff_test.cpp`; `tools/netplay_handoff_drive.cpp` |

## The library room, in the hub

- **Join or Host Lobby on a PSX or SNES game.** The hub resolves the install
  the way Play does: its disc or ROM and the player's BIOS choice
  (`HubModel::play_launch_options`). It asks the game for its identity, about
  0.4 s on CTR, then sends the create or join with `join_fields`. A hosted
  room publishes the game's own `match_caps`.
- **Seated.** The hub sends the game's `set_ready`. It sends it again
  whenever the server has cleared it; every membership change clears ready.
- **The host's PLAY.** The game settles from the room, and its `start` is
  sent as is. A refusal, such as "no BIOS every seat can boot", shows in the
  room.
- **`launch`.** Every seat writes its record under
  `<data>/netplay/<title id>/` and starts the game detached. The room then
  shows what the status file says. The hub keeps its seat, so the room is
  there for a rematch.
- **A build without the support** can still sit in the room. PLAY says to
  update the game, or to start the match from the game's own Netplay menu.

## Measured (2026-09-30)

Each run used two seats driven by the hub's own `LobbyClient` and record code
(`retcomm-netplay-handoff-drive`), and two game processes, each from its own
directory. The lobby server was a local recomp-net-server on `feat/host-relay`.
Everything ran inside a loopback-only network namespace: no router, no public
server. The games rendered with `SDL_VIDEO_DRIVER=offscreen`, ran free for
40 s, and received no input.

| game | identity | settle | handoff | match |
|---|---|---|---|---|
| Crash Team Racing (psxrecomp master + this branch; recomp-ui master + this branch) | `disc_fp 27c53f62…` (= game.toml `required_disc_fp`); offer OpenBIOS + SCPH-1001 `37157331` | `start` with `session_bios: scph1001`, both seats' offers | both `{"ok":true}`; "netplay session BIOS = SCPH-1001 … match only; preference unchanged" | rollback, server relay, RTT 21 ms; **all 80 per-subsystem live digests identical on both peers** (sim 0–2528); no mismatch |
| Gundam Wing Endless Duel (snesrecomp main + this branch) | `mod_offer` of its 4 packages | `start` with `mod_plan: []` (vanilla) | both `{"ok":true}`; "peer confirmed the mod set" | rollback, server relay; boot digest agreed (`c8f44722`) on both; no mismatch logged (rbengine's hash confirm is silent on agreement, so continuous agreement is not shown by the logs) |

**Not established:**
- the real `retro-hub` window driving this (the page code compiled, but
  nobody clicked through it);
- two machines;
- the public server;
- any input, or play judged by a person.

These runs also opened the machine's real audio device. The driver now sets
`SDL_AUDIO_DRIVER=dummy`.

## Rolling it out (not done)

A game takes hub matches only when its build has these engine changes. Every
port pins psxrecomp or snesrecomp and recomp-ui as submodules. Measured
2026-09-30, the local ports lag their engine heads by 17–756 commits:
- CTR: psxrecomp 263, recomp-ui 83;
- Gundam: snesrecomp 17, recomp-ui 83.

So each port needs:
- the branches merged in the engines;
- a pin bump in the port;
- a release.

SNES ports with their own `main.c`, like Gundam, also need the two small
changes listed above. Third-party ports, such as mstan's Tomba, get the
support when their maintainers re-release.

## Known limits

- **A query that hangs holds a worker.** A hung game in query mode is never
  killed, and the room shows "Preparing…". A build that supports the query
  answers in under a second.
- **No server IP in the record.** The hub's WebSocket layer does not expose
  the server's IP, so the record carries no `handoff_server_ip`. psx's relay
  rewrite falls back to the lobby URL's host.
- **Spectators** are seated by the server but not exercised here.
