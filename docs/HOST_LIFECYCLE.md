# Host lifecycle — Retro as the frontend cores run behind

**Status: design draft, 2026-09-23. Nothing here is implemented yet.**
Decisions marked **(decided)** were made by Alex on that date; everything else is
a proposal until a ruling or an implementation replaces it.

This changes the model in `ARCHITECTURE.md` ("Two launchers, one job each"),
where the hub `exec`s a title and the title opens its own recomp-ui. Under this
design the hub *is* the frontend: one rendered window, a menu drawn as an
overlay, and engine cores running games behind it — the shape RetroArch has,
with cores built against a contract we own.

---

## 1. Decisions

| Decision | Ruling |
|---|---|
| Where the host lives | One generalized host (this repo). Cores never own a window, audio device, input, config source or UI. **(decided)** |
| Where a core runs | In a **child process** — a generic `retcomm-core-runner` that loads the core library. Never in the host process. **(decided)** |
| Quick menu pauses the core | **Yes offline. Never in netplay.** **(decided)** |
| Contract shape | Our own versioned C ABI, not libretro — `include/rcore/rcore.h`, see `CORE_ABI.md`. **(decided)** |

Why a child process, recorded so it is not re-litigated:

- **Loud traps must not kill the frontend.** Doctrine makes dispatch misses trap
  loudly and makes a bridge fatal in strict mode (`recomp-ai-rules/PRINCIPLES.md`,
  "HLE Dispatch Is an Allowlist", "The interpreter may bridge — but only
  honestly"). In-process, every such trap takes the menu with it.
- **Unload is process exit.** The C engines carry global state (psxrecomp's
  `main.cpp` alone is ~16k lines); re-initialising them in one process is where
  in-process core models leak state between runs.
- **Saves survive a core crash.** Battery RAM / EEPROM live in shared memory the
  host owns (§4), so the host flushes them after the runner dies.
- **The cost is small for CPU frames.** A 640×480 RGBA8 frame is 1.2 MB.
  n64lle's product host carries an OpenGL rasterizer, so it may need the GPU
  path early (`CORE_ABI.md`, open item 6). GPU-rendering cores (shared
  dma-buf / DXGI textures) are a later extension of the same protocol.

---

## 2. States

```
Boot ─► Menu (no content) ─► Loading ─► Running ◄──► Quick menu
          ▲                     │          │              │
          │                     ▼          ▼              │
          └────────────── Unload ◄─────────┴──────────────┘
                                ▲
                  runner exit / crash / trap ──► Fault report ─► Menu
```

### Boot
Load config, library index, controller maps, and the **manifests** of installed
cores. A core's identity, ABI version, platforms and capabilities are read from
a sidecar manifest — never by loading the library — so the menu can list what
runs what without executing core code.

### Menu (no content)
Today's hub: library, install / build, global settings, netplay lobby, mods,
Steam shortcuts. The controller-first shell already built stays the navigation
model.

### Loading
1. Resolve title → core via the title's lock (core hash, ABI version, tools
   hash, toolchain).
2. Ensure core, tools and the title's game package exist; run install / generate
   / build if not. This is a progress page that keeps rendering — never a
   blocking call on the render thread.
3. Create the shared region (§4), spawn the runner with it.
4. Runner loads the core, checks its ABI against the manifest (mismatch is a
   refusal with both versions named, not a warning), calls `init` then `load`.
5. Runner reports geometry and audio rate; host sizes the frame texture and
   opens audio.

### Running
The host render loop runs at display refresh, independent of the core's rate.
Each iteration: poll input → write input block → take the newest completed
frame → draw it (aspect, integer scale; shaders later) → drain audio → draw the
overlay if open → swap.

### Quick menu (overlay)
Opened by the guide button or a hotkey. The last frame stays behind it, dimmed.
Contents: resume, save / load state, reset, core options, controls, mods,
netplay, close content. Options that only apply on reload say so.

- **Offline:** the core is paused. The host stops granting frames (§4); a core
  that owns its own loop blocks inside `present` until granted.
