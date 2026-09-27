#!/usr/bin/env bash
# Build retro-hub for THIS machine into a FLAT hub prefix, for local use: what
# a port's framework points RETRO_HUB at to run or package a title app
# (docs/RELEASES.md, "Title-app mode"). Linux and macOS; scripts/build-local.ps1
# is Windows.
#
# Not a release: the release is .github/workflows/release.yml and its gates.
# The prefix is laid out like the bare hub archive (scripts/package_hub_archive.sh,
# the same file list: scripts/flat_hub_layout.sh) plus the runner:
#
#   <out>/retro-hub                 title_app 1, direct_mode 4
#   <out>/retro-core-runner         from the Retro-Runtime submodule, or --runtime
#   <out>/libSDL3.so.0              Linux: the SDL3 the hub was linked against
#   <out>/retcomm.png, fonts/, platforms/, controllers/, setup/, LICENSE
#   <out>/packaging/title/          the title-app kit (build-title-app.sh)
#   <out>/packaging/common/         what the kit shares with the launcher's packaging
#
# SDL3: found (CMAKE_PREFIX_PATH, pkg-config), else built from source into
# <repo>/.cache/sdl3 (scripts/sdl3_local.sh); on macOS from Homebrew.
#
# The prefix is checked before its path is printed: retro-hub --version must
# say `title_app 1` and list --package, the runner must answer --version, and
# the kit must run. The LAST line on stdout is always
#   RETRO_HUB=<absolute path to retro-hub>
#
# usage: scripts/build-local.sh [--debug] [--out DIR] [--build DIR]
#          [--runtime <Retro-Runtime checkout>] [--jobs N] [--help]
set -euo pipefail

die() { echo "build-local: $*" >&2; exit 1; }
say() { echo "build-local: $*" >&2; }

usage() {
  cat <<'EOF'
usage: scripts/build-local.sh [options]

Build retro-hub (and a retro-core-runner) into a flat hub prefix for this
machine, and print its path last, as RETRO_HUB=<absolute path>.

  --debug          Debug build (default: Release)
  --out DIR        the flat prefix (default: <repo>/out/local/<platform>/)
  --build DIR      CMake build directory (default: <repo>/build-local[-debug])
  --runtime DIR    build the runner from this Retro-Runtime checkout (its
                   scripts/build-local.sh when it has one) instead of the
                   submodule's
  --jobs N         parallel build jobs (default: all CPUs)
  --help           this text

<platform> is linux-x86_64, linux-arm64, macos-x86_64 or macos-arm64.
Relative paths are taken against the current directory.
EOF
}

CONFIG=Release OUT="" BUILD="" RUNTIME="" JOBS=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug)   CONFIG=Debug; shift ;;
    --out)     [[ $# -ge 2 ]] || die "--out needs a directory"; OUT="$2"; shift 2 ;;
    --build)   [[ $# -ge 2 ]] || die "--build needs a directory"; BUILD="$2"; shift 2 ;;
    --runtime) [[ $# -ge 2 ]] || die "--runtime needs a directory"; RUNTIME="$2"; shift 2 ;;
    --jobs)    [[ $# -ge 2 ]] || die "--jobs needs a number"; JOBS="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument '$1' (see --help)" ;;
  esac
done
[[ -z "${JOBS}" || "${JOBS}" =~ ^[1-9][0-9]*$ ]] || die "--jobs: want a positive number, got '${JOBS}'"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$(uname -s)" in
  Linux)  OS=linux ;;
  Darwin) OS=macos ;;
  *) die "$(uname -s): this script builds on Linux and macOS; use scripts/build-local.ps1 on Windows" ;;
esac
case "$(uname -m)" in
  x86_64|amd64)  ARCH=x86_64 ;;
  aarch64|arm64) ARCH=arm64 ;;
  *) die "$(uname -m): no platform name for this architecture" ;;
esac
PLATFORM="${OS}-${ARCH}"

abspath() { mkdir -p "$1" && (cd "$1" && pwd); }
if [[ -z "${BUILD}" ]]; then
  BUILD="${ROOT}/build-local"
  [[ "${CONFIG}" == Debug ]] && BUILD+="-debug"
fi
BUILD="$(abspath "${BUILD}")"
OUT="$(abspath "${OUT:-${ROOT}/out/local/${PLATFORM}}")"
[[ "${OUT}" != "${BUILD}" ]] || die "--out and --build must differ"
if [[ -n "${RUNTIME}" ]]; then
  [[ -f "${RUNTIME}/CMakeLists.txt" && -d "${RUNTIME}/runner" ]] ||
    die "--runtime ${RUNTIME}: not a Retro-Runtime checkout"
  RUNTIME="$(cd "${RUNTIME}" && pwd)"
fi
if [[ -z "${JOBS}" ]]; then
  JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"
fi
command -v cmake >/dev/null || die "cmake not found"
[[ -f "${ROOT}/third_party/Retro-Runtime/CMakeLists.txt" ]] ||
  die "third_party/Retro-Runtime is empty: run 'git submodule update --init' in ${ROOT}"

