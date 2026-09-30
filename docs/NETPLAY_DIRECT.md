# Netplay in Direct mode: one game's lobby, and matches the hub launches

**Status: in progress (2026-09-29).** Each piece below is marked **built**,
**in progress** or **not built**, where it is described. This page is the
contract across the four repositories involved.

## The ask (Alex, 2026-09-29)

- **A Netplay button in Direct mode.** It opens the lobby for the game
  running there (Pokemon Stadium Recomp first), and only that game.
- **The hub launches matches.** The lobby server establishes them. In the
  match, **the host is the relay** (the SFU), not the lobby server's relay.
- **Transfer Paks.**
  - A lobby is hosted with or without Transfer Paks.
  - Players bring their paks. Each player's pak saves go to every peer; a
    remote peer's save is stored in an isolated netplay folder.
  - Every player must provide the three Game Boy ROMs themselves, hash
    checked: Pokemon Red, Blue and Yellow, the No-Intro USA/Europe dumps.
  - Direct mode's **Transfer Pak Support** popup keeps their paths and
    sha256.
- **Host reachability.** STUN plus UPnP. When a guest still cannot reach the
  host, fall back to the lobby server's relay.

## Rulings this builds on

- **The runner owns netplay** (2026-09-25, Retro-Runtime `docs/CORE_ABI.md`
  §Netplay). One binding of recomp-net's `rb_driver`, for every core. A core
  supplies the snapshot ring, a resim frame and digests (rcore rev 4). The
  hub never runs the simulation loop.
- **`recomp-ai-rules/NETPLAY.md`:**
  - only published inputs;
  - a deterministic cold boot;
  - host-authoritative saves with guests sandboxed;
  - settle up front or abort;
  - a digest mismatch is a stop;
  - free-run, never pause a peer.

## The pieces

### 1. Hub (retcomm-launcher)

| piece | state |
|---|---|
| Transfer Pak Support popup: Red/Blue/Yellow pickers, size+sha256 against the No-Intro dumps, `platform/n64/transfer_pak.ini` (path + sha256), recheck before a Transfer Pak lobby | **built** (d0514fb) |
| Direct mode **Netplay** button: the lobby page locked to this title (no game filter); the lobby pin is `<title version>+c<core sha256 10>.p<package sha256 10>`, so only identical builds see each other | **built** |
| Host Lobby: "Transfer Paks" on/off (`match_caps.tpak`); a Transfer Pak lobby needs all three ROMs set and rechecked, to host and to join (a join without them leaves, saying why) | **built**; PLAY in a Transfer Pak lobby waits until every seated player's pak has arrived |
| Pak exchange: each seated player's pak (which of the three carts, its save) goes to every peer before start, over the lobby's opaque `signal` (type `0x5450`, base64 chunks of 12 KiB, sha256 checked on arrival). A remote pak is kept under `<data>/netplay/<title>/paks/<player id>.sav`. At launch each seat's save is copied into the match's sandbox (`session-<id>/saves/tpakN.sav`) with the local ROM for that cart, so the runner's match key holds every seat's ROM and save hash | **built** |
| Host relay: a Direct-mode room asks for it (`match_caps.relay = "host"`). The host's hub holds the game port while the room waits (`netplay_nat` `HostPort`: UPnP IGD, else NAT-PMP, else STUN), advertises it with `set_host_endpoint` and answers probes; each guest probes it and sends `path_report`; the room says which way it went. At launch the host frees the port and its runner binds it | **built**; UPnP checked read-only against Alex's router (discovery, the WANIPConnection:1 control URL, GetExternalIPAddress); no mapping was added from here |
| PLAY -> `start` -> `launch` -> a PlaySession in netplay mode: `netplay_runner_args` turns the launch into `--net-*` (transport `sfu`: every peer dials the relay; `host`: guests dial the host); a fresh save sandbox per match (`<data>/netplay/<title>/session-<id>/saves`); the player's NETPLAY options dropped for the core's defaults; the game never pauses (the menu opens over it, the local pad neutral); no save states, no turbo; a runner exit 4 is "The match ended", not a fault | **built** |

### 2. Runner (Retro-Runtime)

| piece | state |
|---|---|
| rcore rev 6: `net_row_from_pad` / `net_row_to_pad`, so a console's pad travels whole in a row (a row holds 16 buttons and one 8-bit stick; n64lle's C buttons ride the right stick, which the "pads need no core hook" claim in rev 4 missed) | **built** (branch `feat/netplay-runner`) |
| recomp-net linked (MIT, self-contained since its rbengine merge); the launcher finds `../recomp-net` | **built** for local builds; a CI build without recomp-net gets a runner answering `netplay 0` |
| `RNetRbHost` bound to the core's rev 4 slots; transport: host = `rnet_session_start_lan_hub` (3+ seats) or a direct pair, guests dial it; `--net-relay` for the server relay | **built** |
| Match key: runner netplay version, core sha256, package sha256, ROM sha256, NETPLAY options, Transfer Pak ROM/save hashes, clock epoch; refused on mismatch at sim 0 | **built** |
| `--net-*` CLI, headless and through the hub link (grants carry the local pad; each grant answered by the next live frame, replays first) | **built** |

### 3. Core (n64lle)

