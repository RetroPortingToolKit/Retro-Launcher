# Android builds

The **Android** Actions workflow builds an ARM64 release APK on pushes to
`main`, pull requests, and manual dispatch. The main **Release** workflow
calls the same build and publishes `Retro-Launcher-android-arm64.apk` alongside
the desktop installers. Android 11 (API 30) and OpenGL ES 3 are required.

For each CI build, **latest Retro-Runtime** means the HEAD of its default
branch at checkout time. The workflow checks out that source separately, then
compiles both the launcher's runtime libraries and `retro-core-runner` against
the same checkout. It does not download a prebuilt desktop runner or silently
fall back to the launcher's submodule pin. An incompatible upstream change
fails the build. The exact launcher, runtime, and SDL commits are recorded in
`assets/retcomm/build-arm64-v8a.json` inside the APK and in the CI artifact's
`android-build.json`.

The runner is **bundled in the APK and remains a child process**. The NDK
builds it as a PIE executable named `libretro-core-runner.so` so Android's
package manager installs it in the native library directory. The launcher
starts that installed copy through `RETRO_CORE_RUNNER`; it is never extracted
into writable app storage and then executed. The APK verifier checks the
architecture and executable program header as well as required libraries and
resources. Launcher and runner updates happen by installing a newer APK.

## Release signing

Configure these repository Actions secrets before running **Release**:

| Secret | Value |
|---|---|
| `ANDROID_KEYSTORE_BASE64` | Base64-encoded release keystore |
| `ANDROID_KEYSTORE_PASSWORD` | Keystore password |
| `ANDROID_KEY_ALIAS` | Signing key alias |
| `ANDROID_KEY_PASSWORD` | Signing key password |

Keep the same signing key for subsequent releases so Android accepts upgrades.
The release workflow requires signing and verifies the resulting signature.
Standalone CI builds without secrets produce an explicitly named
`Retro-Launcher-android-arm64-unsigned.apk`; this must be signed before
installation. No temporary debug key is used for published releases.

The Android version code is `major * 1000000 + minor * 1000 + patch`.
Minor and patch components must be below 1000. A prerelease suffix shares the
base version's code; bump the numeric version for a normal upgrade.

## Local build (Linux)

Install JDK 17, CMake 3.24+, Ninja, Make, Perl, Python 3, and the Android SDK
with platform 36, build tools 36.0.0, and NDK 29.0.14206865. The Gradle wrapper
pins Gradle 9.0.0. SDL is built from the same pinned source as its Java bridge.

For a debug APK, one command does everything below:

```sh
tools/build-android-debug.sh                          # arm64-v8a
tools/build-android-debug.sh --abi x86_64 --install emulator-5554
```

It finds the SDK holding the pinned NDK, picks JDK 17 when the default `java`
is newer than Gradle supports, uses or clones the pinned SDL, builds the
native payload and the APK, and runs the APK verifier. `--install [SERIAL]`
installs and starts it; a serial is required when more than one device is
attached. The debug APK's application id is
`org.retroportingtoolkit.launcher.debug` and its label is "Retro Launcher
(debug)", so it installs beside a release install rather than being refused
over it. It is debuggable, so `adb shell run-as
org.retroportingtoolkit.launcher.debug` reaches its private files.

The steps by hand:

```sh
git clone --branch release-3.2.16 --depth 1 https://github.com/libsdl-org/SDL.git .cache/android/SDL
export ANDROID_HOME="$HOME/Android/Sdk"
export RETCOMM_RUNTIME_DIR="$HOME/Documents/GitHub/Retro-Runtime"
bash scripts/build-android.sh
cd android
./gradlew :app:assembleRelease -PretroVersion=0.1.1 -PretroVersionCode=1001
```

Gradle refuses to run if `ANDROID_HOME` and `ANDROID_SDK_ROOT` name different
SDKs; unset whichever one does not hold the NDK above.

`RETCOMM_RUNTIME_DIR` defaults to the submodule for local builds; naming the
sibling checkout uses it without modifying either repository. To use an
existing SDL checkout, set `SDL_SOURCE_DIR`. The build downloads pinned curl
and OpenSSL source archives, checks their SHA-256 hashes, and cross-compiles
them. Intermediate output is in `.cache/android/` and `build-android-<ABI>/`.

For a signed local release, set `ANDROID_KEYSTORE` to an absolute keystore
path and set the three password/alias variables listed above. For a local
debug-signed build by hand, run `./gradlew :app:assembleDebug`. The native
payload is still built with release optimizations. Set `ANDROID_ABI=x86_64`
for the native build and pass `-PretroAbi=x86_64` to Gradle to test on an
emulator.

## Installing a title

A game runs through a **core** built for Android (n64lle's
`tools/build_core.sh --platform android-arm64`), and a generic core needs the
title's **game package** built for Android too (n64lle's cross build,
`tools/emit_game.sh --arch arm64 --arm android`). Both are ROM-derived local
builds, never published, so they reach a phone as a **title bundle**: a zip
holding a schema 1 `title.json` (docs/RELEASES.md, "Title-app mode") at its
root or in one folder, with the core, its `.rcore.toml`, the game package and
`game.toml` beside it.

**Add/Scan Files → Install title (.zip)** opens Android's file picker. The hub
copies the pick out of its `content://` URI (SDL's stream), unpacks it into
`<data dir>/titles/<id>/` (replacing an earlier install of that id), checks
that `title.json` reads and that everything it names is inside the bundle,
registers it in `config.json` `core_titles`, and scans for its ROM. **Import
ROM** takes the ROM the same way, named by the provider's display name
(`LauncherActivity.displayName`, over JNI), into the library's platform folder.
Both work the same on a desktop, from a path.

The core and the package are `dlopen`ed by the bundled runner from that app
storage. Measured 2026-10-09 on a Pixel 6 Pro (Android 17, targetSdk 36):
Pokemon Stadium installed this way, its ROM imported from Downloads, played
from the library on the Vulkan renderer (Mali-G78) with no fault.

What this needed in the shared code:

- **No `--gl` without GL.** The Android runner has no SDL3 and reports `gl 0`;
  a session asks for a GL context only when the runner reports one
  (`hub_play.cpp` `runner_lends_gl`), so a play no longer exits on `--gl`.
  n64lle then draws with Vulkan, or software, and says which.
- **TLS on Android** (72455fb, `http_apply_tls_trust`): Android names its CA
  files by OpenSSL's old subject hash and OpenSSL 3 looks a CApath up by the
  new one, so nothing verified until the system store was loaded into every
  handle. The catalog, box art and the netplay NAT probe all ride on it.

Registered core titles also join the library when no catalog could be loaded
(a first start without network), instead of being skipped with it.

## Platform scope

This introduces the Android launcher build and packaging path. Desktop game
executables and toolchain packs do not become Android-compatible: titles need
Android native cores. The hub uses GLES 3; the standalone runner has no Java
Activity, reports `gl 0`, and cannot lend a desktop OpenGL context to a core.
Software-presenting cores can use the existing shared-memory frame path.
Runner netplay requires an explicit `RETRO_RUNTIME_RECOMP_NET_DIR` checkout;
the default Android CI build does not enable it.

Settings, saves, and copied UI resources live in app-private storage. Existing
desktop file-manager, executable self-update, and host compilation flows are
not substitutes for Android storage/import or package installation APIs.
Device gameplay and storage workflows need separate validation; successful
compilation and APK verification alone do not establish them.
