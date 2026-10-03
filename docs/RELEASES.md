# Releases

What a Retro Launcher release publishes, and the contract of the bare hub
archive that tools download to run `retro-hub` in Direct mode without an
installer. The installers and their names are in
[`packaging/README.md`](../packaging/README.md).

## Cutting a release

Run the `Release` workflow from the Actions tab (manual only). `version` empty
auto-bumps from the newest `vX.Y.Z` tag (`bump` picks the component);
`prerelease` marks it a prerelease, which `releases/latest` skips.

## What a release contains

| Asset | What |
|---|---|
| `Retro-Launcher-linux-x86_64.AppImage` | Linux installer-free app (unchanged). |
| `Retro-Launcher-macos-{arm64,x86_64}.dmg` | macOS (unchanged). |
| `Retro-Launcher-windows-x64-setup.exe`, `Retro-Launcher-portable-windows.zip` | Windows (unchanged). |
| `retro-hub-<version>-linux-x86_64.tar.gz` | The bare hub, below. |
| `retro-hub-<version>-linux-x86_64.tar.gz.sha256` | That archive's SHA-256. |
| `SHA256SUMS` | Every bare hub archive's SHA-256. |
| `hub-manifest.json` | What a tool reads to find and check the bare hub. |

The installer names never change: Retro Studio, the self-updater and users
depend on them. Only the bare archive carries the version in its name, as
Retro-Runtime's archives do.

## Direct mode

```sh
retro-hub --run-core <core> [--package <shim>] --rom <image> [--title-dir <dir>] \
          [--tpak1-rom <gb rom>] [--tpak1-save <sav>] [--no-gl] [--opt key=value]... [--boot]
```

