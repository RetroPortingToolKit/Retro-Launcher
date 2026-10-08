#!/usr/bin/env bash
# Build a debug-signed launcher APK on Linux, and optionally install and start it.
#
#   tools/build-android-debug.sh [--abi arm64-v8a|x86_64] [--install [SERIAL]]
#
# The debug APK's application id is org.retroportingtoolkit.launcher.debug, so
# it installs beside a release install instead of being refused over it (the
# signing keys differ). It is debuggable: `adb shell run-as <id>` reaches its
# private files. The native payload is still built with release optimizations.
#
# Environment (all optional): ANDROID_HOME, JAVA_HOME, SDL_SOURCE_DIR,
# RETCOMM_RUNTIME_DIR (default: the Retro-Runtime submodule), JOBS.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_ID=org.retroportingtoolkit.launcher.debug
SDL_TAG=release-3.2.16
SDL_COMMIT=c9a6709bd21750f1ad9597be21abace78c6378c9

die() { echo "build-android-debug: $*" >&2; exit 1; }

ABI=arm64-v8a
INSTALL=0
SERIAL=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --abi) ABI="${2:?--abi needs a value}"; shift 2 ;;
    --install)
      INSTALL=1; shift
      if [[ $# -gt 0 && "$1" != --* ]]; then SERIAL="$1"; shift; fi ;;
    -h|--help) sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done
case "$ABI" in arm64-v8a|x86_64) ;; *) die "unsupported ABI: $ABI" ;; esac

# Use the first SDK that holds the pinned NDK. Gradle refuses to run when
# ANDROID_HOME and ANDROID_SDK_ROOT name different SDKs, so drop the other.
SDK=""
for dir in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$HOME/Android/Sdk"; do
  if [[ -n "$dir" && -d "$dir/ndk/29.0.14206865" ]]; then SDK="$dir"; break; fi
done
[[ -n "$SDK" ]] || die "no Android SDK with NDK 29.0.14206865 (tried ANDROID_HOME, ANDROID_SDK_ROOT, ~/Android/Sdk)"
export ANDROID_HOME="$SDK"
unset ANDROID_SDK_ROOT

# Gradle 9.0 needs JDK 17-24; take JDK 17 when the default java is outside that.
java_major() { "$1" -version 2>&1 | sed -n 's/.*version "\([0-9]*\).*/\1/p' | head -1; }
if [[ -z "${JAVA_HOME:-}" ]] || ! (( $(java_major "$JAVA_HOME/bin/java") >= 17 && $(java_major "$JAVA_HOME/bin/java") <= 24 )); then
  JAVA_HOME=""
  for jdk in /usr/lib/jvm/java-17-openjdk /usr/lib/jvm/java-17-openjdk-amd64 /usr/lib/jvm/temurin-17*; do
    if [[ -x "$jdk/bin/java" ]]; then JAVA_HOME="$jdk"; break; fi
  done
  [[ -n "$JAVA_HOME" ]] || die "JDK 17 not found; set JAVA_HOME"
fi
export JAVA_HOME

# SDL: an explicit checkout, either cache location, or a fresh pinned clone.
if [[ -z "${SDL_SOURCE_DIR:-}" ]]; then
  for dir in "$ROOT/.cache/android/SDL" "$ROOT/.cache/SDL-src"; do
    if [[ -f "$dir/CMakeLists.txt" ]]; then SDL_SOURCE_DIR="$dir"; break; fi
  done
fi
if [[ -z "${SDL_SOURCE_DIR:-}" ]]; then
  SDL_SOURCE_DIR="$ROOT/.cache/android/SDL"
  git clone --quiet --branch "$SDL_TAG" --depth 1 https://github.com/libsdl-org/SDL.git "$SDL_SOURCE_DIR"
fi
if [[ "$(git -C "$SDL_SOURCE_DIR" rev-parse HEAD 2>/dev/null)" != "$SDL_COMMIT" ]]; then
  echo "build-android-debug: warning: $SDL_SOURCE_DIR is not SDL $SDL_TAG ($SDL_COMMIT)" >&2
fi
export SDL_SOURCE_DIR
export JOBS="${JOBS:-$(nproc)}"

# Pick the install target before a long build, not after it.
ADB="$ANDROID_HOME/platform-tools/adb"
if (( INSTALL )) && [[ -z "$SERIAL" ]]; then
  mapfile -t devices < <("$ADB" devices | awk 'NR > 1 && $2 == "device" { print $1 }')
  (( ${#devices[@]} == 1 )) || die "--install needs a serial when ${#devices[@]} devices are attached: ${devices[*]:-none}"
  SERIAL="${devices[0]}"
fi

ANDROID_ABI="$ABI" bash "$ROOT/scripts/build-android.sh"
(cd "$ROOT/android" && ./gradlew --quiet :app:assembleDebug -PretroAbi="$ABI")

APK="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
python3 -I "$ROOT/scripts/verify-android-apk.py" "$APK" "$ABI"
echo "Debug APK: $APK"

if (( INSTALL )); then
  "$ADB" -s "$SERIAL" install -r "$APK"
  "$ADB" -s "$SERIAL" shell am start -n "$APP_ID/org.retroportingtoolkit.launcher.LauncherActivity"
fi
