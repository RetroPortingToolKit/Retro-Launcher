# The hub ↔ runner link

How the hub runs a core in its own window. The core runs in
`retro-core-runner` (`CORE_RUNNER.md`), a child process; this link is how the
two talk. The design is `HOST_LIFECYCLE.md` §4; this page is what was built.
Code: `src/corelink/` (shared), `src/runner/runner_link.cpp` (runner side),
`src/hub/hub_play.*` (hub side).

**Status, 2026-09-25: Linux only.** It uses `memfd`, `SOCK_SEQPACKET` and
`SCM_RIGHTS`. Windows builds exactly as before, with no runner, no link and no
play mode.

## Transport

| Channel | What | Owner |
|---|---|---|
| control | `SOCK_SEQPACKET` socketpair, one message per packet; the runner's end at fd 3 | — |
| shared region | one `memfd` at fd 4: header, 3 frame slots of 1024×1024 RGBA, an audio ring | **hub** creates it |
| save memory | one `memfd` per save region, sent to the hub with `SCM_RIGHTS` | runner creates it (sizes are known only after `load()`); **hub** fills and persists it |

Everything the hub needs after a runner crash is memory the hub holds: the
picture, the queued audio, and every save region. EOF on the socket is how the
hub learns the runner died.

## Versioning

The runner is released and updated separately from the hosts that start it
(`Retro-Runtime`), so the link is versioned the way `rcore.h` is:

- **Protocol major.** Host and runner must agree exactly. The host writes its
  major into the shared header, and the runner refuses a mismatch before
  anything else, naming both: "protocol major 2 from the host, 1 in this
  runner -- update whichever is older" (exit 2).
- **Protocol minor.** Only append-only edits: a new message type, or a field
  at the end of a message or of the shared header. The host states its minor
  in the shared header and the runner states its own in Hello. The session
  speaks the lower of the two, and neither side sends anything the other's
  minor does not know.

**1.0** (2026-09-25) is the layout described on this page. It resets the
unreleased development counter, which had reached 3.

## A session

```
hub                                    runner
 spawn (argv = headless flags + --link)
                           <-- Hello        identity: sha256, id, version, caps
                                            set options, lend GL, init, load
                           <-- SaveRegions  one memfd per region
 map, fill (erase value, then file)
 SavesFilled + seats before frame 1 -->
                                            adopt saves, load state if asked
                           <-- Ready
 Grant(frame k, every seat's pad) -->       run_frame; picture -> shared slot
                           <-- FrameDone(k)
 ...
 Quit -->                                   unload, deinit, exit 0
```

- **Input rides inside each Grant**, so the contract's "identical within one
  frame" holds by construction.
- **SavesFilled carries the seats as they stand before frame 1.** A core may
  read input outside a frame, and a seat's `connected` flag is guest-visible.
  Without this, n64lle reading the controllers while `unserialize()` restored a
  state saw "no controller" over the link but "port 1 connected" headless, and
  every savestate scenario diverged. That was found by the byte comparison
  below and is fixed in protocol version 2. The general rule is now in
  `CORE_ABI.md`.
- **Frames go through a lock-free triple buffer.** The runner owns the back
  slot, the hub owns the front slot, and one atomic `middle` with a fresh bit is
  exchanged between them. Neither side waits, and neither can touch the slot
  the other is using.
- **Audio** is an SPSC ring. If the hub stops draining it (paused), the runner
  drops samples and counts them rather than blocking.
- **One grant is outstanding at a time.** Pausing is simply not granting.

## Pacing

While the core produces audio, the hub paces grants by it, granting while
fewer than 60 ms are queued in its SDL audio stream. The audio is what the
player hears, so it is the clock that must not drift. With no audio yet, or no
audio device, the hub paces by the core's stated frame rate
(`set_frame_rate`, rev 5, carried in the shared header). Only a core that
states neither falls back to a fixed 60 Hz.

## Direct mode

`retcomm-hub --run-core <title>_core.so --rom <image> [--title-dir D]
[--tpak1-rom GB --tpak1-save SAV] [--opt key=value ...] [--no-gl]` boots
straight into the core and exits when the player closes it. This is the shape a
standalone release takes (`HOST_LIFECYCLE.md` §3).

- **Files:**
  - session logs go to `<data_dir>/sessions/<stem>/` (`runner.log`,
    `core.log`, `events.tsv`);
  - saves go to `<data_dir>/saves/<stem>/<region>.sav`, except where named by
    a flag.
- **Menu:** the guide button, Esc or F1 open the paused quick menu (Resume,
  Close game). F11 toggles fullscreen.
- **Input:**
  - Gamepads fill seats 0–3 in the order SDL lists them.
  - With none attached, the keyboard is port 1: arrows = D-pad, X/Z/C/S = the
    face buttons, Q/E = shoulders, Shift = Z/L2, IJKL = left stick,
    Enter = Start.
- **Faults:** a runner exit the player did not ask for shows the exit code,
  the runner's reason, FAULT and DISPATCH_MISS events, and the last 40 lines
  of `runner.log`. Saves are written before that screen appears.

## How it was checked (2026-09-25)

All on `pokemonstadium_core.so` built clean from n64lle `ffa84cfc`.

- **The link reproduces the machine exactly.** `retro-core-link-test`
  drives a core through `CoreLink` the way the hub does and writes headless
  mode's artifacts. Through n64lle's `core_parity.sh`, all 10 scenarios give
  artifacts byte-identical to that build's own `rcore_probe`: `summary.txt`,
  every `state_hash.tsv` row, `shot.ppm` taken from shared memory,
  `events.tsv` and `core.log`. That includes the Transfer Pak and
  input-script scenarios.
- **The hub runs it.** `retcomm-hub --run-core` ran on SDL's offscreen video
  and dummy audio drivers for 25 s: about 1,380 frames at about 60 fps under
  audio pacing. When the hub was killed, the runner saw EOF, unloaded the core
  (`RUN_DONE fields=1383`) and exited, and no runner was left behind.
- **Saves survive a crash.** The runner was `SIGKILL`ed 763 frames into a
  Transfer Pak session. The hub reported exit -9 and rewrote the pak's save
  from its own mapping afterwards.
- **Frame-rate pacing** (rev 5, protocol v3). `tests/rcore_fake_core.c` is a
  silent core that states 50/1 after `load()`. The hub, offscreen with dummy
  audio, ran it for 10.07 s of wall time including start-up: 468 frames,
  about 50 fps. The 60 Hz fallback would have run about 565. After the
  shared-header change, `boot300_default` and `slot02_tpak` through the link
  are still byte-identical to `rcore_probe`.
- **Not checked:** the picture on a real screen, sound, controller feel and
  the menus. No one has looked at them. That verdict is Alex's.

## Not built yet

- **Per-title options, accessories and save choice** from the title page.
  Library launch itself exists: see `CORE_LIBRARY.md`.
- **Netplay through the runner** (rev 4).
- **Savestates from the quick menu,** with the envelope.
- **Options UI.** Options come only from `--opt`.
- **Accessory binding UI, per-seat remapping, hot-plug.**
- **The Windows transport.**
