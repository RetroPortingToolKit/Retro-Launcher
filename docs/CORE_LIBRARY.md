# Core titles in the library

A **core title** plays inside the hub's own window through an rcore core
(Retro-Runtime `docs/CORE_LINK.md`), instead of being spawned as its own program. This page covers
how such a title gets into the library and how Play reaches it. Linux only,
like the link. Code: `include/retcomm/core_titles.hpp`,
`src/core/core_titles.cpp`, and the launch hook and play takeover in
`src/hub/hub_model.cpp` and `src/hub/hub_main.cpp`.

## Where they come from

A per-title core's generated sidecar (`<stem>.rcore.toml`) already names its
title and the ROMs it was generated from (`[title] id`, `content_sha256`). A
core title is a catalog `Title` synthesized from it:

| `Title` field | From |
|---|---|
| `id` | `[title] id` |
| `name` | the registration's name, else `[game] name` in the title's `game.toml`, else the id |
| `kind` | `"core"` |
| `platform` | `[core] platforms[0]` |
| `rom_identity.sha256` | `[title] content_sha256` |
| `rom_extensions` | `.z64` for n64: the hashes are of the big-endian image, so only that byte order can match |
| `core_manifest` | the sidecar's path |

Because it is an ordinary `Title`, everything else already works:

- the ROM scan finds and binds its ROM;
- it gets a library row, cover-art lookup and the missing-ROM flow (Rescan
  Library, Import ROM).

Where the catalog already has a title with the same id, that title gains the
core instead of getting a twin row.

**Registering one.** Registrations are stored in `config.json` as
`core_titles: [{manifest, name}]`, and merged into the catalog after every
catalog load (hub and CLI).

- **Nothing in the hub adds one any more** (since 2026-09-29). Self-
  recompilation is being abandoned for n64lle, in favour of distributing the
  machine-generated source as part of the move to core-based recompilations.
  So the hub's two ways in are gone: **Generate Local Recomp** (2026-09-28,
  which scaffolded a port from the player's dump) and **Add Core Title…**
  (2026-09-25 to 09-28, which adopted a port project the player had built).
  Registrations either one made still load. A registration with an `app`
  runs that title app on Play, with `--rom <the matched ROM>` when there is
  one; the app keeps its settings and saves in its own `<id>-data/` beside it
  (docs/RELEASES.md, "Title-app mode").
- On the CLI: `retcomm core add <sidecar.rcore.toml> [--name NAME]`, then
  `retcomm scan`. `retcomm core list` shows each core title, its core and its
  matched ROM.
- **Uninstall** (Manage Game Data) removes a registration. It touches no
  files: they were never the launcher's.

**Catalog installs** can carry a core too: a `*.rcore.toml` in an install
directory whose `[title] id` matches the title makes it a core title
(`core_manifest_for`). No catalog title ships one yet.

## Rows

A title with a core takes its install state from the core, not from an
install record:

- it is installed when the core's library exists, with
  `install_method = "core"`;
- `core_label` reads "<core id> <version> (dev build)" for display;
- Install, Update, Wine and local build are off: the core was built
  elsewhere, and rebuilding it is not this launcher's job yet.

## Play

Every Play entry point converges on the launch worker
(`HubModel::start_job(Launch)`). For a title with a core, the worker:

1. resolves the core and the ROM;
2. posts a `PlayRequest` to the main thread, instead of spawning anything.

The main loop then:

- starts a `PlaySession` with the session in `<data_dir>/sessions/<id>` and
  saves in `<data_dir>/saves/<id>`;
- hands it every event and the whole frame. The library's shortcuts are
  suspended, since Start would otherwise open the drawer underneath the game;
- returns to the library when the player closes the game.

**Outside the hub:**

- `retro-hub --play <title-id>` starts the hub, presses Play on that title,
  and exits when it closes.
- `retcomm launch <title-id>` on a core title `exec`s `retro-hub --play`. A
  caller that waits on `retcomm launch`, such as a Steam shortcut, therefore
  still waits for the game.

