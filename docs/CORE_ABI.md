# Core ABI — `include/rcore/rcore.h`

**Status: draft, 2026-09-23.** Alex ruled on 2026-09-23 that the contract is our
own ABI, not libretro. The header is `RCORE_ABI_MAJOR 0` (draft) until a core
and the runner both build against it; the first implemented contract is 1.

Lifecycle, process model and pause rules: `HOST_LIFECYCLE.md`. This page covers
only the shape of the contract and why.

## Draft revisions

| Rev | Date | Change |
|---|---|---|
| 1 | 2026-09-23 | First draft. |
| 4 | 2026-09-25 | Alex's ruling that the runner owns netplay: `rb_snap_*`, `run_frame_resim` and `state_hash_parts` for `CAP_ROLLBACK`, mapped onto recomp-net's `RNetRbHost`; host `wall_clock_us`, so a clock cartridge stays deterministic. |
| 3 | 2026-09-24 | From the n64lle rcore session's fit report, with Alex's rulings: lent GL context (`RCORE_CAP_GL_COMPUTE`, `gl_get_proc_address`); `RCORE_OPT_STRING` and NULL = unset; `axis_direction` on input descriptors; `erase_value` on save regions; delay-based lockstep netplay for cores without `ROLLBACK`; optional `state_hash`; instrument env knobs allowed. Sidecar manifest specified. |
| 2 | 2026-09-23 | Accessory slots (types, bindings, `RCORE_SAVE_ACCESSORY`, hot-plug); `state_compat_id` plus the host savestate envelope and refusal rule. Driven by the rust-parity session's Transfer Pak and savestate facts. |

`RCORE_DRAFT_REVISION` tracks this table; it disappears when the ABI reaches
major 1.

## Shape

- **One exported symbol**, `rcore_entry(host_abi_major)`, returning a table of
  function pointers. The loader never looks up anything else.
- **Two tables.** `rcore_core_api` (core implements, runner calls) and
  `rcore_host_api` (runner implements, core calls). Every call goes through a
  table, so a core links against nothing from the host.
- **Append-only evolution.** Every struct starts with `struct_size`; minor
  versions only append. No `bool`, no enum-typed fields, nothing by value but
  scalars — the same discipline `n64lle/module_abi.h` applies to generated code.
- **Capabilities are declared, never faked.** A core that does not declare
  `RCORE_CAP_ROLLBACK` is refused for netplay; the host does not attempt it.

## Decisions inside the draft, and why

| Choice | Reason |
|---|---|
| Both `run_frame` and `run` + `frame_boundary` | snesrecomp is call-per-frame (`RtlRunFrame`); psxrecomp and C n64lle own the thread and call out at vblank. One contract has to take both. `frame_boundary` is also the loop-owning core's safe point for savestates. |
| Generic positional pad, core supplies labels | The host maps physical devices once; each core maps the generic pad to its console. The guide button is never delivered — it opens the overlay. |
| Options declared by the core, values from the host | Replaces n64lle's ~133 environment-variable reads. `RCORE_OPT_FLAG_NETPLAY` marks options that change the simulation, so they join the netplay match key. |
| Save memory is host-owned | Lives in the runner's shared region, so a crashed core cannot lose a save. The core never writes save files. |
| `report()` with `DISPATCH_MISS` / `BRIDGE` / `FAULT` | Doctrine's loud misses and honest bridges reach the frontend's banner and fault screen instead of stderr. `RCORE_INIT_STRICT` makes a bridge fatal. |
| Identity is the host-computed file hash | `core_version` is display text. A self-reported identity can be wrong; a hash of the file cannot. |

## Mapping onto n64lle

**Retraction (2026-09-23).** An earlier version of this section mapped
`crates/n64-host`. That was the wrong crate: `n64-host` belongs to n64lle's
Rust *proof* workspace, not the product runtime. The rust-parity session
corrected it; the facts below are from that session, and I checked the two
crate names and their crate types on branch `rust-parity`.

