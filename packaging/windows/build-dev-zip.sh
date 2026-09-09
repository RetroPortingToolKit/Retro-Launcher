#!/usr/bin/env bash
# Build a Windows x64 Retro dev test build on Linux (mingw-w64 cross) and
# bundle it as a zip you can drop on a Windows box.
#
# Usage:
#   ./packaging/windows/build-dev-zip.sh
#   ./packaging/windows/build-dev-zip.sh 0.6.4          # version label
#   JOBS=8 ./packaging/windows/build-dev-zip.sh
#   SKIP_SDL_BUILD=1 ./packaging/windows/build-dev-zip.sh   # require a prebuilt SDL3
#   PORTABLE=1 ./packaging/windows/build-dev-zip.sh         # also emit the single-exe stub
#   STRIP=0 ./packaging/windows/build-dev-zip.sh            # keep symbols in the zip
#
# Output:
#   dist/Retro-Launcher-windows-x64-dev.zip
#     └── Retro-Launcher-windows-x64-dev/
#           retcomm-hub.exe, retcomm.exe, *.dll, fonts/, platforms/,
#           controllers/, setup/, DEV-BUILD.txt
#
# On Windows: unzip anywhere, run retcomm-hub.exe.
#
# This is NOT the release path. Releases are MSVC + vcpkg + Inno Setup via
# .github/workflows/release.yml → packaging/windows/package.ps1. A cross build
# ships no channel.json, so Menu → Update Retro stays disabled (by design:
# a dev build must not overwrite itself with a release asset), and mingw has no
# FreeType here, so ImGui color emoji are off.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "${ROOT}"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"
  exit 0
fi

TRIPLE="${TRIPLE:-x86_64-w64-mingw32}"
SYSROOT="${SYSROOT:-/usr/${TRIPLE}}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
SDL_TAG="${SDL_TAG:-release-3.2.16}"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build-windows}"
PREFIX="${PREFIX:-${ROOT}/out-windows}"
SDL_PREFIX="${SDL_PREFIX:-${ROOT}/.cache/sdl3-mingw}"
SKIP_SDL_BUILD="${SKIP_SDL_BUILD:-0}"
PORTABLE="${PORTABLE:-0}"
STRIP="${STRIP:-1}"
DIST="${ROOT}/dist"
NAME="Retro-Launcher-windows-x64-dev"
STAGE="${DIST}/${NAME}"

if [[ -n "${1:-}" ]]; then
  VERSION="${1#v}"
else
  BASE="$(sed -n 's/.*set(RETCOMM_VERSION "\([^"]*\)".*/\1/p' CMakeLists.txt | head -1)"
  SHA="$(git -C "${ROOT}" rev-parse --short HEAD 2>/dev/null || echo nogit)"
  VERSION="${BASE:-0.0.0}-dev.${SHA}"
fi

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "Missing required command: $1" >&2
    exit 1
  }
}

need_cmd cmake
need_cmd ninja
need_cmd git
need_cmd zip
need_cmd "${TRIPLE}-g++"
need_cmd "${TRIPLE}-windres"
need_cmd "${TRIPLE}-objdump"

if [[ ! -d "${SYSROOT}" ]]; then
  echo "mingw-w64 sysroot not found: ${SYSROOT}" >&2
  echo "Install the cross toolchain (Arch: mingw-w64-gcc mingw-w64-curl mingw-w64-zlib)." >&2
  exit 1
fi

echo "==> Retro Windows dev build ${VERSION}"
echo "    triple: ${TRIPLE}"
echo "    build:  ${BUILD_DIR}"
echo "    prefix: ${PREFIX}"

# --- SDL3 (no mingw-w64 SDL3 package on most distros; build and cache it) ----
sdl3_config() {
  local p
  for p in "${SDL_PREFIX}/lib/cmake/SDL3/SDL3Config.cmake" \
    "${SDL_PREFIX}/lib64/cmake/SDL3/SDL3Config.cmake" \
    "${SYSROOT}/lib/cmake/SDL3/SDL3Config.cmake"; do
    [[ -f "${p}" ]] && return 0
  done
  return 1
}

if sdl3_config; then
  echo "==> Using existing SDL3 (${SDL_PREFIX} or sysroot)"
elif [[ "${SKIP_SDL_BUILD}" == "1" ]]; then
  echo "SDL3 for ${TRIPLE} not found and SKIP_SDL_BUILD=1" >&2
  exit 1
