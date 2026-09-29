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
catalog load (hub and CLI). Ways to add one:

- In the hub, on a development build: the drawer's **Generate Local
  Recomp** makes a port project from one of the player's dumps and registers
  it (`LOCAL_RECOMP.md`; since 2026-09-28). It replaced **Add Core Title…**,
  described below because its registrations still load.
- Until 2026-09-28, the drawer's **Add Core Title…** adopted a **port project**
  (from 2026-09-27; before that it picked a sidecar). Pick the title app the
  project built (`tools/build_app.sh`: `<project>/<build>/app/<Name>-<v>-<os>
  .AppImage|.exe`). The hub walks up from it to the project root -- an n64lle
  port: `game.toml`, `CMakeLists.txt` and `tools/build_app.sh` -- and reads the
  payload staged beside the app (`title/title.json`: the generic core, the game
  package and the ROM's sha256). It then asks to move the whole project folder
  into the default install root (`default_install_root`, where installed apps
  live), as `<root>/<project folder name>`; a project already under an install
  root is added where it is. **Declining adds nothing.** The move is a rename,
  or a copy then removal across drives; a destination that exists is refused.
  Build trees inside the project keep the old path in their CMake caches, so
  they need configuring again. The registration is `core_titles: [{manifest:
  <title.json>, name, app, project}]`: the row matches the library's ROM by
  sha256, and **Play runs the title app** (detached, `--rom <the matched ROM>`
  when there is one), which keeps its settings and saves in its own
  `<id>-data/` beside it (docs/RELEASES.md, "Title-app mode").
- On the CLI: `retcomm core add <sidecar.rcore.toml> [--name NAME]`, then
  `retcomm scan`. `retcomm core list` shows each core title, its core and its
  matched ROM.

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
- **Not checked:** the row, its Play button and the Add Core Title dialog on a
  real screen, and returning to the library after closing a game. These need
  someone at the desktop.

## Not built yet

- **Building a core from the library.** The build pipeline knows psxrecomp,
  gbarecomp and snesrecomp, not n64lle.
- **Per-title options, save choice, and the Transfer Pak cartridge in the
  title page.** The play session starts with the core's defaults and no
  accessories.
- **Generic cores with game packages.**