**Where the product runtime lives** (n64lle branch `rust-parity`, worktree
`~/Documents/GitHub/n64lle-rust-parity`):

- `crates/n64lle-rt` — the engine. A staticlib + rlib that exports the same C ABI as
  the old C runtime.
- `crates/n64lle-host` — SDL3 host, launcher glue, OpenGL rasterizer, `main`.
  A port links it as one staticlib with its generated C.

That rewrite is a faithful C→Rust transliteration, gated bit-exact against the
all-C build. Adopting rcore is a **separate follow-on task**, which needs Alex's
go-ahead after parity lands. Nothing below is being built yet.

| Contract point | n64lle today | Fit |
|---|---|---|
| Loop ownership | The host drives the engine: `host_main` calls the dispatch loop, which returns at a stop predicate on dispatch-loop boundaries (the VI field in practice) | **`RCORE_CAP_RUN_FRAME` is feasible** without restructuring the engine |
| Savestate safe point | Taken only at that same dispatch-loop boundary (`docs/SAVESTATES.md`). A load invalidates learned dispatcher state | Fits. States are raw struct bytes with an integrity key, **bound to one build** — so `state_compat_id` stays NULL and the envelope checks the core file hash (§Savestates) |
| Determinism | The guest is deterministic run to run; the parity gate depends on it. With the async software rasterizer on, RDRAM hashes sampled mid-field race the workers; presented output is still deterministic | `CAP_DETERMINISTIC` holds for the simulation. Any netplay state hash must be sampled at the field boundary, never mid-field |
| Settings | ~183 knobs, read through `getenv` and the host-config seam `n64_config_get()` | Feasible: move the remaining raw `getenv`s behind the seam, then serve the seam from `option_get` |
| Input | The SI/PIF model already has four ports; the host feeds each port's pad | Seats map directly |
| Save memory | Formerly written as files by the engine (SI); on branch `rcore`, write-through into `save_memory` | Done on `rcore`, `erase_value` 0xFF |
| Transfer Pak | Needs a Game Boy ROM plus its battery save per port. Stadium expects one on port 1; without one, a state taken with a pak shows "Transfer Pak is not set properly" | Covered by accessory bindings (§Accessories), `n64.transfer_pak` |
| Window, GL, SDL audio | In `n64lle-host` | On `rcore`, `pokemonstadium_core.so` links no SDL and no GL and exports only `rcore_entry`. GL comes lent (ruling 1). |

## Rulings, 2026-09-24

Made by Alex on the n64lle rcore session's fit report (n64lle branch `rcore`,
`docs/RCORE.md`):

1. **Lent GL context — yes.** `RCORE_CAP_GL_COMPUTE` plus
   `gl_get_proc_address`. The runner creates a headless GL 4.3+ context (EGL
   surfaceless or a hidden window) and keeps it current on the core thread.
   n64lle's GL rasterizer is a compute device: it writes back into RDRAM, and
   the picture still leaves as a CPU frame through `video_submit`. With no GL
   lent, the core runs the software rasterizer and says so through `log()`.
2. **Options: `RCORE_OPT_STRING`, and `default_value == NULL` means unset** —
   the core applies its own default. So every policy knob can become a declared
   option. An undeclared key in `option_get` is a fault, so NULL is unambiguous.
3. **Game Boy RTC — a separate region** (`tpak<n>.rtc`), so the battery file
   stays the standard `.sav` other emulators read.
4. **Netplay without rollback — delay-based lockstep** (§Netplay).
5. **New save regions — `erase_value`.** The host fills a region never saved
   before with this byte. The core never learns whether a region is fresh.

Also settled without needing a ruling:

- **Instrument env knobs stay.** Diagnostic knobs (dumps, censuses, traces)
  may stay environment-driven, per Alex's 2026-09-20 ruling in n64lle
  `docs/CORE-CONFIG.md` §3: they change nothing the machine computes. Only
  policy knobs must be options. The header's wording now says "policy".