else
  echo "==> Cross-building SDL3 ${SDL_TAG} → ${SDL_PREFIX}"
  SDL_SRC="${ROOT}/.cache/SDL-src"
  if [[ ! -d "${SDL_SRC}/.git" ]]; then
    rm -rf "${SDL_SRC}"
    git clone --depth 1 --branch "${SDL_TAG}" https://github.com/libsdl-org/SDL.git "${SDL_SRC}"
  else
    git -C "${SDL_SRC}" fetch --depth 1 origin "refs/tags/${SDL_TAG}:refs/tags/${SDL_TAG}" 2>/dev/null || true
    git -C "${SDL_SRC}" checkout -q "${SDL_TAG}"
  fi
  cmake -G Ninja -S "${SDL_SRC}" -B "${ROOT}/.cache/sdl-build-mingw" \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/toolchain-mingw-w64-x86_64.cmake" \
    -DMINGW_TARGET_TRIPLE="${TRIPLE}" \
    -DMINGW_SYSROOT="${SYSROOT}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${SDL_PREFIX}" \
    -DSDL_SHARED=ON \
    -DSDL_STATIC=OFF \
    -DSDL_TESTS=OFF \
    -DSDL_EXAMPLES=OFF
  cmake --build "${ROOT}/.cache/sdl-build-mingw" -j"${JOBS}"
  cmake --install "${ROOT}/.cache/sdl-build-mingw"
fi

# --- Configure & build ------------------------------------------------------
echo "==> Configure & build Retro"
rm -rf "${PREFIX}"
mkdir -p "${PREFIX}"
cmake -G Ninja -S "${ROOT}" -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/toolchain-mingw-w64-x86_64.cmake" \
  -DMINGW_TARGET_TRIPLE="${TRIPLE}" \
  -DMINGW_SYSROOT="${SYSROOT}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
  -DCMAKE_PREFIX_PATH="${SDL_PREFIX}" \
  -DRETCOMM_VERSION="${VERSION}"
cmake --build "${BUILD_DIR}" -j"${JOBS}"
cmake --install "${BUILD_DIR}"

for exe in retcomm.exe retcomm-hub.exe retcomm-portable.exe; do
  if [[ ! -f "${PREFIX}/bin/${exe}" ]]; then
    echo "${exe} missing from install prefix — build did not produce the full set." >&2
    [[ "${exe}" == "retcomm-hub.exe" ]] &&
      echo "(retcomm-hub needs SDL3 + Dear ImGui: sibling ../recomp-ui or FetchContent.)" >&2
    exit 1
  fi
done

# --- Stage ------------------------------------------------------------------
echo "==> Stage ${STAGE}"
rm -rf "${STAGE}"
mkdir -p "${STAGE}"
cp "${PREFIX}/bin/retcomm.exe" "${PREFIX}/bin/retcomm-hub.exe" "${STAGE}/"

# DLL closure: walk each exe's import table, resolve names against the SDL3
# prefix and the mingw sysroot, repeat for what those DLLs import. Anything not
# found in our search dirs is a Windows system DLL (kernel32, opengl32, …).
declare -A SEEN=()
SEARCH_DIRS=("${SDL_PREFIX}/bin" "${SDL_PREFIX}/lib" "${SYSROOT}/bin" "${SYSROOT}/lib")

find_dll() {
  local name="$1" d
  for d in "${SEARCH_DIRS[@]}"; do
    [[ -f "${d}/${name}" ]] && { printf '%s\n' "${d}/${name}"; return 0; }
  done
  return 1
}

collect_dlls() {
  local file="$1" dep path
  while read -r dep; do
    [[ -z "${dep}" ]] && continue
    local key="${dep,,}"
    [[ -n "${SEEN[${key}]:-}" ]] && continue
    if path="$(find_dll "${dep}")"; then
      SEEN[${key}]="${path}"
      cp -f "${path}" "${STAGE}/"
      collect_dlls "${path}"
    else
      SEEN[${key}]="system"
    fi
  done < <("${TRIPLE}-objdump" -p "${file}" 2>/dev/null |
    sed -n 's/^\s*DLL Name:\s*//p')
}

collect_dlls "${STAGE}/retcomm.exe"
collect_dlls "${STAGE}/retcomm-hub.exe"