## Romhack bases (2026-10-08)

An n64lle game can run as the game itself or as exactly one installed romhack,
its **base** (n64lle `docs/ROMHACK-PLAN.md`). A romhack is installed beside the game
package as `romhacks/<id>/`, holding `base.toml`, the patch and a delta package
(n64lle `tools/n64romhack.py install`). The core picks the base from its option
`system.romhack`; empty means the game itself.

- **Picker.** In Direct and title-app mode the home page shows a **Base** picker
  when anything is installed (`src/core/romhacks.cpp` `scan_romhacks`). It writes
  the title's `system.romhack` to `options/<key>.ini`, the same file the System
  tab writes, so a session gets it like any other option.
  - A base the core would refuse is listed but disabled, with the reason as its
    tooltip: an id that is not its directory's name, or a missing delta.
- **Saves.** A base keeps its own saves, `saves/<key>@<id>`, apart from the game's
  `saves/<key>` (`base_save_dir`). The exception is a `base.toml` that says
  `saves = "shared-with-stock"`, which is the hack author's word that the layout is
  unchanged. A hack changes the save layout as often as not, so feeding it the
  game's saves, or the reverse, risks corrupting them. Every place a session's save
  dir is chosen goes through this: Direct, title-app, library, and the home page's
  Files row.
- **Netplay.** `system.romhack` is the one `NETPLAY` option a match keeps; the rest
  fall back to the core's default. The runner's match key then admits peers on the
  same base and refuses peers on another, instead of quietly running the game in a
  romhack player's place.
- **Mods.** On a base, the Mods page says per package whether it was made for that
  romhack (its manifest's `[target].bases` lists it) or is **unverified** there
  (`mod_base_standing`). The core still refuses a mechanical conflict at load.
  "Last session" shows the core's `base:` and romhack lines with its `mods:` ones.

Checked by `hub_romhack` (ctest): the scan and its refusals, the save dirs, and the
standings, through the Mods page's own manifest reader. The picker and the Files
row were checked by screenshot against the n64lle fixture package with three bases
installed. Clicking an entry in the open picker was not driven: synthetic clicks did
not reach the window, so that interaction is unexercised.

## How it was checked (2026-09-25)

On a scratch data root (`RETCOMM_HOME`), with Alex's cached catalog and a copy
of his library index, and the Pokémon Stadium core built from n64lle
`ffa84cfc`:

- **Registering.** `retcomm core add .../pokemonstadium_core.rcore.toml`
  registered "Pokemon Stadium" (the name came from `game.toml`), n64, n64lle
  0.373.0.
- **Scanning.** `retcomm scan` hashed the 80 N64 files, a platform no catalog
  title had needed before, in a 22 s scan. It bound
  `Pokemon Stadium (USA).z64` via SHA-256, and not the Rev 1 or Rev 2 dumps
  beside it.
- **Launching.** `retcomm launch pokemonstadium` exec'd
  `retro-hub --play pokemonstadium`. The launch worker posted the play
  request, and the main loop ran it (offscreen, dummy audio): 1,612 fields in
  30 s including start-up. When the hub was killed, the runner unloaded the
  core cleanly (`RUN_DONE fields=1612`) with nothing left behind.
- **Failure path.** `retro-hub --play no-such-title` exits 1 with "did not
  start: unknown title: no-such-title".
- **A normal hub start** with a core title in the catalog runs as before.
- **Not checked:** the row and its Play button on a real screen, and returning to the library after closing a game. These need
  someone at the desktop.

## Not built yet

- **Building a core from the library.** The build pipeline knows psxrecomp,
  gbarecomp and snesrecomp, not n64lle.
- **Per-title options, save choice, and the Transfer Pak cartridge in the
  title page.** The play session starts with the core's defaults and no
  accessories.
- **Generic cores with game packages from a scan.** A generic core's sidecar
  alone is still refused here; such a title comes in as a **title bundle**
  through **Add/Scan Files → Install title (.zip)** (docs/ANDROID.md,
  "Installing a title"), which registers its `title.json`.