- **`axis_direction`** (0 = whole axis, ±1 = one half) lets C-Left and C-Right
  be the two halves of RX.
- **Confirmed semantics:**
  - `save_memory` may be fetched lazily after `load()`, and written through or
    in place.
  - `input_get` is called inside `run_frame` and returns one snapshot per
    frame.
  - Mid-run `set_audio_rate` is allowed.
  - `connected` is visible to the guest; a core never invents a controller.
  - One `load()` per process.
  - `BRIDGE` = bridged and continuing. `DISPATCH_MISS` = not bridged (strict),
    then `FAULT`. The host logs every `BRIDGE` as a miss for triage too.

## Sidecar manifest

What the host reads at boot, without loading the library: `<library
stem>.rcore.toml`, next to the library. **Generated by the core's build, never
hand-written.** At load, the runner checks it field by field against
`rcore_core_info` and refuses any disagreement, naming both values.

```toml
[core]
abi_major       = 0
draft_revision  = 3                        # dropped at major 1
id              = "n64lle"
version         = "0.0.1-rcore+01ee8424"   # display only
library         = "pokemonstadium_core.so" # relative to this file
platforms       = ["n64"]
capabilities    = ["run_frame", "savestate", "deterministic", "strict_mode"]
# state_compat_id absent = states bound to the library's file hash

[title]            # present only for a per-title core (generated code linked in)
id              = "pokemonstadium"         # catalog title id
content_sha256  = ["<hex>", "..."]         # the ROMs this core was generated from
dir             = "title"                  # title_dir, relative; holds game.toml

[build]            # provenance, measured by the build
engine_commit   = "<sha>"
engine_dirty    = true
toolchain       = "cmake-clang-v1/v1.0.14"
generated_utc   = "2026-09-24T00:00:00Z"
```

- **No hash in the manifest.** Identity is the SHA-256 the host computes over
  the library it loads. A manifest cannot vouch for itself.
- **Per-title versus generic cores.** A per-title core carries `[title]`, and
  the host offers it only for that title and those ROMs. A generic core
  (`game_package` capability) omits `[title]` and receives
  `rcore_load_params.package_path`. n64lle's first core is per-title
  (`pokemonstadium_core.so`).
- **Dirty builds are allowed and visible.** `engine_dirty = true` shows in the
  UI and keeps the core out of public netplay lobbies.

## Netplay

**Ruling, 2026-09-25: the runner owns the session.** `retcomm-core-runner` binds
recomp-net's `rb_driver` once, for every core. It owns transport, lobby,
identity, the published input rows, and the snapshots of host-owned save
memory. A core provides only the engine-specific pieces, as contract functions.
One netplay implementation serves every engine, and a fix to it reaches every
title with the next runner update.

The alternative was each core binding `rb_driver` itself. That keeps netplay
working inside hosts other than ours. It was declined: it means one netplay per
engine, and a core cannot snapshot the save memory the host owns.

**Standalone releases use the same runner.** A standalone dev release is our
host and runner dedicated to one title, auto-updated as a bundle together with
its core (`HOST_LIFECYCLE.md` §3). Developers ship title data and a core build
recipe, never their own host or runner. So a standalone player and a launcher
player on the same runner, core and title are netplay-compatible by
construction, and nobody maintains a vendored runner.

`RCORE_INIT_NETPLAY` requires `RCORE_CAP_DETERMINISTIC`. The session mode then
follows from the core's capabilities:

| Core declares | Session |
|---|---|
| `DETERMINISTIC` + `ROLLBACK` | Rollback, driven by the runner's `rb_driver` in `RNET_RB_REPLAY_INCREMENTAL` mode. |
| `DETERMINISTIC` only | **Delay-based lockstep.** Peers exchange inputs with a fixed input delay; no resimulation, no snapshots. |
| neither | Refused for netplay. |

