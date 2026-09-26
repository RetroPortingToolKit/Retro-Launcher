#!/usr/bin/env bash
# Package the bare (non-installer) retro-hub archive for one Linux platform.
#
# For a tool that runs the hub in Direct mode (`retro-hub --run-core <core>
# --rom ... --title-dir ...`) without an AppImage -- n64lle's new-project
# scaffolder, in "release" mode. The contract, the asset names and the manifest
# are in docs/RELEASES.md. Shaped on Retro-Runtime's scripts/package-release.sh:
# every check below is a gate, it fails the package rather than warning.
#
# It packages an existing CMake install prefix -- the one the release
# workflow's linux job already builds for the AppImage -- rather than building
# a second time. That prefix must hold bin/retro-hub, share/retcomm/<assets>,
# and lib/libSDL3.so.* (the job copies the pinned SDL3 there).
#
#   - the archive holds exactly the allowlisted files, flat at the root, and
#     never retro-core-runner, even though the prefix holds the prebaked one
#     (a tool takes it from Retro-Runtime's own release and either places it
#     beside retro-hub or names it with RETRO_CORE_RUNNER);
#   - SDL3 ships beside the hub (libSDL3.so.0, found through RUNPATH $ORIGIN):
#     the hub is linked against a shared SDL3 and cannot start without it, and
#     SDL3 is not packaged by the older distros this archive targets;
#   - every other library the hub imports must be on the system allowlist;
#   - the hub INSIDE the archive, extracted to a clean directory, must report
#     this release's version and commit (`--version`), resolve SDL3 from the
#     archive, and accept the Direct-mode command line.
#
# Output in --out:
#   retro-hub-<version>-<platform>.tar.gz
#   <archive>.sha256
#   retro-hub-<version>-<platform>.json     this build's manifest entry, merged
#                                           by scripts/hub_manifest.py
#
# usage: scripts/package_hub_archive.sh --version 0.1.2 --platform linux-x86_64 \
#          --prefix DIR --build DIR --sdl3-prefix DIR [--commit SHA] [--out DIR]
set -euo pipefail

die() { echo "package_hub_archive: $*" >&2; exit 1; }

VERSION="" PLATFORM="" PREFIX="" BUILD="" SDL3_PREFIX="" COMMIT="" OUT="dist"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)     VERSION="$2"; shift 2 ;;
    --platform)    PLATFORM="$2"; shift 2 ;;
    --prefix)      PREFIX="$2"; shift 2 ;;
    --build)       BUILD="$2"; shift 2 ;;
    --sdl3-prefix) SDL3_PREFIX="$2"; shift 2 ;;
    --commit)      COMMIT="$2"; shift 2 ;;
    --out)         OUT="$2"; shift 2 ;;
    *) die "unknown argument $1" ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -n "${VERSION}" ]] || die "--version is required"
[[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.+-][A-Za-z0-9.+-]*)?$ ]] ||
  die "--version ${VERSION}: expected semver without a leading v"
case "${PLATFORM}" in
  linux-x86_64|linux-arm64) ;;
  # Direct mode is built on every OS, but only the Linux job packages a bare
  # archive so far; Windows and macOS users get it inside the installers.
  *) die "--platform: want linux-x86_64 or linux-arm64, got '${PLATFORM}'" ;;
esac
[[ -n "${PREFIX}" && -d "${PREFIX}" ]] || die "--prefix: a cmake --install prefix is required"
[[ -n "${BUILD}" && -f "${BUILD}/CMakeCache.txt" ]] || die "--build: the hub's build directory is required"
[[ -n "${SDL3_PREFIX}" && -d "${SDL3_PREFIX}" ]] || die "--sdl3-prefix: the pinned SDL3 install is required"
[[ -n "${COMMIT}" ]] || COMMIT="$(git -C "${ROOT}" rev-parse HEAD)"
PREFIX="$(cd "${PREFIX}" && pwd)"
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

NAME="retro-hub-${VERSION}-${PLATFORM}"
ARCHIVE_NAME="${NAME}.tar.gz"
ARCHIVE="${OUT}/${ARCHIVE_NAME}"
HUB="${PREFIX}/bin/retro-hub"
ASSETS="${PREFIX}/share/retcomm"
[[ -f "${HUB}" && ! -L "${HUB}" ]] || die "${HUB}: not installed"

# ---- stage: the hub, SDL3, the data it loads beside itself, licences -----
STAGE="$(mktemp -d)"
CLEAN=""
trap 'rm -rf "${STAGE}" "${CLEAN}"' EXIT
files=()
add() { # <source> <path in archive> [mode]
  [[ -f "$1" ]] || die "$1: missing (wanted as $2)"
  mkdir -p "$(dirname "${STAGE}/$2")"
  cp -L "$1" "${STAGE}/$2"
  chmod "${3:-0644}" "${STAGE}/$2"
  files+=("$2")
}

