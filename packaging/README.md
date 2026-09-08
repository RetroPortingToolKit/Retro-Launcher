# RetComM packaging

Helpers used by [`.github/workflows/release.yml`](../.github/workflows/release.yml).

Release assets:

| Platform | Artifact |
|---|---|
| Linux | `RetComM-Launcher-linux-x86_64.AppImage` |
| macOS | `RetComM-Launcher-macos-{arm64,x86_64}.dmg` |
| Windows | `RetComM-Launcher-windows-x64-setup.exe` + `RetComM-Launcher-portable-windows.zip` |

Filenames are stable across releases (version lives in the GitHub tag / binary
`RETCOMM_VERSION`). The portable zip contains `RetComM Launcher.exe` (friendly
desktop name). Self-update replaces AppImage / portable in place and keeps the
user's path, so a versioned download name would go stale.

## Icons

```sh
./packaging/make-icons.sh
```

Writes `assets/retcomm.png` at **512×512** (linuxdeploy’s max allowed size;
1024 is rejected) and `.ico` when ImageMagick is available, from
`assets/retcomm.svg`.

Hub UI fonts live in `assets/fonts/` (Lato Latin, same face as recomp-ui) and
install to `share/retcomm/fonts`. Platform controller icons live in
`assets/platforms/` (`psx.png`, …) → `share/retcomm/platforms`. PSX DualShock
pad art for the Gamepads configure mapper lives in `assets/controllers/`
(`pad_analog.png`, `pad_digital.png`) → `share/retcomm/controllers`. Packaging
**requires** all three:

| Artifact | Font location | Platform icons | PSX pad art |
|---|---|---|---|
| Linux AppImage | `usr/share/retcomm/fonts` + `usr/bin/fonts` | `usr/share/retcomm/platforms` + `usr/bin/platforms` | `usr/share/retcomm/controllers` + `usr/bin/controllers` |
| macOS `.app` / DMG | `Contents/Resources/fonts` | `Contents/Resources/platforms` | `Contents/Resources/controllers` |
| Windows setup / portable | `fonts/` next to the exes | `platforms/` next to the exes | `controllers/` next to the exes |

On Windows, `retcomm-hub` links `/SUBSYSTEM:WINDOWS` (no console window);
`retcomm.exe` remains a console CLI.

CI fails the release job if `LatoLatin-Regular.ttf` is missing from the install
prefix or the final package. Windows/macOS/Linux packagers also require
`platforms/psx.png` and `controllers/pad_analog.png`.

## Linux

### Local AppImage (one shot)

```sh
./packaging/linux/build-local-appimage.sh
# optional version override:
./packaging/linux/build-local-appimage.sh 0.1.2
```

Builds icons, SDL3 (cached under `.cache/sdl3` if not already installed), RetComM,
and writes `dist/RetComM-Launcher-linux-<arch>.AppImage`.

```sh
./dist/RetComM-Launcher-linux-*.AppImage
./dist/RetComM-Launcher-linux-*.AppImage cli list
# if FUSE is unavailable:
./dist/RetComM-Launcher-linux-*.AppImage --appimage-extract-and-run
```

### Manual / CI-style steps

```sh
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/out"
cmake --build build -j && cmake --install build
./packaging/linux/build-appimage.sh "$PWD/out" 0.1.1 x86_64
```

Self-update replaces the running AppImage in place (`APPIMAGE` env). Dev
binaries / loose copies under `~/.local/share/retcomm/bin` are not updatable —
Menu → Update RetComM stays disabled with a hint to launch the AppImage.

When launching a title, RetComM strips AppImage `LD_LIBRARY_PATH` / `APPDIR`
from the child environment so native recomp binaries load their own (or system)
libs instead of the launcher’s bundled SDL.

## macOS

```sh
./packaging/macos/build-app.sh "$PWD/out" 0.1.1 arm64
```

Produces under `dist/`:

| Artifact | Role |
|---|---|
| `RetComM Launcher.app` | App bundle (local staging) |
| `RetComM-Launcher-macos-<arch>.dmg` | Drag-to-Applications installer disk image |

Open the DMG and drag **RetComM Launcher** onto **Applications**. That puts it on
Launchpad, Spotlight, and the Applications folder. Self-update downloads the
matching arch DMG and refreshes the running `.app` only (dev `.app`-less builds
cannot self-update).

`build-app.sh` calls `bundle_dylibs.sh` to copy Homebrew dylibs (SDL3, FreeType,
libpng, …) into `Contents/Frameworks` and rewrite nested install names to
`@rpath` / `@loader_path`. The packager **fails** if any `/usr/local` or
`/opt/homebrew` load command remains. Ad-hoc `codesign` is applied so arm64
accepts the rewritten binaries; downloaded DMGs still need a Developer ID +
notarization to skip Gatekeeper.