**How the contract maps onto `RNetRbHost`** (recomp-net `include/recomp_net/rb_driver.h`):

| `RNetRbHost` | Provided by |
|---|---|
| `snap_save` / `snap_load` / `snap_has` / `snap_oldest` / `snap_drop_after` | core `rb_snap_*`, plus the runner's own ring of every save region under the same tick |
| `publish` | runner: rows become each seat's `rcore_pad` for that frame's `input_get` |
| `run_tick` | not used; incremental mode |
| live tick / replayed tick | core `run_frame` / `run_frame_resim` |
| `resim_begin` / `resim_end` | runner: it chooses `run_frame_resim`, and presentation and audio are its own |
| `digest_master` / `digest_parts` | core `state_hash` / `state_hash_parts` (folded to 32 bits by the runner) |
| `decode_sample` / `neutral_row` / `sanitize_row` | runner, on the generic pad (all-zero is neutral); no core hook |
| `boot_digest_noted`, `request_return_to_lobby`, `log`, `now_ms` | runner and host |

- **The ring is the core's** and sized by it (`rb_snap_depth`). It lives in
  memory only and is never a player savestate. For n64lle it is the existing
  snapshot ring (`state/rollback.rs`), and the digest is `rb_digest.rs`. The
  per-field replay the SDL harness does today becomes `run_frame_resim`.
- **Match key.** Before a session starts, all of these must match:
  - the runner's netplay version;
  - the core file hash;
  - the title and content hash;
  - `NETPLAY` options and `NETPLAY` accessory bindings, including accessory
    content hashes;
  - the agreed clock epoch.
- **Clocks.** During a session, `wall_clock_us` is a pure function of the
  agreed epoch and the frame number. That replaces n64lle's
  `N64LLE_NET_RTC_EPOCH`, so an MBC3 cartridge no longer breaks determinism.
- **Desync detection** compares `state_hash` at the frame boundary. The hash
  covers only what `DETERMINISTIC` promises; n64lle's async raster workers race
  RDRAM mid-field, so it is taken at the field boundary only.

**Save memory is not in core state, and rollback must cover it.** A console
rewind does not rewind a cartridge battery, so n64lle keeps Transfer Pak battery
RAM out of its states. Host-owned save memory is never in `rb_snap_*` or
`serialize()` output. Therefore:

- the **runner snapshots every save region** under each tick it asks the core to
  snapshot, and restores them together, or a resimulated frame sees a future
  write;
- a **player savestate load does not restore save memory**, exactly like the
  hardware. The envelope still records accessory content hashes, so a state is
  refused against a different cartridge.

**n64lle today.** Rollback runs on the unmerged `feat/rollback*` branches,
binding `rb_driver` inside the SDL harness (`host_netplay.rs`). The SDL-free
`netplay_rb.rs` is what becomes the core's side of the functions above. It has
run 2–4 seats over UDP loopback on one machine, never over a real network and
never with a player. The core still declares no `ROLLBACK`.

## Accessories

Anything plugged into a seat: controller paks, rumble paks, Transfer Paks,
memory cards.

- **The core declares** `rcore_accessory_type`s through `accessory_types()`:
  stable id, label, which seats and slots accept it (up to
  `RCORE_MAX_ACCESSORY_SLOTS` = 2 per seat), and flags. `CONTENT` means it needs
  a file, with the extensions it takes. `SAVE` means it owns save memory.
  `NETPLAY` means it affects the simulation.
- **The host chooses** and passes `rcore_accessory_binding`s in
  `rcore_load_params.accessories`: seat, slot, type, and for content
  accessories the path and the SHA-256 the host verified.
- **Accessory saves** are ordinary host-owned save regions of kind
  `RCORE_SAVE_ACCESSORY`, carrying seat and slot. The host keys the file by the
  accessory content's hash as well as its id, so a Game Boy battery save
  follows its cartridge, not the seat it happens to be plugged into.