# A prefix this script made carries a marker; anything else is not ours to empty.
MARK="${OUT}/.retro-hub-local"
if [[ -n "$(ls -A "${OUT}")" && ! -f "${MARK}" ]]; then
  die "--out ${OUT} is not empty and was not made by build-local.sh; pick another directory"
fi

# ---- SDL3 ---------------------------------------------------------------------
# shellcheck source=sdl3_local.sh
source "${ROOT}/scripts/sdl3_local.sh"
if [[ "${OS}" == macos ]]; then
  if ! sdl3_find "${ROOT}/.cache/sdl3"; then
    command -v brew >/dev/null || die "SDL3 not found, and no Homebrew to install it (brew install sdl3)"
    brew list --formula sdl3 >/dev/null 2>&1 || { say "brew install sdl3"; brew install sdl3 >&2; }
    brew_sdl3="$(brew --prefix sdl3)"
    export CMAKE_PREFIX_PATH="${brew_sdl3}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
  fi
elif ! sdl3_find "${ROOT}/.cache/sdl3"; then
  sdl3_build "${ROOT}/.cache/sdl3" "${JOBS}"
fi

# ---- the commit, with -dirty when tracked files changed -----------------------
COMMIT=""
if git -C "${ROOT}" rev-parse --git-dir >/dev/null 2>&1; then
  COMMIT="$(git -C "${ROOT}" rev-parse HEAD)"
  git -C "${ROOT}" diff --quiet HEAD -- 2>/dev/null || COMMIT+="-dirty"
fi

# ---- configure and build --------------------------------------------------------
cmake_args=(
  -S "${ROOT}" -B "${BUILD}"
  "-DCMAKE_BUILD_TYPE=${CONFIG}"
  "-DRETCOMM_COMMIT=${COMMIT}"
)
command -v ninja >/dev/null && cmake_args+=(-G Ninja)
[[ -z "${CMAKE_PREFIX_PATH:-}" ]] || cmake_args+=("-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
# The runner finds SDL3 beside itself in the flat prefix (the hub already
# carries $ORIGIN in its own INSTALL_RPATH).
case "${OS}" in
  linux) cmake_args+=("-DCMAKE_INSTALL_RPATH=\$ORIGIN") ;;
  macos) cmake_args+=("-DCMAKE_INSTALL_RPATH=@loader_path") ;;
esac
# Another generator or other choices in an existing cache: start it afresh.
STAMP="${BUILD}/build-local.args"
if [[ -f "${STAMP}" && "$(cat "${STAMP}")" != "$(printf '%s\n' "${cmake_args[@]}")" ]]; then
  say "configuration changed since the last build in ${BUILD}; reconfiguring from scratch"
  rm -rf "${BUILD}/CMakeCache.txt" "${BUILD}/CMakeFiles"
fi
say "${PLATFORM}, ${CONFIG}, build ${BUILD}"
cmake "${cmake_args[@]}" >&2
printf '%s\n' "${cmake_args[@]}" > "${STAMP}"
cmake --build "${BUILD}" --parallel "${JOBS}" >&2

STAGE="${BUILD}/build-local-stage"
rm -rf "${STAGE}"
cmake --install "${BUILD}" --prefix "${STAGE}" >/dev/null
[[ -x "${STAGE}/bin/retro-hub" ]] ||
  die "retro-hub was not built (it needs SDL3 and Dear ImGui; see the configure output)"

# ---- the runner: the submodule's, or --runtime's ----------------------------
RUNNER_SRC="${STAGE}/bin/retro-core-runner"
RUNNER_FROM="the Retro-Runtime submodule ($(git -C "${ROOT}/third_party/Retro-Runtime" rev-parse --short HEAD 2>/dev/null || echo '?'))"
RUNNER_LIBS=()
if [[ -n "${RUNTIME}" ]]; then
  RT_OUT="${BUILD}/runtime-out" RT_BUILD="${BUILD}/runtime-build"
  if [[ -x "${RUNTIME}/scripts/build-local.sh" ]]; then
    say "building the runner with ${RUNTIME}/scripts/build-local.sh"
    rt_args=(--out "${RT_OUT}" --build "${RT_BUILD}" --jobs "${JOBS}")
    [[ "${CONFIG}" == Debug ]] && rt_args+=(--debug)
    rt_report="$("${RUNTIME}/scripts/build-local.sh" "${rt_args[@]}")" || die "${RUNTIME}/scripts/build-local.sh failed"
    RUNNER_SRC="$(tail -1 <<<"${rt_report}" | sed -n 's/^RETRO_CORE_RUNNER=//p')"
    [[ -x "${RUNNER_SRC}" ]] || die "${RUNTIME}/scripts/build-local.sh printed no RETRO_CORE_RUNNER= line"
    shopt -s nullglob
    RUNNER_LIBS=("$(dirname "${RUNNER_SRC}")"/libSDL3*)
    shopt -u nullglob
  else
    say "building the runner from ${RUNTIME} with cmake (it has no scripts/build-local.sh)"
    rt_cmake=(-S "${RUNTIME}" -B "${RT_BUILD}" "-DCMAKE_BUILD_TYPE=${CONFIG}")
    command -v ninja >/dev/null && rt_cmake+=(-G Ninja)
    [[ -z "${CMAKE_PREFIX_PATH:-}" ]] || rt_cmake+=("-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
    case "${OS}" in
      linux) rt_cmake+=("-DCMAKE_INSTALL_RPATH=\$ORIGIN") ;;
      macos) rt_cmake+=("-DCMAKE_INSTALL_RPATH=@loader_path") ;;
    esac
    cmake "${rt_cmake[@]}" >&2
    cmake --build "${RT_BUILD}" --parallel "${JOBS}" --target retro-core-runner >&2
    rm -rf "${RT_OUT}"
    cmake --install "${RT_BUILD}" --prefix "${RT_OUT}" >/dev/null
    RUNNER_SRC="${RT_OUT}/bin/retro-core-runner"
  fi
  RUNNER_FROM="${RUNTIME} ($(git -C "${RUNTIME}" rev-parse --short HEAD 2>/dev/null || echo '?'))"