## Windows

Requires a Release build that includes `retcomm`, `retcomm-hub`, and
`retcomm-portable` (the stub). Optional: [Inno Setup 6](https://jrsoftware.org/isinfo.php)
on `PATH` (or pass `-InnoSetup`) to build the Start Menu installer.

```powershell
./packaging/make-icons.sh   # from Git Bash / WSL, for assets/retcomm.ico
cmake --build build --config Release
cmake --install build --config Release --prefix out
./packaging/windows/package.ps1 -Prefix out -Version 0.1.1 -VcpkgBin path\to\vcpkg\bin -Arch x64
```

### Dev test zip, cross-compiled from Linux

For handing a build to a Windows tester without a Windows machine or a release
run. Needs mingw-w64 + the cross libs (Arch: `mingw-w64-gcc mingw-w64-curl
mingw-w64-zlib`); SDL3 has no mingw package, so the script cross-builds it once
into `.cache/sdl3-mingw`.

```sh
./packaging/windows/build-dev-zip.sh              # version → <CMake version>-dev.<sha>
./packaging/windows/build-dev-zip.sh 0.6.4        # explicit label
PORTABLE=1 ./packaging/windows/build-dev-zip.sh   # also the single-exe stub
STRIP=0 ./packaging/windows/build-dev-zip.sh      # keep symbols in the zip
```

Writes `dist/RetComM-Launcher-windows-x64-dev.zip` — unzip on Windows, run
`retcomm-hub.exe`. Layout matches the release package (exes + DLLs, with
`fonts/`, `platforms/`, `controllers/`, `setup/` beside them); the DLL set is the
import closure of both exes walked with `objdump`, and the toolchain links
libgcc/libstdc++ statically. `cmake/toolchain-mingw-w64-x86_64.cmake` drives it
and can be used on its own for a plain cross build.

How a dev zip differs from a release:

| | Dev zip | Release |
|---|---|---|
| Toolchain | mingw-w64 (GCC) | MSVC + vcpkg |
| Signing | none — SmartScreen warns | Authenticode when the cert secret is set |
| Self-update | disabled (no `channel.json`) | portable / installer channel |
| ImGui color emoji | off (no mingw FreeType) | on |

Test the CLI under wine (`wine retcomm.exe status`) for a quick link check, but
wine ships no `tar.exe` or `powershell.exe`, so core archive extraction — catalog
sync, installs — always fails there. That is a wine gap, not a build defect;
Windows 10+ ships `tar.exe`.

Produces under `dist/`:

| Artifact | Role |
|---|---|
| `RetComM Launcher.exe` | Single-file portable stub + payload (local; not the GitHub asset) |
| `RetComM-Launcher-portable-windows.zip` | Release zip; root entry is `RetComM Launcher.exe` (no nested folder) |
| `RetComM-Launcher-windows-x64-setup.exe` | Per-user Inno Setup installer (Start Menu / desktop) |

### Code signing

Windows 11 Smart App Control blocks any executable that is neither signed nor
already known by hash, so a fresh unsigned release cannot be run on such a
machine. `package.ps1` signs when a certificate is in the environment and
otherwise packages unsigned with a notice:

| Variable | Meaning |
|---|---|
| `WINDOWS_SIGN_PFX_BASE64` | PKCS#12 certificate, base64 (`base64 -w0 cert.pfx`); a repo Actions secret in CI |
| `WINDOWS_SIGN_PFX_PASSWORD` | its password (omit for a passwordless .pfx) |
| `WINDOWS_SIGN_TIMESTAMP_URL` | optional RFC 3161 server (default DigiCert) |
| `WINDOWS_SIGN_DESCRIPTION` | optional text for the file properties / UAC prompt |

Signed: `retcomm.exe`, `retcomm-hub.exe`, every staged DLL, the combined
`RetComM Launcher.exe` (after its payload is appended; the stub finds the
RCM1 trailer before the certificate table), `retcomm-hub`'s uninstaller and
the Inno Setup installer. The certificate is imported into the user store for
the run and used by thumbprint, so the password never reaches a command line.
Signing needs `signtool.exe` (Windows SDK; the CI runner has it). A configured
certificate that fails to sign stops the package.

### Antivirus false positives

Unsigned releases were being flagged and quarantined by Microsoft Defender
(`Trojan:Win32/Wacatac.B!ml` and friends). The `!ml` suffix means an ML
heuristic, not a signature match. Four things fed it; three are fixed:

- **No Authenticode signature, so no download reputation.** Still the dominant
  factor; see "Code signing" above. Note that `WINDOWS_SIGN_PFX_BASE64` cannot
  be used with a newly issued certificate: since June 2023 publicly trusted
  code-signing keys must live on FIPS 140-2 hardware, so no exportable `.pfx`
  exists to put in a secret. A cloud signing service (Azure Artifact Signing,
  DigiCert KeyLocker, SSL.com eSigner) needs a `signtool /dlib` path here
  instead.
- **The portable stub looked like a dropper.** It wrote `payload.zip` to disk
  and unpacked it by spawning `System32\tar.exe`, falling back to
  `powershell -ExecutionPolicy Bypass -Command "Expand-Archive ..."`. Self-
  extracting to disk and then driving a system interpreter is close to a
  textbook dropper signature. It now inflates the payload in-process with a
  vendored miniz (`third_party/miniz/`) and spawns nothing before the hub
  itself. See `src/portable/win_portable_main.cpp`.
- **No version metadata.** The `.rc` carried only an icon, so the binaries had
  no `CompanyName` / `ProductName` / `OriginalFilename` for SmartScreen to
  attribute a download to. Each exe now gets its own `VERSIONINFO` block via
  `retcomm_add_windows_rc()` in `CMakeLists.txt`, and setup.exe gets the
  matching `VersionInfo*` directives in `setup.iss` (Inno was otherwise
  stamping its own compiler version and leaving the copyright blank). The
  uninstaller inherits company/product/copyright but keeps Inno's own
  FileVersion, which the script cannot override.
- **The hub unpacked downloads the same way the stub did.** Catalog sync and
  every prebuilt install ran `powershell -ExecutionPolicy Bypass -Command
  Expand-Archive`, far more often than the stub's one-time unpack. Zip now
  goes through the shared in-process reader on every platform; `tar.exe`
  remains for `tar.*`/`7z` and as a fallback. Helper processes are also
  resolved to absolute paths now (`win_system_exe` / `win_find_on_path`):
  spawning `tar.exe` by bare name let the calling exe's directory and the
  current directory outrank System32, which was a binary-planting hole as
  well as a heuristic.

Still open: `src/update/self_update.cpp` applies updates through a hidden
`powershell -EncodedCommand`, which is its own behavioural-detection trigger;
`data_root_migrate.cpp` shells out to `cmd.exe /C mklink`; and the portable
payload is still an appended overlay rather than a PE resource.

When a release is flagged anyway, dispute it as the developer at
<https://www.microsoft.com/wdsi/filesubmission> (choose "Software developer").
Turnaround is usually a few days. Each build is a new hash, so a cleared
verdict does not carry to the next release — signing is what ends the cycle.

Portable usage (after unzipping the release zip):

```text
RetComM Launcher.exe           # hub UI
RetComM Launcher.exe cli list  # CLI
```

Self-update channels (detected via `channel.json` / env from the portable stub):

- **installer** (primary) → downloads `RetComM-Launcher-windows-*-setup.exe`, silent Inno into the current install dir
- **portable** → downloads `RetComM-Launcher-portable-windows.zip`, extracts the stub, replaces the desktop exe, re-extracts on next launch

The portable stub keeps everything beside the `.exe` — `RetComM-Data\runtime\`
for the unpacked hub/CLI and `RetComM-Data\{config,data}\` for the toolchain,
engines, installs and caches — and exports `RETCOMM_HOME` so the children resolve
the same folder. When the exe directory is not writable (a network share, a
read-only stick, Downloads under Mark-of-the-Web) it falls back to the historical
`%LOCALAPPDATA%\retcomm\portable\current\`. The update script clears the
`version.txt` stamp in both layouts so the new stub always re-extracts.

Apply scripts/logs (Windows PowerShell; Unicode-safe path args):
`%LOCALAPPDATA%\retcomm\self-update\bin\apply_setup_update.ps1` /
`apply_setup_update.log`, or `apply_portable_update.ps1` /
`apply_portable_update.log`. The `.ps1` is written for diagnostics only —
apply runs via `powershell -EncodedCommand` (not `-File`) so Group Policy
Restricted ("running scripts is disabled on this system") cannot block it.
Spawn handshake diagnostics: `apply_spawn.log` plus a `.ready` marker. The hub
prefers a script-written marker, then falls back to writing the marker itself
after ~1.5s if the child is still alive (avoids deadlock with a leftover
pre-Ready apply script that waits on the hub PID). Installer apply aborts if
the hub PID is still alive after 120s (avoids Inno fighting locked files).

Loose/dev copies without `channel.json` cannot self-update.
