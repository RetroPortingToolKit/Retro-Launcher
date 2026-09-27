#!/usr/bin/env bash
# Package one title as a local, double-clickable app: an AppImage (Linux) or a
# .app in a .dmg (macOS). Windows: build-title-app.ps1 beside this file.
#
# The title-app kit (docs/RELEASES.md, "Title-app mode"). It runs from a flat
# hub prefix -- <hub dir>/packaging/title/ -- and needs nothing from the
# launcher's source tree. A framework stages the title payload (title.json,
# MANIFEST.txt, the core, the game package) and calls this; the app it makes
# is retro-hub in title-app mode, the runner beside it, and the payload in
# title/ beside the hub.
#
# The app is a LOCAL build. The game package holds ROM-derived generated code,
# so it is never published (recomp-ai-rules SHIPPING.md §1), and the ROM is
# never in it: the hub asks for it on first launch and remembers it.
#
# Gates, each failing the build (SHIPPING.md §4):
#   1. the payload allowlist: every file listed in MANIFEST.txt and present,
#      nothing unlisted, no symlink, no ROM/disc extension, no file starting
#      with an N64 ROM header magic -- and the same scan over the whole app;
#   2. retro-hub and retro-core-runner --version, read back OUT OF THE ARTIFACT
#      (the AppImage extracted, the dmg mounted): title_app 1, and game_package
#      1 when the title has a package;
#   3. `retro-hub --check-title` from the artifact, in a clean directory with a
#      clean environment: it must resolve, and (AppImage) put the data dir
#      beside the AppImage, not in the mount.
#
# The last stdout line is `app <absolute path>`.
#
# usage: build-title-app.sh --title <payload dir> --runner <retro-core-runner>
#          --out <dir> [--hub-dir <flat hub prefix>] [--icon <png>]
#          [--format appimage|dmg] [--version <v>]
#
# Environment:
#   RETRO_HUB_LINUXDEPLOY  a linuxdeploy to use (offline copy / system install)
#   RETRO_HUB_TOOLS_DIR    where downloaded tools are cached
#                          (default ${XDG_CACHE_HOME:-~/.cache}/retro-hub/tools)
set -euo pipefail

die() { echo "build-title-app: $*" >&2; exit 1; }
say() { echo "build-title-app: $*" >&2; }

usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }

KIT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMMON="${KIT}/../common"
[[ -f "${COMMON}/appimage.sh" ]] || die "${COMMON}/appimage.sh missing (a partial kit?)"