add "${HUB}" retro-hub 0755
# The soname the hub imports, as a real file (no symlinks in the archive).
SDL_SONAME="$(readelf -d "${HUB}" | sed -n 's/.*(NEEDED).*\[\(libSDL3\.so[^]]*\)\]/\1/p')"
[[ -n "${SDL_SONAME}" ]] || die "retro-hub does not import libSDL3 (built without the hub UI?)"
add "${PREFIX}/lib/${SDL_SONAME}" "${SDL_SONAME}" 0755

# What the hub finds beside its executable (src/hub/hub_main.cpp,
# find_hub_asset_file: SDL_GetBasePath()/<kind>/, and <base>/retcomm.png) --
# the same set build-appimage.sh places beside usr/bin. Only fonts matter to
# Direct mode (without them ImGui's default face is used); the art is for the
# library UI, which the same binary also runs.
add "${ASSETS}/retcomm.png" retcomm.png
for f in LatoLatin-Regular.ttf LatoLatin-Bold.ttf NOTICE.md; do
  add "${ASSETS}/fonts/${f}" "fonts/${f}"
done
for kind in platforms controllers setup; do
  [[ -d "${ASSETS}/${kind}" ]] || die "${ASSETS}/${kind}: not installed"
  while IFS= read -r f; do
    add "${ASSETS}/${kind}/${f}" "${kind}/${f}"
  done < <(cd "${ASSETS}/${kind}" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)
done
[[ -f "${STAGE}/platforms/psx.png" ]] || die "platform art missing"
[[ -f "${STAGE}/setup/setup_easy_rocket.png" ]] || die "setup card art missing"

# Licences: this project, SDL3 (shipped beside it), and what is linked
# statically into retro-hub (Dear ImGui with imgui_freetype, miniz, stb,
# nlohmann/json, Retro-Runtime's corelink). The Lato fonts' OFL notice is
# fonts/NOTICE.md.
add "${ROOT}/LICENSE" LICENSE
add "${SDL3_PREFIX}/share/licenses/SDL3/LICENSE.txt" licenses/SDL3.txt
IMGUI_DIR="$(sed -n 's/^RETCOMM_IMGUI_DIR:[A-Z]*=//p' "${BUILD}/CMakeCache.txt")"
add "${IMGUI_DIR}/LICENSE.txt" licenses/dear-imgui.txt
add "${ROOT}/third_party/miniz/LICENSE" licenses/miniz.txt
sed -n '/^This software is available under 2 licenses/,$p' "${ROOT}/third_party/stb/stb_image.h" \
  > "${STAGE}/stb.txt"
[[ -s "${STAGE}/stb.txt" ]] || die "stb licence text not found in stb_image.h"
add "${STAGE}/stb.txt" licenses/stb.txt
rm -f "${STAGE}/stb.txt"
add "${ROOT}/third_party/nlohmann/LICENSE.MIT" licenses/nlohmann-json.txt
add "${ROOT}/third_party/Retro-Runtime/LICENSE" licenses/Retro-Runtime.txt

expected_files="$(printf '%s\n' "${files[@]}" | LC_ALL=C sort)"
actual_files="$(cd "${STAGE}" && find . \( -type f -o -type l \) | sed 's|^\./||' | LC_ALL=C sort)"
[[ "${actual_files}" == "${expected_files}" ]] ||
  die "archive contents differ from the allowlist:"$'\n'"${actual_files}"
[[ ! -e "${STAGE}/retro-core-runner" ]] ||
  die "retro-core-runner must not be in the hub archive (it comes from Retro-Runtime's release)"

