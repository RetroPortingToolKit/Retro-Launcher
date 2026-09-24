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
| Save memory | The engine writes EEPROM files itself (SI) | **Engine change needed** for host-owned `save_memory` |
| Transfer Pak | Needs a Game Boy ROM plus its battery save per port. Stadium expects one on port 1; without one, a state taken with a pak shows "Transfer Pak is not set properly" | Covered by accessory bindings (§Accessories), `n64.transfer_pak` |
| Window, GL, SDL audio | In `n64lle-host` | Move out to the frontend host; the core submits frames |

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

Stadium's case: `n64.transfer_pak`, seats 0–3, `CONTENT | SAVE | NETPLAY`,
extensions `.gb,.gbc`. The Game Boy save is an `RCORE_SAVE_ACCESSORY` region
declared once the cartridge header gives its size.

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
6. The engine's GPU path. `n64lle-host` carries an OpenGL rasterizer, so the
   first n64lle core may need GPU frames (item 3) sooner than the CPU-frame
   draft assumed.