| piece | state |
|---|---|
| rev 4 slots from the existing ring (`n64_rb_*`, `keep_tip`), `run_frame_resim` (the VI hook's replay path), `state_hash_parts` (rdram / cpu+rsp / dev+io); ROLLBACK declared | **built** (branch `feat/rcore-rollback`, on main 6ab51977) |
| `RCORE_INIT_NETPLAY` settles the software rasterizer, `rdp_async` off, the MBC3 on guest time from the session epoch; the config is sealed; mods are cleared; the runner's published rows settle connected bits; cartridge saves start blank in the match's sandbox | **built** |
| rev 6 row codec: the N64 pad (16 bits incl. C buttons, one stick), `pad_map::unmap` | **built** |
| A title whose `game.toml` says `[netplay] pixel_path = "free"` (Pokemon Stadium, measured) keeps each player's renderer: the digest leaves out the bytes the RDP drew, the hidden plane and TMEM; the readback probe rolls back with the machine and ends a match on a readback. `video.renderer` is no longer a NETPLAY option; the RDP worker pool stays off in every session | **built** (n64lle `feat/rcore-rollback` b433ad12; evidence in n64lle `docs/evidence/NETPLAY-DETERMINISM-BOUNDARY.md`) |

### 4. Lobby server (recomp-net-server)

| piece | state |
|---|---|
| A host-relay launch: `match_caps.relay = "host"`; `start` launches `transport: "host"` (no relay_endpoint) when `host_endpoint` is set and every seated guest's `path_report` is a fresh `direct`, else the SFU; `set_host_endpoint` clears stale reports | **built** (branch `feat/host-relay`, unit-tested); not deployed |
| Deployment to netplay.retcomm.net | Alex's |

## Measured (2026-09-29)

Pokemon Stadium (USA), n64lle's generic core on branch `feat/rcore-rollback`,
every peer its own process, 600 frames unless noted, scripted input:

| run | rollback episodes | digest at the same confirmed frame | desyncs |
|---|---|---|---|
| 2 peers, loopback, input delay 0 | 5 | identical (`56217b8952526d15` at 580) | 0 |
| 3 peers, loopback, seat 0 relaying for the others | 5-6 | identical on all three (`fda361eec7d19f2c` at 580) | 0 |
| a forced-mispredict peer | 198 | -- | 0 |
| a peer with a different settled epoch | refused at sim 0 (`mod_set_mismatch`) | -- | -- |
| **live**: two lobby clients made the match on netplay.retcomm.net (`launch`, transport `sfu`), each runner started with `netplay_runner_args`, 300 frames through the server's relay | 2 | identical (`e3fcbe3acbb202d7` at 280), round trip 53-89 ms | 0 |

| **host relay**, against a local recomp-net-server on `feat/host-relay`: the host held port 7790 with `HostPort`, the guest's probe was answered, the server launched `transport: "host"`, the host's runner bound 7790 and the guest dialled it, 300 frames | 0 | identical (`c7b9d97a0c63242a` at 280) | 0 |
| the same with the guest's probe failing: the server fell back to its relay | 0 | identical (`c7b9d97a0c63242a` at 280) | 0 |
| **Transfer Paks**: two processes running the page's own ingest against the local server, a `tpak` room; the host brought Red, the guest Yellow, each a 32 KiB save. Both assembled both paks (same sha256), the launch carried `--tpak1-*` / `--tpak2-*`, and both runners keyed the same ROM and save hashes | 0 | identical (`44e10f6393ce4fbb` at 280) | 0 |
| **the hub end to end**: the real `retro-hub` in Direct mode (SDL offscreen driver), a scripted guest and a local server, all in a loopback-only network namespace (no router was asked). The hub hosted a Transfer Pak lobby from its own Host Lobby modal; the guest's probe of the host was answered; the hub's PLAY launched `transport: "host"` (127.0.0.1:7777); the hub's PlaySession ran the match and showed "The match ended" when the guest's 300-frame runner stopped | 0 | the boot digest agreed (`c1255d82`); both keyed the same `tpak2` ROM and save | 0 |

The Direct-mode pages were rendered offscreen from the real binary: the home
page's Netplay and Transfer Pak Support buttons, the popup empty and with
three verified dumps, the mode page, the Online browser locked to the title,
the Host Lobby modal, a Transfer Pak room, the match and its end.

Not established: two hubs with people at them, a match across two networks
(the host relay needs the server deployed), a real UPnP mapping, and play
with a Transfer Pak actually inserted in a seat (the matches above ran the
intro, where the game does not read the pak).

## Order

1. Core rev 4/6 and the runner binding. Test: two runner processes on
   loopback, headless, with scripted input and the same digests (the
   doctrine's two-process test).
2. The hub's Direct-mode page, launch and PlaySession netplay mode. Test:
   two hubs on one machine, loopback, the host relaying.
3. The server's host-relay launch, STUN/UPnP, then the pak exchange.

## Known limits, stated up front

- **Speed.** Pokemon Stadium frees its pixel path, so a player with a GPU
  renderer plays at full speed: a Battle Now field costs 7.1 ms on Vulkan
  (RTX 3070). A player on the software rasterizer still pays 23.8 ms (70 % of
  real time), because the rasterizer's worker pool (10.6 ms) forked under
  rollback and stays off in netplay. A title that has not been measured keeps
  every player on software.
- **NAT.** Hosting needs a router that honours UPnP/NAT-PMP, or a forwarded
  port, or an open NAT. Otherwise the server relay carries the match.