Direct mode runs one title in the hub's window; no library pages, no setup
wizard. It is built on every OS. Since revision 3 it opens on the title's
**home page**: Play, **System** and **Gamepads** (the core's settings page,
opened at that tab; the page has no tab switch of its own here), Mods and Quit;
since `updates 1` (below) also **Update**, beside Play, the title's cover at
the top of the left panel, and the activity console on **`** (as in the
library: the hub's log, which in Direct mode includes everything the hub
writes to stderr, and a runner's stderr, on Linux and macOS). Closing the game
returns to the page, and Quit leaves the app.

**The cover** is the title's own when it has one -- title.json `boxart`, else
`boxart.png|.jpg|.jpeg` beside title.json or in the title dir (n64lle stages a
port's `assets/boxart.*` there: n64lle docs/TITLE-BOXART.md) -- else
libretro's `Named_Boxarts`, fetched at the first launch by the title's name and
ROM file names, and saved as `<data dir>/boxart/libretro/<title key>.png`;
later launches reuse it. `--boot` skips the page, plays at
once and exits when the player closes the game (what every revision before 3
did).
The session itself (menu, input, fault screen) is described in Retro-Runtime's
[`docs/CORE_LINK.md`](https://github.com/RetroPortingToolKit/Retro-Runtime/blob/main/docs/CORE_LINK.md), "Direct mode".

| Flag | Since revision | What |
|---|---|---|
| `--run-core <core>` | 1 | The rcore core library (`<title>_core.so`, or a generic core such as `n64lle_core.so`), its `.rcore.toml` beside it. Turns Direct mode on. |
| `--package <shim>` | 2 | A `GAME_PACKAGE` core's game shim (`<slug>_game.so`, the title's generated code), passed to the runner as `--package`. Required for such a core and refused for any other, by the runner. |
| `--rom <image>` | 1 | The game image. Without it, `--run-core` exits 2 before a window opens. |
| `--title-dir <dir>` | 1 | The directory holding the title's `game.toml`. Default: the shim's directory with `--package`, else the core's. |
| `--tpak1-rom`, `--tpak1-save` | 1 | Transfer Pak cartridge and its save, port 1. |
| `--no-gl` | 1 | Do not lend the core GL. |
| `--opt key=value` | 1 | A core option; repeatable. Wins over a value stored by Core Settings. |
| `--boot` | 3 | Play at once and exit with the game, skipping the home page. In title-app mode, only when a ROM resolves. |
| `--core <core>` | 4 | Alias of `--run-core`; in title-app mode it overrides the title's core. |
| `--title <title.json or its dir>` | 4 | Title-app mode (below) with this title. |
| `--runner <runner>` | 4 | This `retro-core-runner`; beats `RETRO_CORE_RUNNER` and the lookup. |
| `--hub <retro-hub>` | 4 | Run that hub instead, with this title and runner (below). |
| `--check-title` | 4 | Resolve the title and print it; no window (below). |

Since revision 4 every path on the command line is made absolute against the
directory the hub was launched from, before anything else.

**Files.** Session logs go to `<data dir>/sessions/<stem>/` and saves to
`<data dir>/saves/<stem>/`, where `<stem>` is the title's: the shim's file stem
with `--package` (`pokemonstadium_game`), the core's otherwise
(`pokemonstadium_core`). A generic core is shared by every packaged title, so
it never names one.

The settings page -- the same one the library opens from an N64 platform
header's **n64lle Config** -- writes these, and every play path reads them
(`src/hub/hub_core_settings.hpp`):

| File | Scope |
|---|---|
| `<data dir>/platform/<platform>/input.ini` | the four controller seats (device and maps), the stick deadzone, and each seat's expansion pak (`pak = tpak`, `tpak_rom`, `tpak_save`: a Transfer Pak's Game Boy ROM and `.srm` save, given to the session as that seat's `--tpakN-rom` and save region `tpakN`; `--tpak1-rom` on the command line wins over seat 1's. Every seat can hold one with a runner reporting `transfer_pak_seats 4`; with an older runner the hub leaves seats 2-4 out and logs why; or `pak = vru`, `vru_device` (`default` or the SDL recording device's name): the VRU Microphone, given to the session as `--vruN`, the seat reading no pad -- a runner without `accessory_data 1` in `--version` gets the seat as an empty port and the hub logs why), every title of the platform (the core's `.rcore.toml` `platforms`) |
| `<data dir>/platform/<platform>/options.ini` | option values for every title |
| `<data dir>/platform/<platform>/options/<stem>.ini` | one title's overrides; `--opt` wins over both |
| `<data dir>/platform/<platform>/core_description.txt` | the last `--describe`, so the page can label things with no core at hand |

Opened from Direct mode's home page, the page is that one title's: its System
tab edits `options/<stem>.ini` only (there is no "All titles" choice), and
each drop-down shows the value the title plays with -- its own file's, else the
one it inherits -- as a plain value among the core's choices. The library's
page keeps both layers; its per-title layer shows values the same way. Editing
one title, the core's `title` value (n64lle: "what this title's game.toml
declares", the default of its tri-state options) is not offered, in the player
or the developer options: an unset value already means it, and shows as
**Default** (the hub does not guess what game.toml resolves it to). Reset to
Default clears the title's file. The page's footer has **Reset to Default** on
the left and **Cancel**, then **Save**, on the right, in the library too (as on the PlayStation and Super Nintendo pages).
**Developer page.** In Direct mode, a **local build** (any build but a CI
release: `build local` in `--version`) has a green **Developer** button at the
settings page's top right. It opens the core's developer-only options on a
page of their own (**Back** or B returns); they are no longer beside the
player's options, and a release build, or the library's page, does not show
them. Values stored for keys the core no longer declares stay on the System
tab. Each page's Reset to Default clears that page's options only. **Show
developer options** (on the Developer page) is saved to `config.json` as soon
as it is toggled (`show_developer_options`).

**Hotkeys (System tab, right).** The host's in-game shortcuts, the same for
every core and saved in `<data dir>/play.ini` (`[keys]`, `[combos]`) with the
page: a key for each of pause menu, save states, show FPS, turbo (held) and
volume up/down (defaults Esc, F7, F3, Tab, =, -; F1 and keypad +/- always
work), and a controller combo for each, **Function + a button** (defaults
Start, R1, L1, R2, D-Up, D-Down; turbo's combo toggles it, its key is held).
**Sound during turbo** (on by default, `play.ini` `turbo_sound`) keeps the
game's sound while turbo runs at its own pitch: it plays in stretches of
about 60 ms, faded at the joins, and what turbo makes in between is skipped
(the usual fast-forward sound); off mutes it, as turbo used to. Function replaces the old fixed Select: it
is an input each seat binds on its Configure page (Back / Select by default),
because some pads (N64 USB adapters among them) report Select oddly; while it
is held that seat sends the game no buttons. L3 + R3 also opens the pause
menu; the Guide button is never used (Steam and the OS usually keep it). Stick deadzones default to 10% on every console's page (n64lle,
PlayStation, Super Nintendo).

**Gamepads tab (N64).** Each seat card shows the controller (rendered from
`assets/src/n64_controller.svg`) above its device choice, and a **Pak** choice
under Configure: None, Transfer Pak (any seat), or VRU Microphone. A Transfer Pak adds a section
below the cards: **Choose ROM…** and **Choose save…** list the `.gb`/`.gbc`
and `.srm` files in the library's gb and gbc folders (`library_root` +
`platform_folders`, any file there, not only indexed titles), and **Create New
Save…** writes a blank `<name>.srm` beside the chosen ROM, sized from its
cartridge header (0xFF, as fresh battery RAM reads).

**VRU Microphone (N64).** Offered on a seat only when the described core
declares the `n64.vru` accessory for it (`accessory` records from
`--describe`, Retro-Runtime rcore rev 7); otherwise the entry is disabled and
its tooltip says why. The seat becomes the voice unit: it reads no controller
(its pad goes to the next Auto seat) and Configure is disabled. Its panel,
below the cards, picks the recording device (System default, or one SDL
lists), shows the live level, and has **Test** (the recognizer for six
seconds, against the title's vocabulary or any English without one), the
recognizer's state and **Download model**, and the title's vocabulary.
Recognition is Vosk on this machine (`third_party/vosk/NOTICE.md`): libvosk
is loaded at runtime from beside the hub, from the build's
`RETCOMM_VOSK_DIR`, from `<data dir>/vru/`, or through the system loader; the
English model (`vosk-model-small-en-us-0.15`, about 40 MB) is downloaded into
`<data dir>/vru/models/`. The vocabulary is the title's phoneme-to-text
table (schema 1, en-US: the file n64lle's `tools/vru_client.py` takes as
`--vocabulary`), made locally from the title's ROM and never checked in --
no tool in either repository writes it yet: `vru_vocabulary.json` beside
`game.toml`, or the file `game.toml`'s `[vru] vocabulary` names. In play, the
core's dictionaries arrive over the link's accessory data (link 2.1) and
the hub answers with `start` / `progress` / `result` / `cancel` at that seat. One
microphone serves every VRU seat (the first one's device). A VRU seat cannot
join a netplay match yet: the room says so and the match is refused until
that seat's Pak is None.

The page is built from what the core declares, asked of the runner with
`retro-core-runner --describe` (Retro-Runtime `docs/CORE_RUNNER.md`), never
from a list compiled into the hub. A runner without `describe 1` in its
`--version` still plays; the page then uses the cached description, or says
the core's settings are unavailable and labels the pad chips generically. Mods reads the title dir's `mods/` tree, in either layout
`include/retcomm/mods.hpp` names (n64lle writes its selection to
`<title dir>/mods.toml`).

**The runner.** Direct mode finds `retro-core-runner` the way the launcher does
(`resolve_runner`, `src/update/runtime_update.cpp`), in this order:

1. `--runner`, when given (revision 4; the hub exports it as `RETRO_CORE_RUNNER`
   for the rest of the process);
2. `RETRO_CORE_RUNNER`, when set, is used as is (the override);
3. otherwise the newest compatible one of `<directory of retro-hub>/retro-core-runner`
   (bundled) and `<data dir>/runtime/<version>/retro-core-runner` (installed
   by the runtime updater).

It logs which on stderr (`retro-hub: runner: bundled: <path> (bundled runner
0.3.0)`). When none is usable, or `--package` is given and the runner's
`--version` does not report `game_package 1` (runners from before Retro-Runtime
added `--package`), the window says so and waits for the player to close it;
the hub then exits 1. When `check_updates_on_startup` is on (the default), the
home page checks everything at launch and asks before installing
([Updates](#updates)); with `--boot` the runtime updater runs once in the
background instead: a newer runner installs beside the one in use and is used
from the next launch, never mid-session (`RETRO_CORE_RUNNER` skips it).

## Title-app mode

One title as its own app, a contract shared with n64lle's `n64lle_add_game_app()`. A
port's framework stages a **title payload** and the kit in
[`packaging/title/`](../packaging/README.md#title-apps) wraps it, this hub and
a runner into an AppImage, a `.app` in a `.dmg`, or a single portable `.exe`.
The app is a local build: its game package is ROM-derived generated code, so
it is never published, and the ROM is never in it.

```sh
retro-hub [--title <title.json | dir>] [--core <core>] [--package <shim>] [--runner <runner>]
          [--rom <image>] [--boot] [--opt key=value]... [--hub <retro-hub>] [--check-title]