TITLE="" RUNNER="" OUT="" HUB_DIR="" ICON="" FORMAT="" APP_VERSION=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --title)   [[ $# -ge 2 ]] || die "--title needs a directory"; TITLE="$2"; shift 2 ;;
    --runner)  [[ $# -ge 2 ]] || die "--runner needs a file"; RUNNER="$2"; shift 2 ;;
    --out)     [[ $# -ge 2 ]] || die "--out needs a directory"; OUT="$2"; shift 2 ;;
    --hub-dir) [[ $# -ge 2 ]] || die "--hub-dir needs a directory"; HUB_DIR="$2"; shift 2 ;;
    --icon)    [[ $# -ge 2 ]] || die "--icon needs a png"; ICON="$2"; shift 2 ;;
    --format)  [[ $# -ge 2 ]] || die "--format needs a value"; FORMAT="$2"; shift 2 ;;
    --version) [[ $# -ge 2 ]] || die "--version needs a value"; APP_VERSION="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument '$1' (see --help)" ;;
  esac
done

# ---- every path absolute, against the directory this was run from --------
abs() { [[ "$1" == /* ]] && printf '%s\n' "$1" || printf '%s\n' "${PWD}/$1"; }
[[ -n "${TITLE}" ]] || die "--title <payload dir> is required"
[[ -n "${RUNNER}" ]] || die "--runner <retro-core-runner> is required"
[[ -n "${OUT}" ]] || die "--out <dir> is required"
TITLE="$(abs "${TITLE}")"
[[ -f "${TITLE}" && "$(basename "${TITLE}")" == title.json ]] && TITLE="$(dirname "${TITLE}")"
[[ -d "${TITLE}" ]] || die "--title ${TITLE}: not a directory"
TITLE="$(cd "${TITLE}" && pwd)"
RUNNER="$(abs "${RUNNER}")"
[[ -f "${RUNNER}" && -x "${RUNNER}" ]] || die "--runner ${RUNNER}: not an executable file"
HUB_DIR="$(abs "${HUB_DIR:-${KIT}/../..}")"
[[ -d "${HUB_DIR}" ]] || die "--hub-dir ${HUB_DIR}: not a directory"
HUB_DIR="$(cd "${HUB_DIR}" && pwd)"
[[ -f "${HUB_DIR}/retro-hub" && ! -L "${HUB_DIR}/retro-hub" ]] ||
  die "${HUB_DIR}/retro-hub: not found (--hub-dir must be a flat hub prefix: scripts/build-local.sh output or the bare hub archive)"
[[ -z "${ICON}" ]] || ICON="$(abs "${ICON}")"
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"
command -v python3 >/dev/null || die "python3 is required"

case "$(uname -s)" in
  Linux)  OS=linux ;;
  Darwin) OS=macos ;;
  *) die "$(uname -s): use build-title-app.ps1 on Windows" ;;
esac
case "$(uname -m)" in
  x86_64|amd64)  ARCH=x86_64 LD_ARCH=x86_64 ;;
  aarch64|arm64) ARCH=arm64 LD_ARCH=aarch64 ;;
  *) die "$(uname -m): no platform name for this architecture" ;;
esac
if [[ -z "${FORMAT}" ]]; then
  FORMAT="$([[ "${OS}" == linux ]] && echo appimage || echo dmg)"
fi
case "${OS}:${FORMAT}" in
  linux:appimage|macos:dmg) ;;
  *:exe) die "--format exe is built on Windows, by build-title-app.ps1" ;;
  *) die "--format ${FORMAT} cannot be built on ${OS}" ;;
esac

WORK="$(mktemp -d "${TMPDIR:-/tmp}/build-title-app.XXXXXX")"
MOUNT=""
cleanup() {
  if [[ -n "${MOUNT}" ]]; then hdiutil detach "${MOUNT}" -force -quiet 2>/dev/null || true; fi
  chmod -R u+w "${WORK}" 2>/dev/null || true
  rm -rf "${WORK}"
}
trap cleanup EXIT

# ---- title.json ------------------------------------------------------------
# One `key<TAB>value` per line; the hub parses it for real at gate 3.
meta="$(python3 - "${TITLE}/title.json" <<'PY'
import json, re, sys
try:
    t = json.load(open(sys.argv[1]))
except Exception as e:
    sys.exit(f"title.json: {e}")
if not isinstance(t, dict) or t.get("schema") != 1:
    sys.exit("title.json: schema must be 1")
tid = t.get("id", "")
if not re.fullmatch(r"[a-z0-9_-]+", tid or ""):
    sys.exit(f"title.json: id '{tid}' must match [a-z0-9_-]+")
for k in ("core",):
    if not t.get(k):
        sys.exit(f"title.json: '{k}' is required")
print("id\t" + tid)
print("name\t" + (t.get("name") or tid))
print("version\t" + (t.get("version") or ""))
print("package\t" + (t.get("package") or ""))
PY
)" || die "$(echo "${meta}" | tail -1)"
meta_field() { awk -F'\t' -v k="$1" '$1 == k { sub(/^[^\t]*\t/, ""); print; exit }' <<<"${meta}"; }
ID="$(meta_field id)"
NAME="$(meta_field name)"
HAS_PACKAGE="$(meta_field package)"
[[ -n "${APP_VERSION}" ]] || APP_VERSION="$(meta_field version)"
[[ -n "${APP_VERSION}" ]] || APP_VERSION="0.0.0"
[[ "${APP_VERSION}" =~ ^[A-Za-z0-9._+-]+$ ]] || die "version '${APP_VERSION}': letters, digits and . _ + - only"
# Spaces in the name are kept; path separators and characters Windows
# refuses are not.
FILE_NAME="$(printf '%s' "${NAME}" | tr '/\\:*?"<>|' '---------')"
BUNDLE_ID="com.retroportingtoolkit.title.${ID//_/-}"

# ---- gate 1: the payload allowlist ------------------------------------------
ROM_EXT_RE='\.(z64|n64|v64|rom|bin|iso|cue|chd|sfc|smc|gba|gb|gbc|nds|gen|sms|nes)$'
# N64 ROM header magic in its three byte orders (z64, v64, n64).
rom_magic() { # <file> -> 0 when it starts with one
  local head
  head="$(od -An -tx1 -N4 "$1" 2>/dev/null | tr -d ' \n')"
  [[ "${head}" == 80371240 || "${head}" == 37804012 || "${head}" == 40123780 ]]
}
# Every file under <dir>: no ROM magic, and -- in the payload (<what> =
# payload) or any title/ inside an app -- no ROM extension and no symlink. The
# extension list is the contract's, so it also refuses a Markdown file (.md is
# a Mega Drive extension); the hub's own fonts/NOTICE.md is outside title/.
scan_for_roms() { # <dir> <what>
  local dir="$1" what="$2" f rel bad="" in_title
  while IFS= read -r -d '' f; do
    rel="${f#"${dir}"/}"
    in_title=0
    [[ "${what}" == payload || "/${rel}" == */title/* ]] && in_title=1
    if [[ -L "${f}" && "${in_title}" == 1 ]]; then
      bad+=$'\n'"  ${rel}: a symlink (the payload holds regular files only)"
      continue
    fi
    [[ -f "${f}" ]] || continue
    # tr, not ${rel,,}: macOS runs this with bash 3.2.
    if [[ "${in_title}" == 1 &&
          "$(printf '%s' "${rel}" | tr '[:upper:]' '[:lower:]')" =~ ${ROM_EXT_RE} ]]; then
      bad+=$'\n'"  ${rel}: a ROM/disc image extension"
    elif rom_magic "${f}"; then
      bad+=$'\n'"  ${rel}: begins with an N64 ROM header"
    fi
  done < <(find "${dir}" \( -type f -o -type l \) -print0)
  [[ -z "${bad}" ]] || die "refusing to package: the ${what} holds a ROM or something that may be one:${bad}"
}
check_payload() {
  local dir="$1" line listed actual
  [[ -f "${dir}/title.json" ]] || die "${dir}/title.json: missing"
  [[ -f "${dir}/MANIFEST.txt" ]] || die "${dir}/MANIFEST.txt: missing"
  scan_for_roms "${dir}" payload
  listed=""
  while IFS= read -r line || [[ -n "${line}" ]]; do
    line="${line%$'\r'}"
    [[ -z "${line}" ]] && continue
    case "/${line}/" in
      */../*|*/./*|//*) die "MANIFEST.txt: '${line}' is not a plain relative path" ;;
    esac
    [[ "${line}" != /* ]] || die "MANIFEST.txt: '${line}' is absolute"
    [[ "${line}" != title.json && "${line}" != MANIFEST.txt ]] ||
      die "MANIFEST.txt lists ${line}, which it must not (it lists every OTHER file)"
    [[ -f "${dir}/${line}" && ! -L "${dir}/${line}" ]] ||
      die "MANIFEST.txt lists ${line}, which is not in the payload"
    listed+="${line}"$'\n'
  done < "${dir}/MANIFEST.txt"
  listed="$(printf '%s' "${listed}" | LC_ALL=C sort -u)"
  # title.json / MANIFEST.txt only count at the root.
  actual="$(cd "${dir}" && { find . \( -type f -o -type l \) | sed 's|^\./||' |
            grep -vx -e title.json -e MANIFEST.txt || true; } | LC_ALL=C sort)"
  local extra
  extra="$(LC_ALL=C comm -13 <(printf '%s\n' "${listed}") <(printf '%s\n' "${actual}"))"
  [[ -z "${extra}" ]] || die "refusing to package: not in MANIFEST.txt:"$'\n'"${extra}"
}
check_payload "${TITLE}"
say "payload ${TITLE}: $(grep -c . "${TITLE}/MANIFEST.txt") listed file(s), allowlist ok"

# Copies exactly the allowlisted payload into <dest>.
copy_payload() {
  local dest="$1" line
  mkdir -p "${dest}"
  cp "${TITLE}/title.json" "${TITLE}/MANIFEST.txt" "${dest}/"
  while IFS= read -r line || [[ -n "${line}" ]]; do
    line="${line%$'\r'}"
    [[ -z "${line}" ]] && continue
    mkdir -p "$(dirname "${dest}/${line}")"
    cp -p "${TITLE}/${line}" "${dest}/${line}"
  done < "${TITLE}/MANIFEST.txt"
}
# The artifact's title/ must be exactly the payload: the same files, byte for
# byte -- nothing added, and nothing rewritten (linuxdeploy is never pointed at
# the core or the package; this proves it left them alone).
sha256_of() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
check_packaged_payload() { # <title dir inside the artifact>
  local got want f
  got="$(cd "$1" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)"
  want="$({ printf 'title.json\nMANIFEST.txt\n'; grep -v '^[[:space:]]*$' "${TITLE}/MANIFEST.txt" | tr -d '\r'; } |
          LC_ALL=C sort -u)"
  [[ "${got}" == "${want}" ]] ||
    die "the artifact's title/ differs from the payload:"$'\n'"$(diff <(echo "${want}") <(echo "${got}") || true)"
  while IFS= read -r f; do
    [[ "$(sha256_of "$1/${f}")" == "$(sha256_of "${TITLE}/${f}")" ]] ||
      die "the artifact's title/${f} is not byte-identical to the payload's (something rewrote it)"
  done <<<"${want}"
}

# ---- the hub's files: an allowlist of what a flat hub prefix holds -----------
# retro-hub, its shared libraries (SDL3), the UI assets, licences. Never the
# kit itself, a runner already there, the portable stub, or anything else.
stage_hub() { # <dest dir for the binaries> <dest dir for assets>
  local bin="$1" res="$2" f kind
  mkdir -p "${bin}" "${res}"
  cp -p "${HUB_DIR}/retro-hub" "${bin}/retro-hub"
  shopt -s nullglob
  for f in "${HUB_DIR}"/lib*.so* "${HUB_DIR}"/*.dylib; do
    [[ -f "${f}" ]] && cp -pL "${f}" "${bin}/$(basename "${f}")"
  done
  shopt -u nullglob
  [[ -f "${HUB_DIR}/retcomm.png" ]] && cp -p "${HUB_DIR}/retcomm.png" "${res}/"
  [[ -f "${HUB_DIR}/LICENSE" ]] && cp -p "${HUB_DIR}/LICENSE" "${res}/"
  for kind in fonts platforms controllers setup licenses; do
    [[ -d "${HUB_DIR}/${kind}" ]] && cp -pR "${HUB_DIR}/${kind}" "${res}/${kind}"
  done
  [[ -f "${res}/fonts/LatoLatin-Regular.ttf" ]] ||
    die "${HUB_DIR}/fonts/LatoLatin-Regular.ttf: missing (not a complete hub prefix)"
  cp -p "${RUNNER}" "${bin}/retro-core-runner"
  chmod 0755 "${bin}/retro-hub" "${bin}/retro-core-runner"
}

# The icon: --icon, else the payload's icon.png, else the hub's own.
if [[ -z "${ICON}" ]]; then
  if [[ -f "${TITLE}/icon.png" ]]; then ICON="${TITLE}/icon.png"
  else ICON="${HUB_DIR}/retcomm.png"; fi
fi
[[ -f "${ICON}" ]] || die "icon ${ICON}: not found"
png_size() { # prints WxH of a PNG, or fails
  python3 - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read(32)
if d[:8] != b"\x89PNG\r\n\x1a\n" or d[12:16] != b"IHDR":
    sys.exit("not a PNG")
w, h = struct.unpack(">II", d[16:24])
print(f"{w}x{h}")
PY
}
ICON_SIZE="$(png_size "${ICON}")" || die "icon ${ICON}: not a PNG"

# ---- gate 2 and 3 helpers -----------------------------------------------------
version_field() { sed -n "s/^$2 //p" <<<"$1"; }
check_versions() { # <hub> <runner>
  local hub_report runner_report
  hub_report="$(env -i PATH=/usr/bin:/bin HOME="${HOME}" "$1" --version)" ||
    die "the packaged retro-hub --version failed"
  runner_report="$(env -i PATH=/usr/bin:/bin HOME="${HOME}" "$2" --version)" ||
    die "the packaged retro-core-runner --version failed"
  [[ "$(version_field "${hub_report}" title_app)" =~ ^[1-9][0-9]*$ ]] ||
    die "the packaged retro-hub has no 'title_app' in --version (a hub from before title-app mode)"
  if [[ -n "${HAS_PACKAGE}" ]]; then
    [[ "$(version_field "${runner_report}" game_package)" == 1 ]] ||
      die "the packaged retro-core-runner cannot load a game package (no 'game_package 1')"
  fi
  HUB_VERSION="$(version_field "${hub_report}" version)"
  RUNNER_VERSION="$(version_field "${runner_report}" version)"
  say "gate: packaged retro-hub ${HUB_VERSION} (title_app $(version_field "${hub_report}" title_app), direct_mode $(version_field "${hub_report}" direct_mode)); retro-core-runner ${RUNNER_VERSION}"
}
# Runs `<cmd...> --check-title` from an empty directory with a clean
# environment (plus the NAME=value pairs before `--`), and returns its report.
check_title_report() {
  local clean="${WORK}/clean-cwd" envs=() report
  while [[ "$1" != -- ]]; do envs+=("$1"); shift; done
  shift
  rm -rf "${clean}"
  mkdir -p "${clean}"
  # ${envs[@]+...}: an empty array is "unbound" under set -u in bash < 4.4.
  report="$(cd "${clean}" && env -i PATH=/usr/bin:/bin HOME="${HOME}" ${envs[@]+"${envs[@]}"} "$@" --check-title)" || {
    echo "${report}" >&2
    die "gate: --check-title failed in the packaged app"
  }
  [[ -z "$(ls -A "${clean}")" ]] || die "gate: --check-title wrote into its working directory"
  printf '%s\n' "${report}"
}

# ============================================================================
if [[ "${FORMAT}" == appimage ]]; then
  # shellcheck source=../common/appimage.sh
  source "${COMMON}/appimage.sh"
  [[ -f "${COMMON}/AppRun.in" ]] || die "${COMMON}/AppRun.in missing"
  ARTIFACT="${OUT}/${FILE_NAME}-${APP_VERSION}-linux-${ARCH}.AppImage"
  APPDIR="${WORK}/${ID}.AppDir"
  stage_hub "${APPDIR}/usr/bin" "${APPDIR}/usr/bin"
  # exe_dir/title/: where the hub finds its title.
  copy_payload "${APPDIR}/usr/bin/title"
  sed "s|@VERSION@|${APP_VERSION}|g" "${COMMON}/AppRun.in" > "${APPDIR}/AppRun"
  chmod 0755 "${APPDIR}/AppRun"
  # Desktop entry: the title's name; Exec is the hub (AppRun runs it).
  desktop_name="${NAME//$'\n'/ }"
  cat > "${APPDIR}/${ID}.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=${desktop_name}
Comment=${desktop_name} (a local Retro title app)
Exec=retro-hub
Icon=${ID}
Categories=Game;
Terminal=false
EOF
  # linuxdeploy takes only its standard icon sizes (square, <= 512).
  case "${ICON_SIZE}" in
    16x16|24x24|32x32|48x48|64x64|128x128|256x256|512x512)
      cp "${ICON}" "${APPDIR}/${ID}.png" ;;
    *)
      conv="$(command -v magick || command -v convert || true)"
      [[ -n "${conv}" ]] ||
        die "icon ${ICON} is ${ICON_SIZE}; give a square 256 or 512 px PNG (or install ImageMagick to resize it)"
      "${conv}" "${ICON}" -resize 512x512 -background none -gravity center -extent 512x512 \
        "${APPDIR}/${ID}.png"
      say "icon resized from ${ICON_SIZE} to 512x512" ;;
  esac
  # Belt and braces: nothing ROM-like anywhere in what is about to be packed.
  scan_for_roms "${APPDIR}" app

  TOOLS="${RETRO_HUB_TOOLS_DIR:-${XDG_CACHE_HOME:-${HOME}/.cache}/retro-hub/tools}"
  LINUXDEPLOY="$(rh_linuxdeploy "${LD_ARCH}" "${TOOLS}")"
  rm -f "${ARTIFACT}"
  export LDAI_OUTPUT="${ARTIFACT}"
  export LINUXDEPLOY_OUTPUT_VERSION="${APP_VERSION}"
  (cd "${WORK}" && rh_run_linuxdeploy "${LINUXDEPLOY}" \
      --appdir "${APPDIR}" \
      --executable "${APPDIR}/usr/bin/retro-hub" \
      --executable "${APPDIR}/usr/bin/retro-core-runner" \
      --desktop-file "${APPDIR}/${ID}.desktop" \
      --icon-file "${APPDIR}/${ID}.png" \
      --output appimage) >&2 || die "linuxdeploy failed"
  [[ -f "${ARTIFACT}" ]] || die "linuxdeploy produced no ${ARTIFACT}"
  chmod 0755 "${ARTIFACT}"

  # ---- the gates, on the artifact itself -----------------------------------
  rh_extract_appimage "${ARTIFACT}" "${WORK}/check" || die "gate: cannot extract ${ARTIFACT}"
  ROOTFS="${WORK}/check/squashfs-root"
  check_packaged_payload "${ROOTFS}/usr/bin/title"
  scan_for_roms "${ROOTFS}" "packaged AppImage"
  # The core and package load into the runner: whatever they import must be
  # in the AppImage or a system library (they are not rewritten by linuxdeploy).
  # A library shipped anywhere in the payload satisfies an import of its name
  # or soname: the generic core contract has the runner dlopen the core
  # first, so the package finds it by soname with no RPATH to it.
  payload_libs=" "
  while IFS= read -r -d '' elf; do
    payload_libs+="$(basename "${elf}") "
    so="$( { readelf -d "${elf}" 2>/dev/null || true; } | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')"
    [[ -z "${so}" ]] || payload_libs+="${so} "
  done < <(find "${ROOTFS}/usr/bin/title" -type f -print0)
  system_ok='^(libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|ld-linux-(x86-64|aarch64)\.so\.[0-9]+|libstdc\+\+\.so\.6|libgcc_s\.so\.1|libGL\.so\.1|libOpenGL\.so\.0|libGLX\.so\.0|libEGL\.so\.1|libvulkan\.so\.1|libasound\.so\.2|libpulse\.so\.0|libX11\.so\.6|libxcb\.so\.1|libwayland-client\.so\.0)$'
  while IFS= read -r -d '' elf; do
    head4="$(od -An -tx1 -N4 "${elf}" | tr -d ' \n')"
    [[ "${head4}" == 7f454c46 ]] || continue
    while read -r lib; do
      [[ -z "${lib}" ]] && continue
      [[ "${lib}" =~ ${system_ok} || -e "${ROOTFS}/usr/lib/${lib}" || -e "${ROOTFS}/usr/bin/${lib}" ||
         "${payload_libs}" == *" ${lib} "* ]] ||
        die "gate: ${elf#"${ROOTFS}"/} imports ${lib}, which is neither in the AppImage nor a system library"
    done < <(readelf -d "${elf}" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
  done < <(find "${ROOTFS}/usr/bin/title" -type f -print0)
  check_versions "${ROOTFS}/usr/bin/retro-hub" "${ROOTFS}/usr/bin/retro-core-runner"
  # APPIMAGE is what the AppImage runtime sets; the hub must anchor its data
  # dir to it (SHIPPING.md §5), never to the mount.
  report="$(check_title_report "APPIMAGE=${ARTIFACT}" -- "${ROOTFS}/AppRun")"
  field() { sed -n "s/^$1 //p" <<<"${report}"; }
  [[ "$(field title)" == "${ROOTFS}/usr/bin/title/title.json" ]] ||
    die "gate: the packaged hub resolved title '$(field title)', not its own"
  [[ "$(field runner)" == "${ROOTFS}/usr/bin/retro-core-runner" ]] ||
    die "gate: the packaged hub resolved runner '$(field runner)', not the bundled one"
  [[ "$(field data_dir)" == "${OUT}/${ID}-data" ]] ||
    die "gate: data dir '$(field data_dir)' is not beside the AppImage (${OUT}/${ID}-data)"
  [[ ! -e "${OUT}/${ID}-data" ]] || say "note: ${OUT}/${ID}-data already exists (an earlier run of the app)"
  say "gate: --check-title ok from a clean directory; data dir beside the AppImage"

# ============================================================================
else # dmg
  # shellcheck source=../common/macos.sh
  source "${COMMON}/macos.sh"
  ARTIFACT="${OUT}/${FILE_NAME}-${APP_VERSION}-macos-${ARCH}.dmg"
  APP="${WORK}/${FILE_NAME}.app"
  stage_hub "${APP}/Contents/MacOS" "${APP}/Contents/Resources"
  # Shared libraries go to Frameworks, where bundle_dylibs.sh finds them.
  mkdir -p "${APP}/Contents/Frameworks"
  shopt -s nullglob
  for f in "${APP}/Contents/MacOS"/*.dylib "${APP}/Contents/MacOS"/lib*.so*; do
    mv "${f}" "${APP}/Contents/Frameworks/"
  done
  shopt -u nullglob
  # <exe_dir>/../Resources/title/: where a macOS hub finds its title.
  copy_payload "${APP}/Contents/Resources/title"
  rh_render_plist "${COMMON}/Info.plist.in" "${APP}/Contents/Info.plist" "${NAME}" "${BUNDLE_ID}" "${APP_VERSION}"
  rh_make_icns "${ICON}" "${APP}/Contents/Resources/AppIcon.icns" "${WORK}"
  scan_for_roms "${APP}" app
  "${COMMON}/bundle_dylibs.sh" "${APP}" >&2
  rh_create_dmg "${APP}" "${NAME}" "${ARTIFACT}" "${WORK}"
  [[ -f "${ARTIFACT}" ]] || die "no ${ARTIFACT} was produced"

  # ---- the gates, on the mounted dmg ---------------------------------------
  MOUNT="${WORK}/mnt"
  mkdir -p "${MOUNT}"
  hdiutil attach -nobrowse -readonly -noautoopen -mountpoint "${MOUNT}" "${ARTIFACT}" >/dev/null ||
    die "gate: cannot mount ${ARTIFACT}"
  MAPP="${MOUNT}/$(basename "${APP}")"
  check_packaged_payload "${MAPP}/Contents/Resources/title"
  scan_for_roms "${MAPP}" "packaged app"
  check_versions "${MAPP}/Contents/MacOS/retro-hub" "${MAPP}/Contents/MacOS/retro-core-runner"
  report="$(check_title_report -- "${MAPP}/Contents/MacOS/retro-hub")"
  field() { sed -n "s/^$1 //p" <<<"${report}"; }
  [[ "$(field title)" == "${MAPP}/Contents/Resources/title/title.json" ]] ||
    die "gate: the packaged hub resolved title '$(field title)', not its own"
  # A mounted dmg is read-only: the data dir must NOT be inside it (the hub
  # falls back to the user data directory there).
  case "$(field data_dir)" in
    "${MOUNT}"/*) die "gate: data dir '$(field data_dir)' is inside the mounted dmg" ;;
  esac
  hdiutil detach "${MOUNT}" -quiet || hdiutil detach "${MOUNT}" -force -quiet || true
  MOUNT=""
  say "gate: --check-title ok from the mounted dmg; data dir $(field data_dir)"
fi

say "${NAME} ${APP_VERSION} (${ID}): hub ${HUB_VERSION}, runner ${RUNNER_VERSION}"
echo "app ${ARTIFACT}"
