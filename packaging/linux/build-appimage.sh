#!/usr/bin/env bash
# Assemble a Retro AppImage from a CMake install prefix.
#
# Usage:
#   packaging/linux/build-appimage.sh <install-prefix> <version> [arch]
# Example:
#   packaging/linux/build-appimage.sh "$PWD/out" 0.1.0 x86_64
#
# <version> is embedded (AppRun / linuxdeploy metadata). Output filename is
# stable: dist/Retro-Launcher-linux-<arch>.AppImage
set -euo pipefail

PREFIX="${1:?install prefix}"
VERSION="${2:?version}"
ARCH="${3:-$(uname -m)}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# linuxdeploy fetch/run and AppImage extraction, shared with packaging/title/.
# shellcheck source=../common/appimage.sh
source "${ROOT}/packaging/common/appimage.sh"
OUT_DIR="${ROOT}/dist"
APPDIR="${OUT_DIR}/Retro.AppDir"
TOOL_DIR="${OUT_DIR}/tools"

rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr" "${TOOL_DIR}" "${OUT_DIR}"

# Copy staged install (bin + optional lib/share icons). Catalog is on-device only.
cp -a "${PREFIX}/." "${APPDIR}/usr/"

# Hub fonts (Lato) — required for AppImage UI. Prefer CMake install; fall back to assets/.
FONTS_DST="${APPDIR}/usr/share/retcomm/fonts"
if [[ ! -f "${FONTS_DST}/LatoLatin-Regular.ttf" ]]; then
  if [[ -f "${ROOT}/assets/fonts/LatoLatin-Regular.ttf" ]]; then
    mkdir -p "${FONTS_DST}"
    cp -a "${ROOT}/assets/fonts/." "${FONTS_DST}/"
  fi
fi
if [[ ! -f "${FONTS_DST}/LatoLatin-Regular.ttf" ]]; then
  echo "error: hub fonts missing (expected ${FONTS_DST}/LatoLatin-Regular.ttf or assets/fonts/)" >&2
  echo "  cmake --install should place share/retcomm/fonts from assets/fonts/" >&2
  exit 1
fi
# Also next to the binary: SDL_GetBasePath() is often …/usr/bin/ inside the AppImage.
mkdir -p "${APPDIR}/usr/bin/fonts"
cp -a "${FONTS_DST}/." "${APPDIR}/usr/bin/fonts/"

# Platform controller icons (library cards). Prefer CMake install; fall back to assets/.
PLAT_DST="${APPDIR}/usr/share/retcomm/platforms"
if [[ ! -f "${PLAT_DST}/psx.png" ]]; then
  if [[ -f "${ROOT}/assets/platforms/psx.png" ]]; then
    mkdir -p "${PLAT_DST}"
    cp -a "${ROOT}/assets/platforms/." "${PLAT_DST}/"
  fi
fi
if [[ ! -f "${PLAT_DST}/psx.png" ]]; then
  echo "error: hub platform icons missing (expected ${PLAT_DST}/psx.png or assets/platforms/)" >&2
  exit 1
fi
mkdir -p "${APPDIR}/usr/bin/platforms"
cp -a "${PLAT_DST}/." "${APPDIR}/usr/bin/platforms/"

# PSX DualShock art for the Gamepads configure mapper.
CTRL_DST="${APPDIR}/usr/share/retcomm/controllers"
if [[ ! -f "${CTRL_DST}/pad_analog.png" ]]; then
  if [[ -f "${ROOT}/assets/controllers/pad_analog.png" ]]; then
    mkdir -p "${CTRL_DST}"
    cp -a "${ROOT}/assets/controllers/." "${CTRL_DST}/"
  fi
fi
if [[ ! -f "${CTRL_DST}/pad_analog.png" ]]; then
  echo "error: hub PSX pad art missing (expected ${CTRL_DST}/pad_analog.png or assets/controllers/)" >&2
  exit 1
fi
mkdir -p "${APPDIR}/usr/bin/controllers"
cp -a "${CTRL_DST}/." "${APPDIR}/usr/bin/controllers/"

# First-run setup path cards (Easy / Advanced).
SETUP_DST="${APPDIR}/usr/share/retcomm/setup"
if [[ ! -f "${SETUP_DST}/setup_easy_rocket.png" ]]; then
  if [[ -f "${ROOT}/assets/setup/setup_easy_rocket.png" ]]; then
    mkdir -p "${SETUP_DST}"
    cp -a "${ROOT}/assets/setup/." "${SETUP_DST}/"
  fi
fi
for n in setup_easy_rocket.png setup_advanced_wrench.png; do
  if [[ ! -f "${SETUP_DST}/${n}" ]]; then
    echo "error: hub setup card art missing (expected ${SETUP_DST}/${n} or assets/setup/)" >&2
    exit 1
  fi
done
mkdir -p "${APPDIR}/usr/bin/setup"
cp -a "${SETUP_DST}/." "${APPDIR}/usr/bin/setup/"