- **Hot-plugging** (`RCORE_CAP_ACCESSORY_HOTPLUG`): `accessory_changed()` at a
  frame boundary. On unplug the host persists the old region before the call
  returns; on plug the core declares any new region through the out
  parameter. A core without the capability gets accessory changes only at load.
- **Netplay:** every `NETPLAY` accessory binding — type and content hash — joins
  the match key. Peers with different Transfer Pak cartridges do not match.

Stadium's case: `n64.transfer_pak`, seats 0–3, slot 0 only (one pak per
controller), `CONTENT | SAVE | NETPLAY`, extensions `.gb,.gbc`. `load()` reads
the cartridge header, then declares `tpak<seat+1>` (battery RAM, `erase_value`
0xFF) at exactly the header's size. MBC3 carts with a clock also get
`tpak<seat+1>.rtc`.

## Savestates

The core's bytes stay the core's own format, integrity key included. The host
wraps them in an **envelope** recording everything the state is valid against,
and checks that envelope before the core ever sees the bytes. The envelope is a
host format, not ABI, so it can evolve without touching cores.

**Envelope contents**

| Field | Source |
|---|---|
| magic, envelope version | host |
| rcore ABI major | `rcore_core_info` |
| core id | `rcore_core_info.core_id` |
| **core file SHA-256** | host-computed over the library the runner actually loaded (hash the opened file, not a path looked up again) |
| state compat id | `rcore_core_info.state_compat_id`, or absent |
| game package SHA-256 | host, when `CAP_GAME_PACKAGE` |
| content SHA-256 | host-verified ROM / disc |
| accessory bindings | seat, slot, type id, content SHA-256 — every binding, not only the `NETPLAY` ones |
| simulation options | key and value of every `RCORE_OPT_FLAG_NETPLAY` option |
| core state size, SHA-256 of the core bytes | host |
| frame number, timestamp, thumbnail | host, for display only |

**Load rule — refuse, never attempt.** In this order, stopping at the first
mismatch and naming both values:

1. ABI major, core id.
2. **Build identity.** If the core declares a `state_compat_id`, the ids must
   match exactly. If it declares none (n64lle today: states are bound to one
   build), the **core file hashes** must match.
3. Game package hash, content hash.
4. Accessory bindings, then simulation options.
5. SHA-256 of the core bytes (a corrupt file).

There is no "load anyway". A state from another build is undefined memory in
the new one, and loading it hands the player a crash or a silently wrong game.
A refused state stays on disk and stays listed, marked with why, so a player who
rolls back to the old core gets it back.

**`state_compat_id` is a promise.** A core should declare one only when a gate
proves its states survive rebuilds with the same id. Without that gate, leave it
NULL and accept that core updates invalidate states. Stating otherwise is a
claim with nothing enforcing it.

**Rollback netplay uses no envelope.** Rollback states live in memory, inside
one session whose peers already matched on the full identity at session start.

## Open

1. **Where the header lives.** Collaborators need it without this repo. Likely
   its own small repo vendored by host and cores; drafted here until then.
2. **Vocabulary collision.** This repo's static library is named `retcomm_core`,
   and "core" now means an engine. Rename the library (e.g. `retcomm_lib`)
   before the word drifts (`recomp-ai-rules/PRINCIPLES.md`, "Watch the
   Vocabulary").
3. GPU frames (shared dma-buf / DXGI textures) — a later minor version.
4. Whether 2 accessory slots per seat covers every platform we care about
   (PSX: one memory card per port plus multitap; N64: one pak per controller).
5. A gate that would let n64lle declare a `state_compat_id` — that is, states
   surviving a rebuild — if that is ever wanted.
6. **Resolved (2026-09-24).** n64lle's GL is compute only and its output still
   leaves as a CPU frame, so a lent context (ruling 1) is enough. No GPU-frame
   path is needed for it.
