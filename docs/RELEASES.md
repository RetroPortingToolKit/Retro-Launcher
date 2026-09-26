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

## The bare hub archive

For a tool that runs the hub in Direct mode, such as n64lle's new-project
scaffolder in "release" mode:

```sh
retro-hub --run-core <core.so> --rom <image> --title-dir <dir> \
          [--tpak1-rom <gb rom>] [--tpak1-save <sav>] [--no-gl] [--opt key=value]...
```

Direct mode boots straight into the core in the hub's window and exits when the
player closes it; no library pages, no setup wizard. Direct mode is built on
every OS, but so far only the Linux job packages a bare archive; on Windows and
macOS the hub (with Direct mode) ships inside the installers.

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

**It does not contain `retro-core-runner`.** Take it from
[Retro-Runtime's release](https://github.com/RetroPortingToolKit/Retro-Runtime/blob/main/docs/RELEASES.md).
Direct mode finds the runner the way the launcher does (`resolve_runner`,
`src/update/runtime_update.cpp`), in this order:

1. `RETRO_CORE_RUNNER`, when set, is used as is (the override);
2. otherwise the newest compatible one of `<directory of retro-hub>/retro-core-runner`
   (bundled) and `<data dir>/runtime/<version>/retro-core-runner` (installed
   by the runtime updater).

It logs which on stderr (`retro-hub: runner: bundled runner 0.3.0`, ...).

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
  `--run-core` without `--rom` (exit 2).

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
    "cli_revision": 1,
    "flags": ["--run-core", "--rom", "--title-dir", "--tpak1-rom", "--tpak1-save", "--no-gl", "--opt"],
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
- `direct_mode.cli_revision` changes on any incompatible change to the Direct
  mode flags; adding a flag appends to `flags` without changing it.

## `retro-hub --version`

One `key value` per line, exit 0, printed before SDL starts (no display needed):

```
retro-hub 0.1.2
version 0.1.2
commit 8f538f7218855e94cdcba437a23245a4e1be22f8
link_protocol 1.0
rcore_abi_major 0
rcore_draft_revision 5
direct_mode 1
direct_mode_flags --run-core --rom --title-dir --tpak1-rom --tpak1-save --no-gl --opt
runner_lookup RETRO_CORE_RUNNER exe_dir/retro-core-runner data_dir/runtime/<version>/retro-core-runner
```

A build that was not given `RETCOMM_COMMIT` says `commit unknown`. A build
without the hub <-> runner link would print only the first three lines and
`direct_mode 0`; every current build has it.

## Using it, for a tool

1. Fetch the manifest. If `schema` is not one you know, stop.
2. Take `platforms[<this platform>]`. If it has `unavailable`, report the reason.
3. Check `direct_mode.cli_revision` and `flags` against what you will pass, and
   `link_protocol.major` against the runner you will place beside it.
4. Check the machine against `requires.glibc` (`gnu_get_libc_version()`).
5. Download `url`; check `size` and `sha256` **before** extracting.
6. Extract into a fresh directory, run `<dir>/retro-hub --version`, and require
   its `version` and `commit` to match the manifest.
7. Put `retro-core-runner` from Retro-Runtime's release at `<dir>/retro-core-runner`,
   or run the hub with `RETRO_CORE_RUNNER=<path to it>`.