# Desktop + icon at AppDir root (linuxdeploy / appimagetool convention).
install -m 644 "${ROOT}/packaging/linux/retcomm.desktop" "${APPDIR}/retcomm.desktop"
if [[ -f "${ROOT}/assets/retcomm.png" ]]; then
  # Must be a linuxdeploy-allowed size (max 512x512). make-icons.sh writes 512.
  install -m 644 "${ROOT}/assets/retcomm.png" "${APPDIR}/retcomm.png"
  mkdir -p "${APPDIR}/usr/share/icons/hicolor/512x512/apps"
  install -m 644 "${ROOT}/assets/retcomm.png" \
    "${APPDIR}/usr/share/icons/hicolor/512x512/apps/retcomm.png"
fi
if [[ -f "${ROOT}/assets/retcomm.svg" ]]; then
  mkdir -p "${APPDIR}/usr/share/icons/hicolor/scalable/apps"
  install -m 644 "${ROOT}/assets/retcomm.svg" \
    "${APPDIR}/usr/share/icons/hicolor/scalable/apps/retcomm.svg"
fi

sed "s|@VERSION@|${VERSION}|g" "${ROOT}/packaging/common/AppRun.in" > "${APPDIR}/AppRun"
chmod 755 "${APPDIR}/AppRun"

# Bundle runtime deps (SDL3, libcurl, …) with linuxdeploy (cached in
# dist/tools/, or $RETRO_HUB_LINUXDEPLOY).
LINUXDEPLOY="$(rh_linuxdeploy "${ARCH}" "${TOOL_DIR}")"

# Plugin optional — linuxdeploy still copies DT_NEEDED libs without it.
# Stable filename (no version): self-update replaces the AppImage in place and
# keeps the user's path, so a versioned name becomes stale after the first update.
export LDAI_OUTPUT="${OUT_DIR}/Retro-Launcher-linux-${ARCH}.AppImage"
export LINUXDEPLOY_OUTPUT_VERSION="${VERSION}"

# rh_run_linuxdeploy: --appimage-extract-and-run when FUSE is missing (CI), and
# NO_STRIP=1 (linuxdeploy's old strip rejects modern RELR ELF).
rh_run_linuxdeploy "${LINUXDEPLOY}" \
  --appdir "${APPDIR}" \
  --executable "${APPDIR}/usr/bin/retro-hub" \
  --executable "${APPDIR}/usr/bin/retro-core-runner" \
  --executable "${APPDIR}/usr/bin/retcomm" \
  --desktop-file "${APPDIR}/retcomm.desktop" \
  --icon-file "${APPDIR}/retcomm.png" \
  --output appimage

# Normalize output name if linuxdeploy used a different default.
APPIMAGE_OUT="${OUT_DIR}/Retro-Launcher-linux-${ARCH}.AppImage"
shopt -s nullglob
for f in "${OUT_DIR}"/*.AppImage; do
  base="$(basename "$f")"
  if [[ "${base}" != "Retro-Launcher-linux-${ARCH}.AppImage" &&
        "${base}" != linuxdeploy* ]]; then
    mv -f "$f" "${APPIMAGE_OUT}"
  fi
done

if [[ ! -f "${APPIMAGE_OUT}" ]]; then
  echo "error: AppImage not produced at ${APPIMAGE_OUT}" >&2
  exit 1
fi

# Verify fonts survived packaging. Prefer --appimage-extract; if that fails
# (no FUSE / display auth), unsquash at the type-2 squashfs offset. Hard-fail
# when fonts are missing or the image cannot be inspected.
VERIFY_DIR="${OUT_DIR}/.appimage-font-check"
rh_extract_appimage "${APPIMAGE_OUT}" "${VERIFY_DIR}" || {
  echo "error: could not extract AppImage to verify hub fonts" >&2
  echo "  need working --appimage-extract or unsquashfs" >&2
  exit 1
}
(
  cd "${VERIFY_DIR}"
  ROOT_DIR=squashfs-root
  if [[ ! -f "${ROOT_DIR}/usr/share/retcomm/fonts/LatoLatin-Regular.ttf" &&
        ! -f "${ROOT_DIR}/usr/bin/fonts/LatoLatin-Regular.ttf" ]]; then
    echo "error: AppImage is missing hub fonts (LatoLatin-Regular.ttf)" >&2
    find "${ROOT_DIR}/usr" -name '*.ttf' 2>/dev/null || true
    exit 1
  fi
  # AppRun must export APPDIR so hub font lookup works when the runtime does not.
  if ! grep -q 'export APPDIR=' "${ROOT_DIR}/AppRun"; then
    echo "error: AppRun does not export APPDIR (hub fonts may fail at runtime)" >&2
    exit 1
  fi
  echo "fonts ok in AppImage"
)
rm -rf "${VERIFY_DIR}"

echo "AppImage: ${APPIMAGE_OUT}"
