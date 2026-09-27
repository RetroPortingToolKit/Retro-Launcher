#!/usr/bin/env bash
# Build Retro Launcher.app from a CMake install prefix.
#
# Usage:
#   packaging/macos/build-app.sh <install-prefix> <version> <arch>
# Example:
#   packaging/macos/build-app.sh "$PWD/out" 0.1.0 arm64
set -euo pipefail

PREFIX="${1:?install prefix}"
VERSION="${2:?version}"
ARCH="${3:?arch (arm64|x86_64)}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# Info.plist, the icon and the DMG, shared with packaging/title/.
# shellcheck source=../common/macos.sh
source "${ROOT}/packaging/common/macos.sh"
OUT_DIR="${ROOT}/dist"
APP_NAME="Retro Launcher.app"
APP="${OUT_DIR}/${APP_NAME}"

rm -rf "${APP}"
mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources" "${OUT_DIR}"

install -m 755 "${PREFIX}/bin/retcomm" "${APP}/Contents/MacOS/retcomm"
install -m 755 "${PREFIX}/bin/retro-hub" "${APP}/Contents/MacOS/retro-hub"
# The core runner, beside the hub that starts it (the published one, prebaked
# by scripts/fetch_runtime.py), and its licenses.
install -m 755 "${PREFIX}/bin/retro-core-runner" "${APP}/Contents/MacOS/retro-core-runner"
if [[ -d "${PREFIX}/share/licenses/retro-runtime" ]]; then
  mkdir -p "${APP}/Contents/Resources/licenses/retro-runtime"
  cp -a "${PREFIX}/share/licenses/retro-runtime/." "${APP}/Contents/Resources/licenses/retro-runtime/"
fi
# The hub's old name: older self-updaters relaunch Contents/MacOS/retcomm-hub.
ln -sf retro-hub "${APP}/Contents/MacOS/retcomm-hub"
# Title catalog is fetched on-device (~/.local/share/retcomm/catalog, or
# <root>/data/catalog when RETCOMM_HOME / a root marker is set); not bundled.
# Hub fonts (Lato); also under share/retcomm/fonts when installed via CMake prefix.
if [[ -d "${PREFIX}/share/retcomm/fonts" ]]; then
  mkdir -p "${APP}/Contents/Resources/fonts"
  cp -a "${PREFIX}/share/retcomm/fonts/." "${APP}/Contents/Resources/fonts/"
elif [[ -f "${ROOT}/assets/fonts/LatoLatin-Regular.ttf" ]]; then
  mkdir -p "${APP}/Contents/Resources/fonts"
  cp -a "${ROOT}/assets/fonts/." "${APP}/Contents/Resources/fonts/"
fi
if [[ ! -f "${APP}/Contents/Resources/fonts/LatoLatin-Regular.ttf" ]]; then
  echo "error: hub fonts missing in app bundle (LatoLatin-Regular.ttf)" >&2
  exit 1
fi

# Hub platform controller icons (library cards).
if [[ -d "${PREFIX}/share/retcomm/platforms" ]]; then
  mkdir -p "${APP}/Contents/Resources/platforms"
  cp -a "${PREFIX}/share/retcomm/platforms/." "${APP}/Contents/Resources/platforms/"
elif [[ -f "${ROOT}/assets/platforms/psx.png" ]]; then
  mkdir -p "${APP}/Contents/Resources/platforms"
  cp -a "${ROOT}/assets/platforms/." "${APP}/Contents/Resources/platforms/"
fi
if [[ ! -f "${APP}/Contents/Resources/platforms/psx.png" ]]; then
  echo "error: hub platform icons missing in app bundle (psx.png)" >&2
  exit 1
fi

# PSX DualShock art for the Gamepads configure mapper.
if [[ -d "${PREFIX}/share/retcomm/controllers" ]]; then
  mkdir -p "${APP}/Contents/Resources/controllers"
  cp -a "${PREFIX}/share/retcomm/controllers/." "${APP}/Contents/Resources/controllers/"
elif [[ -f "${ROOT}/assets/controllers/pad_analog.png" ]]; then
  mkdir -p "${APP}/Contents/Resources/controllers"
  cp -a "${ROOT}/assets/controllers/." "${APP}/Contents/Resources/controllers/"
fi
if [[ ! -f "${APP}/Contents/Resources/controllers/pad_analog.png" ]]; then
  echo "error: hub PSX pad art missing in app bundle (pad_analog.png)" >&2
  exit 1
fi

# First-run setup path cards (Easy / Advanced).
if [[ -d "${PREFIX}/share/retcomm/setup" ]]; then
  mkdir -p "${APP}/Contents/Resources/setup"
  cp -a "${PREFIX}/share/retcomm/setup/." "${APP}/Contents/Resources/setup/"
elif [[ -f "${ROOT}/assets/setup/setup_easy_rocket.png" ]]; then
  mkdir -p "${APP}/Contents/Resources/setup"
  cp -a "${ROOT}/assets/setup/." "${APP}/Contents/Resources/setup/"
fi
for n in setup_easy_rocket.png setup_advanced_wrench.png; do
  if [[ ! -f "${APP}/Contents/Resources/setup/${n}" ]]; then
    echo "error: hub setup card art missing in app bundle (${n})" >&2
    exit 1
  fi
done

rh_render_plist "${ROOT}/packaging/common/Info.plist.in" "${APP}/Contents/Info.plist" \
  "Retro Launcher" "com.technicallycomputers.retcomm-launcher" "${VERSION}"

# Icon: prefer prebuilt .icns; else build from PNG via iconutil.
if [[ -f "${ROOT}/assets/retcomm.icns" ]]; then
  install -m 644 "${ROOT}/assets/retcomm.icns" "${APP}/Contents/Resources/AppIcon.icns"
elif [[ -f "${ROOT}/assets/retcomm.png" ]]; then
  # Standard iconset sizes from the 512 master PNG.
  rh_make_icns "${ROOT}/assets/retcomm.png" "${APP}/Contents/Resources/AppIcon.icns" "${OUT_DIR}"
fi

# Recursively bundle Homebrew/SDL/FreeType dylibs and rewrite nested install
# names (freetype → libpng, curl → openssl, …). Fails if any cellar path remains.
"${ROOT}/packaging/common/bundle_dylibs.sh" "${APP}"

# Drag-to-Applications DMG (.app + /Applications symlink).
# Stable filename (no version): version is in Info.plist / release tag only.
DMG="${OUT_DIR}/Retro-Launcher-macos-${ARCH}.dmg"
rh_create_dmg "${APP}" "Retro Launcher" "${DMG}" "${OUT_DIR}"

echo "App: ${APP}"
echo "DMG: ${DMG}"