fi
[[ -x "${RUNNER_SRC}" ]] || die "no retro-core-runner at ${RUNNER_SRC}"

# ---- the flat prefix ------------------------------------------------------------
find "${OUT}" -mindepth 1 -delete
touch "${MARK}"
# shellcheck source=flat_hub_layout.sh
source "${ROOT}/scripts/flat_hub_layout.sh"
entries="$(flat_hub_entries "${STAGE}")" || die "the install stage ${STAGE} is incomplete"
while IFS=$'\t' read -r src dst mode; do
  [[ -f "${src}" ]] || die "${src}: missing (wanted as ${dst})"
  mkdir -p "$(dirname "${OUT}/${dst}")"
  cp -L "${src}" "${OUT}/${dst}"
  chmod "${mode}" "${OUT}/${dst}"
done <<<"${entries}"
cp "${ROOT}/LICENSE" "${OUT}/LICENSE"
cp -L "${RUNNER_SRC}" "${OUT}/retro-core-runner"
chmod 0755 "${OUT}/retro-core-runner"
for lib in ${RUNNER_LIBS[@]+"${RUNNER_LIBS[@]}"}; do
  [[ -e "${OUT}/$(basename "${lib}")" ]] || cp -L "${lib}" "${OUT}/$(basename "${lib}")"
done

# SDL3 beside the hub (Linux): the one it was linked against, resolved by the
# build-tree binary. On macOS the hub keeps Homebrew's install name; the
# title-app kit's bundle_dylibs.sh copies it into an app.
SDL_NOTE="SDL3 from the system install name"
if [[ "${OS}" == linux ]]; then
  sdl="$(ldd "${BUILD}/retro-hub" | awk '$1 ~ /^libSDL3\.so/ {print $1 " " $3; exit}')"
  [[ -n "${sdl}" ]] || die "retro-hub does not import libSDL3"
  soname="${sdl%% *}" path="${sdl#* }"
  [[ -f "${path}" ]] || die "retro-hub needs ${soname}, which does not resolve"
  cp -L "${path}" "${OUT}/${soname}"
  chmod 0755 "${OUT}/${soname}"
  SDL_NOTE="${soname} from ${path}"
fi

# ---- check the prefix, then print where it is -----------------------------------
HUB="${OUT}/retro-hub"
report="$(env -u LD_LIBRARY_PATH "${HUB}" --version)" || die "${HUB} --version failed"
field() { sed -n "s/^$1 //p" <<<"${report}"; }
[[ "$(field title_app)" == 1 ]] || die "${HUB} --version has no 'title_app 1'"
[[ " $(field direct_mode_flags) " == *" --package "* ]] || die "${HUB} --version lists no --package"
if [[ "${OS}" == linux ]]; then
  env -u LD_LIBRARY_PATH ldd "${HUB}" | grep -qF "${soname} => ${OUT}/${soname}" ||
    die "${HUB} does not resolve ${soname} from the prefix"
fi
runner_report="$(env -u LD_LIBRARY_PATH "${OUT}/retro-core-runner" --version)" ||
  die "${OUT}/retro-core-runner --version failed"
grep -qx 'game_package 1' <<<"${runner_report}" ||
  die "${OUT}/retro-core-runner cannot load a game package (no 'game_package 1')"
bash "${OUT}/packaging/title/build-title-app.sh" --help >/dev/null || die "the title-app kit does not run"

say "${PLATFORM} ${CONFIG} flat hub prefix in ${OUT}"
say "runner $(sed -n 's/^version //p' <<<"${runner_report}") from ${RUNNER_FROM}; ${SDL_NOTE}"
echo "${report}"
echo "RETRO_HUB=${HUB}"