# ---- what the hub may import from the system -----------------------------
# Everything else it needs is in the archive. The GL, curl and FreeType
# libraries are the host's own (the driver has to be); libstdc++ and libgcc_s
# are recorded as a floor below rather than bundled.
system_ok='^(libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|ld-linux-(x86-64|aarch64)\.so\.[0-9]+|libstdc\+\+\.so\.6|libgcc_s\.so\.1|libcurl\.so\.4|libGL\.so\.1|libOpenGL\.so\.0|libGLX\.so\.0|libEGL\.so\.1|libfreetype\.so\.6)$'
bad="" system_libs=()
for elf in retro-hub "${SDL_SONAME}"; do
  while read -r lib; do
    [[ -f "${STAGE}/${lib}" ]] && continue
    if [[ "${lib}" =~ ${system_ok} ]]; then system_libs+=("${lib}"); else bad+=" ${elf}:${lib}"; fi
  done < <(readelf -d "${STAGE}/${elf}" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
done
[[ -z "${bad}" ]] || die "imports neither in the archive nor on the system allowlist:${bad}"
readelf -d "${STAGE}/retro-hub" | grep -E '\((RUNPATH|RPATH)\)' | grep -q '\$ORIGIN' ||
  die "retro-hub has no \$ORIGIN RUNPATH, so it cannot find the bundled ${SDL_SONAME}"

# The newest symbol version either binary needs is the oldest distro it runs on.
newest() { # <prefix>
  objdump -T "${STAGE}/retro-hub" "${STAGE}/${SDL_SONAME}" | grep -o "$1[0-9][0-9.]*" |
    sed "s/$1//" | sort -t. -k1,1n -k2,2n -k3,3n -u | tail -1
}
GLIBC_FLOOR="$(newest GLIBC_)"
GLIBCXX_FLOOR="$(newest GLIBCXX_)"
[[ -n "${GLIBC_FLOOR}" && -n "${GLIBCXX_FLOOR}" ]] || die "could not read the symbol versions"

# Flat at the root, so a tool extracts it straight into the directory it
# then drops retro-core-runner into.
rm -f "${ARCHIVE}"
tar -C "${STAGE}" --owner=0 --group=0 --numeric-owner -czf "${ARCHIVE}" "${files[@]}"

# ---- read the claims back out of the archive, in a clean directory -------
CLEAN="$(mktemp -d)"
tar -C "${CLEAN}" -xzf "${ARCHIVE}"
EXPECT_RUNNER_LOOKUP="RETRO_CORE_RUNNER exe_dir/retro-core-runner data_dir/runtime/<version>/retro-core-runner"
report="$(env -u LD_LIBRARY_PATH "${CLEAN}/retro-hub" --version)" ||
  die "the archived hub failed to run --version"
field() { sed -n "s/^$1 //p" <<<"${report}"; }
[[ "$(field version)" == "${VERSION}" ]] ||
  die "the archived hub says version '$(field version)', this release is ${VERSION}"
[[ "$(field commit)" == "${COMMIT}" ]] ||
  die "the archived hub says commit '$(field commit)', this release is ${COMMIT}"
[[ "$(field direct_mode)" =~ ^[1-9][0-9]*$ ]] || die "the archived hub was built without Direct mode"
[[ "$(field runner_lookup)" == "${EXPECT_RUNNER_LOOKUP}" ]] ||
  die "the archived hub looks for the runner at '$(field runner_lookup)'"
# SDL3 must come from the archive, not from whatever the machine has.
env -u LD_LIBRARY_PATH ldd "${CLEAN}/retro-hub" | grep -F "${SDL_SONAME} => ${CLEAN}/${SDL_SONAME}" >/dev/null ||
  die "the archived hub does not resolve ${SDL_SONAME} from the archive"
# The Direct-mode parser is live: --run-core without --rom is refused before
# SDL starts, with exit 2.
set +e
direct_err="$(env -u LD_LIBRARY_PATH "${CLEAN}/retro-hub" --run-core "${CLEAN}/none.so" 2>&1 >/dev/null)"
direct_rc=$?
set -e
[[ ${direct_rc} -eq 2 && "${direct_err}" == *"--run-core needs --rom"* ]] ||
  die "the archived hub did not refuse --run-core without --rom (exit ${direct_rc}: ${direct_err})"

# ---- record what shipped --------------------------------------------------
SHA256="$(sha256sum "${ARCHIVE}" | cut -d' ' -f1)"
echo "${SHA256}  ${ARCHIVE_NAME}" > "${ARCHIVE}.sha256"
SIZE="$(stat -c %s "${ARCHIVE}")"
proto="$(field link_protocol)"
python3 - "${OUT}/${NAME}.json" <<EOF
import json, sys
entry = {
    "platform": "${PLATFORM}",
    "serves": ["${PLATFORM}"],
    "version": "${VERSION}",
    "commit": "${COMMIT}",
    "archive": "${ARCHIVE_NAME}",
    "sha256": "${SHA256}",
    "size": ${SIZE},
    "executable": "retro-hub",
    "files": """$(printf '%s\n' "${files[@]}")""".split(),
    "requires": {"glibc": "${GLIBC_FLOOR}", "glibcxx": "${GLIBCXX_FLOOR}"},
    "system_libraries": sorted(set("""${system_libs[*]}""".split())),
    "link_protocol": {"major": ${proto%%.*}, "minor": ${proto#*.}},
    "rcore_abi": {"major": $(field rcore_abi_major),
                  "draft_revision": $(field rcore_draft_revision)},
    "direct_mode": {"cli_revision": $(field direct_mode),
                    "flags": "$(field direct_mode_flags)".split(),
                    "runner_lookup": "$(field runner_lookup)"},
}
open(sys.argv[1], "w").write(json.dumps(entry, indent=2) + "\n")
EOF
echo "package_hub_archive: ${ARCHIVE}"
echo "  sha256 ${SHA256}, ${SIZE} bytes, glibc ${GLIBC_FLOOR}, glibcxx ${GLIBCXX_FLOOR}, link ${proto}"