```

**Finding the title.** `--title`; else `<exe dir>/title/title.json`; on macOS
also `<exe dir>/../Resources/title/title.json`. When a title is found,
`--run-core`/`--core` override its core rather than leaving title-app mode, so
a title app runs a dev core as itself. Direct mode (unchanged) is a hub with
no title: none passed, none beside it.

**`title.json`, schema 1** (`src/hub/hub_title.hpp`): `schema`, `id`
(`[a-z0-9_-]+`), `name`, `version`, `platform`, `core`, and optionally
`package`, `title_dir` (default: the package's directory), `opts`
(`["key=value", ...]`, under `--opt`), `rom` (`file_names`, `size`,
`sha256`, `label`), `boxart` (the title's cover, png or jpg) and `update`
(`{"github": "owner/repo"}`: where the port publishes releases, for the
[Update page](#updates)). Paths are relative to
`title.json` and may not leave its directory. Unknown keys are ignored (a hub
from before `update` ignores it); a greater `schema` is refused.

**The ROM**, in order: `--rom` (checked, and remembered); the remembered one, if
it still exists and still matches; each `rom.file_names` entry beside the app,
then in `<data dir>/roms/`. Otherwise the home page shows **Choose ROM…** (SDL3
`SDL_ShowOpenFileDialog`) with Play disabled until a ROM checks out. A ROM is
checked against `rom.size` and `rom.sha256`; a mismatch is **refused** and the
page (or `--check-title`, on stderr) shows expected against got. A static
recompilation of one image does not run another.

**The data directory** is `<dir of the app>/<id>-data/`, where the app is
`$APPIMAGE`, the portable `.exe` (`RETCOMM_PORTABLE_EXE`, exported by the
stub), the `.app` bundle, or else `retro-hub` itself. It is probed by writing a
file; when that fails (a mounted dmg, a read-only share) it is
`<user data>/<id>/` (`~/.local/share/<id>`, `%LOCALAPPDATA%\<id>`), and the log
says so. It holds everything the hub writes for the title: `config.json`,
`sessions/<id>/`, `saves/<id>/`, `platform/` (settings), `mods.toml`, the
remembered ROM (`rom.json`), and what the [Update page](#updates) installs
(`hub/`, `core/`, `runtime/`). Nothing is written inside the AppImage mount,
the extracted portable payload or the bundle. Nothing updates by itself: the
app runs what it was built with until the player presses **Update**.

**`RETRO_TITLE_STATE_DIR`.** The runner child gets
`RETRO_TITLE_STATE_DIR=<data dir>` in its environment (the runner passes its
environment to the core). The Mods page writes an n64lle selection to
`<data dir>/mods.toml` instead of `<title dir>/mods.toml`, and a core that
honours the variable reads it from there (n64lle's generic core does). Direct
mode does not set it.

**`--check-title`** resolves what a launch would and prints `key value` lines,
with no window: `title`, `id`, `core`, `package` (or `none`), `title_dir`,
`runner` (or `none`), `runner_version` (or `none`), `rom` (a path, `none`, or
`refused`) and `data_dir`. It exits 0 when title, core, package and runner
resolve; a missing ROM is not a failure, a `--rom` that does not match is. It
creates nothing and remembers nothing (the data dir is where it would be).

**`--hub <p>`** re-executes that hub with the same arguments minus `--hub`
(paths absolute), plus `--title <this title.json>` and `--runner <the runner
this hub would use>` unless `--runner` was given, and `RETRO_HUB_APP` so its
state stays beside this app. Nothing happens when `<p>` is this hub, and a hub
started that way (`RETRO_HUB_REEXEC` set) never re-executes again. A hub from
before title-app mode (no `title_app` in `--version`) would ignore `--title`
and open its library, so it is given the title as a Direct-mode command line
instead (`--run-core --package --title-dir --rom`, the runner in
`RETRO_CORE_RUNNER`) when its `direct_mode` is 2 or more and a ROM already
resolves; otherwise the hub refuses (exit 2). `--check-title` is never handed
to such a hub.

## Updates

The home page's **Update** button (Direct mode and title-app mode) opens a page
with one row per piece: what runs, what is published, and what can be done
(`src/hub/hub_update.hpp`). It checks when it opens; **Install updates**
installs every row that can be installed, and **Restart now** closes the window
and starts the app again once a new hub or core is in place. Nothing is ever
written over the app: everything lands in the data dir (a title app's own, or
Direct mode's), and each start picks the newest usable copy.

**At launch**, when **Check for updates on startup** is on (the checkbox at the
page's bottom left; `check_updates_on_startup` in `config.json`, saved as it is
toggled, on by default -- title apps included), the home page runs the same
check in the background. If anything can be installed, or the game has a newer
release, an **Updates available** prompt lists it: **Install & Restart**
installs everything installable in one go and restarts when that needs it (a
hub or core, or a dev path dropped); a failure opens the page instead.
**Details** opens the page; **Not now** closes the prompt. Nothing installs
without the player.

**Developer paths.** On a local build, with **Show developer options** on (the Developer page), the
Core, Runner and Hub rows get **Browse…** (the OS file picker), and the page's
bottom right **Reset to Defaults** and **Save & Restart**. A pick is checked as
a start would check it (a core needs its `.rcore.toml`, the title's core id and,
for a title with a game package, the package's module ABI -- read from the core's
own `n64_module_abi_version` in a child `retro-hub --probe-module-abi <core>`;
a runner must speak this link and ABI major; a hub must have Direct mode 4, and
title-app mode for a title app) and refused on the row otherwise. Nothing is
copied: Save & Restart writes the paths to `config.json` (`dev_core_path`,
`dev_runner_path`, `dev_hub_path`) and restarts, and every start then uses them
in place of the bundled or installed ones -- the core when title.json names it
(Direct mode's core is its command line's, so that Browse is disabled), the
runner through `RETRO_HUB_DEV_RUNNER` (after `--runner` and
`RETRO_CORE_RUNNER`; resolve_runner's source `dev`), the hub by handing over to
it as to an updated hub, from any build. Reset to Defaults empties the three
(Save & Restart applies it). A release always replaces a dev build: a row
running one offers the newest release whatever the versions say, and
installing it clears that row's dev path. A dev path that no longer exists, or
a dev hub that will not run, is logged and ignored.

**A hand-chosen core of another module ABI.** A core named by `--core` or
`dev_core_path` that is built for another module ABI than the game package
would only be refused at load ("The game stopped"). A start reads its module
ABI first; when it differs, the title's own core (bundled, or the newest
installed for this package) starts instead if that one matches, and the Core
row on the Update page says why. When no core matches, the chosen one is kept
and the row says the game has to be rebuilt for it. A core that exports no
module ABI, or a package that records none, is not second-guessed.

| Row | Checked against | Installed into | Used |
|---|---|---|---|
| Game | the newest release of title.json `update.github` | nothing: the game package is generated from the player's ROM and is never published. A newer release is reported with a link to it, to rebuild the app from. | -- |
| Core | n64lle's `n64lle-release-manifest.json` ([n64lle docs/RELEASES.md](https://github.com/RetroPortingToolKit/n64lle/blob/main/docs/RELEASES.md) §5), for a title whose core is n64lle's generic core | `<data dir>/core/<version>/`: the core library, its `.rcore.toml` and `installed.json` (module ABI), taken from the release prefix | after a restart |
| Runner | Retro-Runtime's `runtime-manifest.json` (the runtime updater, unchanged rules) | `<data dir>/runtime/<version>/` | from the next Play |
| Hub | this repo's `hub-manifest.json` (below), Linux only for now | `<data dir>/hub/<version>/`, the bare hub archive | after a restart |

Every manifest is the **newest release's, pre-releases included**, found from
`https://github.com/<repo>/releases.atom` (n64lle docs/RELEASES.md §2), with the
REST API as the fallback a `GITHUB_TOKEN` can reach a private repository
through. `RETRO_HUB_MANIFEST_URL`, `RETRO_CORE_MANIFEST_URL` and
`RETRO_RUNTIME_MANIFEST_URL` name another (file:// included, for tests). Each
archive's size and SHA-256 are checked before it is opened, and what was
extracted must itself report the manifest's version (and commit, for the hub
and runner) before it is moved into place. The newest two versions of each are
kept.

