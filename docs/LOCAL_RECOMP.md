# Generate Local Recomp

The drawer's **Generate Local Recomp** turns one of the player's own dumps
into a native title app on this machine. It runs n64lle's new-project scaffold
on the dump, then registers the result as a core title (`CORE_LIBRARY.md`), so
the game becomes a row in the library.

**Development builds only.** A CI release (`RETCOMM_RELEASE_BUILD`) has no
such button and no Nintendo 64 platform. Windows has neither either, because
the scaffold and the process runner are POSIX.

Code: `src/hub/hub_local_recomp.{hpp,cpp}` (windowless, unit-tested by
`tests/hub_local_recomp_test.cpp`), `HubModel::*local_recomp*` and
`remove_core_title` in `src/hub/hub_model.cpp`, and `draw_local_recomp` in
`src/hub/hub_main.cpp`.

## The Nintendo 64 platform is local recomps only

No catalog title runs an N64 game. A row on `n64` is shown only when both are
true (`title_passes_library_filter`):

- this is a development build;
- the row has a title app on this machine (`core_app`): one this dialog
  generated, or a registration made earlier by Add Core Title.

So Nintendo 64 is not on Home until the first recomp is added. When the last
one is uninstalled, the title page returns to the grid, and the empty grid
returns to Home.

## The flow

1. **Platform.** Nintendo 64 is the only choice, because n64lle is the only
   framework that makes a port from a dump.
2. **Dump.** The list comes from every file under the platform's library
   folders (`AppConfig::platform_roots("n64")`), searched recursively. It
   includes `.z64`, `.n64` and `.v64` files of any case, and each such entry
   inside a `.zip`. Folders whose names start with `.` are skipped. A zip
   that cannot be read becomes a warning in the console, not a failure. The
   page also shows the n64lle checkout (**Change…** picks another), which hub
   and runner will be bundled, and where the project will go.
3. **"Are you sure you want to recompile this ROM?"** The prompt says it may
   take 5–10 minutes.
4. **Progress.** Shows the scaffold's current step (its `==> ` lines), the
   elapsed time and its output.
   - **Hide** leaves it running; the drawer's button reads "Generating Local
     Recomp…" and reopens it.
   - **Cancel** signals the whole process group. The scaffold's own EXIT trap
     then removes the half-made project.
   - A failure names the log file: `<data>/logs/local-recomp-<time>.log`.
5. **Registered.** The hub adds it to `core_titles` and scans `n64`, and
   **Show in Library** opens the platform.

## What runs

A generation (`local_recomp::Generator`, on its own thread) does this:

1. **Stages the dump.** It reads the file or zip entry, converts it to
   big-endian (`.v64` byte-swapped, `.n64` word-swapped) and writes
   `<data>/local-recomp/staging/<stem>.z64`. The stem is the dump's own,
   because the scaffold names the project from it. The sha256 of that image
   is the title's identity. A dump whose sha256 is already registered is
   refused before anything is built. The staged copy is deleted afterwards.
2. **Gets a development hub and runner.** It uses this hub's own directory if
   that is a title-app hub prefix (`retro-hub`, `retro-core-runner`,
   `packaging/title/build-title-app.sh`: `scripts/build-local.sh` output).
   Otherwise it runs `scripts/build-local.sh` in the Retro-Launcher checkout
   above this hub (at most four folders up) and uses the prefix named on its
   `RETRO_HUB=` line.
3. **Runs the scaffold** from the n64lle checkout, non-interactively:

   ```sh
   bash <n64lle>/tools/new_project/setup_project.sh <staged.z64> --yes \
     --core generate --runner <prefix>/retro-core-runner --hub <prefix>/retro-hub \
     --dir <default install root> --players 4 --transfer-pak --copy-rom \
     --generate --app --git
   ```

   - The core, runner and hub are all development builds. `--core generate`
     builds n64lle's generic core from the checkout, because n64lle has not
     published a release yet.
   - **Accessories.** All four controller ports are declared, and the
     Transfer Pak is supported (`game.toml [transfer_pak] supported = true`).
     No port pins a pak (`portN.pak`), so the Controller Pak and Rumble Pak
     follow the cartridge's own default (ares' per-port table in the core).
   - The project keeps its own copy of the dump (`--copy-rom`). It is a local
     git repository, never a GitHub one.
   - The n64lle checkout is `config.json` `n64lle_checkout`. When that is
     empty, the hub uses `n64lle/` beside the Retro-Launcher checkout.
4. **Finds the app.** The newest `.AppImage` / `.exe` / `.dmg` at the root of
   `<project>/build-release/` is the app, and
   `build-release/app/title/title.json` is its payload.

The registration is `core_titles: [{manifest: <title.json>, name, app,
project, generated: true, rom: <project>/roms/<stem>.z64}]`. Play runs the app
with the library's matching dump, or with `rom` when the library has none (for
example when the library's copy is zipped or byte-swapped).

## Uninstall

Uninstall is in Manage Game Data.

- **Generated recomps** are removed from `core_titles`, and their project
  folder is deleted. The folder is checked first: it must be a port project
  (`game.toml`, `CMakeLists.txt`, `tools/build_app.sh`) and not an install
  root.
  - With **Keep save data**, the app's `<id>-data/` folder is first moved to
    `<data>/local-recomp/kept/`.
  - Generating the same title again moves that folder back beside the new app.
- **A title added by Add Core Title** loses only its registration. Its
  project folder was never the launcher's.

Add Core Title itself is gone from the drawer; this dialog replaces it.
`retcomm core add` still registers a sidecar.

## How it was checked

See the pull request that added it. Nobody has looked at the dialog on a
screen yet, and nobody has played a generated title from the library. Both
are Alex's verdicts to give.