- **Netplay:** the core keeps running and the local seat's input goes neutral.
  Pausing one peer to open a menu stalls the other — the pause-to-synchronise
  pattern `CLAUDE.md` §4 forbids. The overlay says it is not paused.

### Unload
Host asks the runner to unload; the core flushes; the runner exits; the host
writes save memory to disk from the shared region, tears the region down, and
returns to Menu or goes straight to Loading for the next title.

### Fault report
Any runner exit the host did not request — crash, loud trap, strict-mode bridge,
ABI refusal — lands here. The host flushes saves from the shared region first,
then shows the exit status, the last lines of the runner's log, and the core's
miss / bridge ring from the shared region, and offers "back to menu". A fault is
never silently retried.

---

## 3. Entry points

The launcher and a standalone release are the **same host binary** in two modes.

| Mode | Boots into | "Close content" |
|---|---|---|
| Library (the launcher) | Menu | returns to Menu |
| Standalone (one bundled core + title) | Loading, for the bundled title | exits the app |
| Direct (`retcomm run <title>`, Steam shortcuts) | Loading | exits the app |

In standalone mode the menu shows only that title's pages (settings, controls,
mods, netplay). Because it is the same host and the same runner, a standalone
player and a launcher player running the same core hash and game package are
netplay-compatible by construction.

---

## 4. Host ↔ runner protocol (sketch)

One shared region per session, created by the host and inherited by the runner
(a `memfd` on Linux, a named file mapping on Windows).

| Block | Writer → reader | Contents |
|---|---|---|
| Control | both | protocol version, state, heartbeat counters, pause / grant, unload request |
| Frames | runner → host | 3 frame slots + "latest completed" index (the host never waits on the core) |
| Audio | runner → host | lock-free SPSC ring of S16 stereo |
| Input | host → runner | per-seat pad state + frame number |
| Save memory | runner ↔ host | battery RAM / EEPROM / memory cards, host-owned so a crash cannot lose them |
| Events | runner → host | always-on ring of misses, bridges, faults, log lines |
| State | runner ↔ host | savestate transfer buffer |

Wake-ups use an eventfd / Windows event pair; nothing polls with sleeps.

**Pause** is the host withholding the grant: a call-per-frame core is simply not
called, and a loop-owning core blocks in `present`.

**Rollback netplay:** the host owns the session, transport and lobby. The
**rollback executor lives in the runner**, next to the core, because resimulating
N frames through a shared-memory round-trip per frame would multiply latency.
The host feeds confirmed and predicted inputs; the runner saves, restores and
resimulates. It snapshots every save region with each core state, because
save memory is host-owned and not in core savestates. A core without
`RCORE_CAP_ROLLBACK` gets delay-based lockstep instead (`CORE_ABI.md`, §Netplay).

---

## 5. Migration

- **Current per-game executables become "external titles"**: launched exactly
  as today (`src/launch/launch.cpp`), with no overlay. They move to the core path
  one engine at a time.
- **n64lle goes first**, after its C→Rust parity rewrite lands; adopting rcore
  is a separate follow-on task. The product runtime is `crates/n64lle-rt` (the
  engine) plus `crates/n64lle-host` (SDL3 host, GL rasterizer, `main`), not the
  proof workspace's `n64-host`. An earlier draft of this item named the wrong
  crate. The host already drives the engine's dispatch loop to a VI-field stop
  point, so it fits `RCORE_CAP_RUN_FRAME`. Fit details: `CORE_ABI.md`.
- **snesrecomp next** — `RtlRunFrame(inputs)` is already call-per-frame.
- **psxrecomp last** — the guest owns the thread and there is no run-one-frame
  entry, so it needs the loop-owning mode.
- The hub's existing pieces (install / build pipeline, library, netplay lobby
  client, mods, Steam) become Menu pages; they are not rewritten.

---

## 6. Open questions

1. The GPU-frame path for GPU-rendering cores (dma-buf / DXGI shared textures).
2. Whether the runner is one generic binary for every core or one per engine
   family.
3. Save-state compatibility across core updates within one ABI version.
4. Hotkey layout and the overlay's controller binding when a game uses every
   button.