What is refused, and said on the row rather than installed:

- **A core** named by `--run-core`/`--core` (Direct mode always), a core with
  no game package (it is the game), a core whose rcore ABI major or draft
  revision differs from this hub's, or whose `module_abi` differs from the one
  the game package records (`<shim>.n64game.toml`): that core refuses this
  package at load, and the game has to be rebuilt first. A start uses an
  installed core only when its sidecar id and `installed.json` module ABI match
  the title's core and package.
- **A hub** whose link-protocol or rcore ABI major differs from this one's,
  whose Direct mode revision is below 4, or that has no Update page
  (`direct_mode.updates` in its manifest, `updates` in its `--version`):
  running it would leave the app no way to update again. In Direct mode (no
  title), a development build -- any build but the CI release
  (`build local` in `--version`, which `scripts/build-local.sh` makes too,
  commit and all) -- is never replaced: it shares its data dir with the
  released launcher.
- **A runner**, as the runtime updater always has, and not at all while
  `RETRO_CORE_RUNNER` or `--runner` names one.

**Handing over to an updated hub.** At start, a hub that finds a newer hub in
`<data dir>/hub/` (with `updates`, and `title_app` for a title) runs it the way
`--hub` does: same arguments, `--title` for a title app, `RETRO_HUB_APP` so the
data dir stays beside the app. It does not pass `--runner`; it exports
`RETRO_HUB_BUNDLED_RUNNER=<its own bundled runner>`, which the updated hub
counts as bundled, so a runner update still wins. `--check-title`, `--hub`, a
hub already started by another (`RETRO_HUB_REEXEC`) and, in Direct mode, a
local build (`build local`) never hand over. On
Linux this is `execv`, so an AppImage stays mounted. **Restart now** starts the
first hub again (`RETRO_HUB_ENTRY`, with the environment it was started in),
which picks the newest of everything once more.