if ! compgen -G "${STAGE}/SDL3.dll" >/dev/null; then
  echo "SDL3.dll not bundled — the hub cannot run without it." >&2
  exit 1
fi
if ! compgen -G "${STAGE}/libcurl*.dll" >/dev/null; then
  echo "libcurl DLL not bundled — catalog/downloads cannot work without it." >&2
  exit 1
fi

# Runtime assets, same layout as the release package (next to the exes).
for dir in fonts platforms controllers setup; do
  src="${PREFIX}/share/retcomm/${dir}"
  [[ -d "${src}" ]] || src="${ROOT}/assets/${dir}"
  if [[ ! -d "${src}" ]]; then
    echo "Missing runtime assets: ${dir}" >&2
    exit 1
  fi
  mkdir -p "${STAGE}/${dir}"
  cp -a "${src}/." "${STAGE}/${dir}/"
done
[[ -f "${ROOT}/assets/retcomm.ico" ]] && cp "${ROOT}/assets/retcomm.ico" "${STAGE}/"
[[ -f "${ROOT}/assets/retcomm.png" ]] && cp "${ROOT}/assets/retcomm.png" "${STAGE}/"

for need in fonts/LatoLatin-Regular.ttf platforms/psx.png controllers/pad_analog.png \
  setup/setup_easy_rocket.png setup/setup_advanced_wrench.png; do
  [[ -f "${STAGE}/${need}" ]] || {
    echo "Staged payload missing ${need}" >&2
    exit 1
  }
done

# Symbols are ~16 MB of the hub alone; the unstripped binaries stay in
# ${PREFIX}/bin for gdb. STRIP=0 keeps them in the zip instead.
if [[ "${STRIP}" == "1" ]]; then
  "${TRIPLE}-strip" "${STAGE}"/*.exe "${STAGE}"/*.dll
fi

# No channel.json on purpose — see the header comment.
cat >"${STAGE}/DEV-BUILD.txt" <<EOF
Retro Launcher ${VERSION}
Windows x64 dev test build — cross-compiled on Linux with ${TRIPLE}
Built $(date -u '+%Y-%m-%d %H:%M:%S UTC') from $(git -C "${ROOT}" rev-parse HEAD 2>/dev/null || echo 'unknown commit')

Run retcomm-hub.exe (GUI) or retcomm.exe (CLI) from this folder — keep the
DLLs and the fonts/, platforms/, controllers/, setup/ folders beside them.

Not a release build:
  * unsigned — SmartScreen / Smart App Control will warn
  * self-update is disabled (no channel.json)
  * built with mingw-w64, not MSVC; color emoji are off (no FreeType)
Release builds come from .github/workflows/release.yml.
EOF

# --- Zip --------------------------------------------------------------------
ZIP="${DIST}/${NAME}.zip"
rm -f "${ZIP}"
(cd "${DIST}" && zip -qr9 "${NAME}.zip" "${NAME}")

# --- Optional: single-file portable exe (stub + payload, as package.ps1) -----
if [[ "${PORTABLE}" == "1" ]]; then
  echo "==> Portable single-exe stub"
  PAYLOAD="${DIST}/windows-portable-payload.zip"
  rm -f "${PAYLOAD}"
  # Stub extracts the payload root next to itself, so the zip must be flat.
  (cd "${STAGE}" && zip -qr9 "${PAYLOAD}" .)
  PORT_EXE="${DIST}/Retro Launcher.exe"
  cp "${PREFIX}/bin/retcomm-portable.exe" "${PORT_EXE}"
  [[ "${STRIP}" == "1" ]] && "${TRIPLE}-strip" "${PORT_EXE}"
  cat "${PAYLOAD}" >>"${PORT_EXE}"
  # Trailer: uint64 LE payload size + "RCM1" (src/portable/portable_trailer.hpp).
  python3 -c 'import struct,sys; open(sys.argv[1],"ab").write(struct.pack("<Q", int(sys.argv[2])) + b"RCM1")' \
    "${PORT_EXE}" "$(stat -c %s "${PAYLOAD}")"
  rm -f "${PAYLOAD}"
  echo "    ${PORT_EXE}"
fi

echo
echo "Done: ${ZIP}"
du -h "${ZIP}" | awk '{print "      " $1}'
echo "Copy to Windows, unzip, run ${NAME}\\retcomm-hub.exe"