## The bare hub archive

For a tool that runs the hub in Direct mode, such as n64lle's new-project
scaffolder in "release" mode. So far only the Linux job packages one; on
Windows and macOS the hub (with Direct mode) ships inside the installers.

Everything is flat at the archive root, so it extracts straight into one
directory:

| Path | Why it is there |
|---|---|
| `retro-hub` | The hub. |
| `libSDL3.so.0` | The hub links SDL3 dynamically and cannot start without it; found through the hub's `RUNPATH` `$ORIGIN`. The same pinned SDL3 the AppImage bundles. |
| `fonts/` | Lato UI face and its OFL notice. Found beside the executable; without it the hub falls back to ImGui's default font. |
| `retcomm.png`, `platforms/`, `controllers/`, `setup/` | Window icon and library-UI art, found beside the executable, as in the AppImage. Direct mode does not use them. |
| `LICENSE` | Retro Launcher's licence. |
| `licenses/` | SDL3, and what is linked statically into `retro-hub`: Dear ImGui, miniz, stb, nlohmann/json, Retro-Runtime. |
| `packaging/title/`, `packaging/common/` | The title-app kit ([`packaging/README.md`](../packaging/README.md#title-apps)); it finds the hub at `../..`. |

**It does not contain `retro-core-runner`.** Take it from
[Retro-Runtime's release](https://github.com/RetroPortingToolKit/Retro-Runtime/blob/main/docs/RELEASES.md)
and put it beside `retro-hub`, or name it with `RETRO_CORE_RUNNER` (see
[the runner](#direct-mode) above). With `--package`, it must be a runner that
reports `game_package 1`.

The hub also needs from the machine: the GL driver (`libOpenGL`/`libGLX` or
`libGL`), `libcurl.so.4`, `libstdc++` no older than `requires.glibcxx`, and a
glibc no older than `requires.glibc` (`libfreetype.so.6` too, if the build
found FreeType; the release build on Ubuntu 22.04 does not). The manifest lists
exactly what the archived binaries import as `system_libraries`.

`scripts/package_hub_archive.sh` builds it from the install prefix the release's
Linux job already builds for the AppImage, and fails the job, rather than
warning, when:

- the archive holds a file not on its allowlist, or holds `retro-core-runner`;
- the hub or the bundled SDL3 imports a library that is neither in the archive
  nor on the system allowlist, or the hub has no `$ORIGIN` `RUNPATH`;
- the hub **inside the archive**, extracted to a clean directory, reports a
  different version or commit (`--version`), was built without Direct mode,
  resolves SDL3 from anywhere but the archive, or does not refuse
  `--run-core` without `--rom` (exit 2);
- the title-app kit is missing, or its `build-title-app.sh --help` does not run
  from the extracted archive, or the hub reports no `title_app`.

## The manifest

The newest non-prerelease manifest is always at:

```
https://github.com/RetroPortingToolKit/Retro-Launcher/releases/latest/download/hub-manifest.json
```

and a specific release's is at `…/releases/download/v<version>/hub-manifest.json`.
Its shape follows Retro-Runtime's `runtime-manifest.json`:

```json
{
  "schema": 1,
  "name": "retro-hub",
  "version": "0.1.2",
  "tag": "v0.1.2",
  "commit": "<40-hex>",
  "published_utc": "2026-09-26T04:01:18Z",
  "link_protocol": { "major": 1, "minor": 0 },
  "rcore_abi": { "major": 0, "draft_revision": 5 },
  "direct_mode": {
    "cli_revision": 4,
    "updates": 1,
    "flags": ["--run-core", "--package", "--rom", "--title-dir", "--tpak1-rom", "--tpak1-save", "--no-gl", "--opt", "--boot", "--title", "--core", "--runner", "--hub", "--check-title"],
    "runner_lookup": "RETRO_CORE_RUNNER exe_dir/retro-core-runner data_dir/runtime/<version>/retro-core-runner"
  },
  "platforms": {
    "linux-x86_64": {
      "url": "https://github.com/RetroPortingToolKit/Retro-Launcher/releases/download/v0.1.2/retro-hub-0.1.2-linux-x86_64.tar.gz",
      "archive": "retro-hub-0.1.2-linux-x86_64.tar.gz",
      "sha256": "<64-hex>",
      "size": 4959744,
      "executable": "retro-hub",
      "files": ["retro-hub", "libSDL3.so.0", "fonts/LatoLatin-Regular.ttf", "…", "LICENSE", "licenses/SDL3.txt", "…"],
      "requires": { "glibc": "<newest GLIBC_ symbol version>", "glibcxx": "<newest GLIBCXX_>" },
      "system_libraries": ["libGLX.so.0", "libOpenGL.so.0", "libc.so.6", "libcurl.so.4", "…"]
    },
    "linux-arm64":    { "unavailable": "not built yet" },
    "windows-x86_64": { "unavailable": "no bare archive is built for this platform yet; …" },
    "macos-arm64":    { "unavailable": "no bare archive is built for this platform yet; …" },
    "macos-x86_64":   { "unavailable": "no bare archive is built for this platform yet; …" }
  }
}
```

`schema` changes only on an incompatible edit to this format. Fields may be
added, so a reader ignores fields it does not know. Every contract value was
read out of the hub inside the archive (`--version`), not typed into the
workflow; `requires` is measured from the symbol versions of `retro-hub` and
the bundled SDL3.

- `link_protocol` is the hub ↔ runner link the hub speaks
  (Retro-Runtime `corelink/link_protocol.hpp`). A runner is compatible when its
  `link_protocol.major` is equal; the minor is negotiated per session.
- `rcore_abi` is the core ABI the hub was built against
  (Retro-Runtime `include/rcore/rcore.h`). The runner, not the hub, loads cores.
- `direct_mode.updates` is the hub's [Update page](#updates) revision (`updates`
  in `--version`); absent before it. A hub is only installed by, and handed
  over to from, another hub when it has it.
- `direct_mode.cli_revision` goes up whenever the set of Direct mode flags
  changes (the table under [Direct mode](#direct-mode) says which revision
  introduced each). A tool requires at least the revision that introduced the
  flags it passes, e.g. 2 for `--package`. Each revision still accepts every
  earlier revision's command line; a change that breaks that will be called out
  here.
- **Revision 3 changed what an earlier command line does:** it is still
  accepted, but it opens the home page instead of playing at once. A tool that
  needs the old behaviour passes `--boot`, and so needs revision 3.
- **Revision 4** adds title-app mode (`--title --core --runner --hub
  --check-title`, and `title_app 1` in `--version`). An earlier command line
  does what it did, with one difference: a hub with `title/title.json` beside
  it and no `--run-core` now opens that title instead of the library.

## `retro-hub --version`

One `key value` per line, exit 0, printed before SDL starts (no display needed):

```
retro-hub 0.1.2
version 0.1.2
commit 8f538f7218855e94cdcba437a23245a4e1be22f8
build release
link_protocol 1.0
rcore_abi_major 0
rcore_draft_revision 5
direct_mode 4
direct_mode_flags --run-core --package --rom --title-dir --tpak1-rom --tpak1-save --no-gl --opt --boot --title --core --runner --hub --check-title
title_app 1
updates 1
runner_lookup RETRO_CORE_RUNNER exe_dir/retro-core-runner data_dir/runtime/<version>/retro-core-runner
```

`title_app` is the title-app mode revision (and the `title.json` schema it
reads); a hub without the line predates it. `updates` is the
[Update page](#updates)'s revision; the manifest carries it as
`direct_mode.updates`, and a hub without it is never installed by one. A build that was not given
`RETCOMM_COMMIT` says `commit unknown`. `build` is `release` for a CI release
(CMake `-DRETCOMM_RELEASE_BUILD=ON`, set only by the release workflow) and
`local` for every other build; only a local build offers Direct mode's
Developer page and developer paths. A build
without the hub <-> runner link would print only the first three lines and
`direct_mode 0`; every current build has it.

## Using it, for a tool

1. Fetch the manifest. If `schema` is not one you know, stop.
2. Take `platforms[<this platform>]`. If it has `unavailable`, report the reason.
3. Check `direct_mode.cli_revision` and `flags` against what you will pass
   (`--package` needs revision 2), and `link_protocol.major` against the
   runner you will place beside it (with `--package`, that runner must report
   `game_package 1`).
4. Check the machine against `requires.glibc` (`gnu_get_libc_version()`).
5. Download `url`; check `size` and `sha256` **before** extracting.
6. Extract into a fresh directory, run `<dir>/retro-hub --version`, and require
   its `version` and `commit` to match the manifest.
7. Put `retro-core-runner` from Retro-Runtime's release at `<dir>/retro-core-runner`,
   or run the hub with `RETRO_CORE_RUNNER=<path to it>`.
