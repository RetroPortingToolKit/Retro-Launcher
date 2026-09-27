# shellcheck shell=bash
# AppImage helpers shared by the launcher's AppImage (packaging/linux/
# build-appimage.sh) and the title-app kit (packaging/title/build-title-app.sh).
# Source this file; it defines functions only.
#
#   rh_linuxdeploy <arch> <tools dir>
#       Prints the linuxdeploy to use: $RETRO_HUB_LINUXDEPLOY when set (an
#       offline copy, or a system install), else <tools dir>/linuxdeploy-<arch>
#       .AppImage, downloaded there once (the continuous release).
#   rh_run_linuxdeploy <linuxdeploy> <args...>
#       Runs it with --appimage-extract-and-run when that works (no FUSE in CI
#       or containers), else directly. NO_STRIP defaults to 1: linuxdeploy's
#       bundled strip rejects modern ELF (RELR) from current toolchains.
#   rh_extract_appimage <appimage> <dest dir>
#       Unpacks it to <dest dir>/squashfs-root: --appimage-extract, else
#       unsquashfs at the type-2 squashfs offset. Fails when neither works.

rh_linuxdeploy() {
  local arch="$1" tools="$2" tool
  if [[ -n "${RETRO_HUB_LINUXDEPLOY:-}" ]]; then
    [[ -x "${RETRO_HUB_LINUXDEPLOY}" ]] || {
      echo "error: RETRO_HUB_LINUXDEPLOY=${RETRO_HUB_LINUXDEPLOY} is not an executable" >&2
      return 1
    }
    printf '%s\n' "${RETRO_HUB_LINUXDEPLOY}"
    return 0
  fi
  mkdir -p "${tools}"
  tool="${tools}/linuxdeploy-${arch}.AppImage"
  if [[ ! -x "${tool}" ]]; then
    echo "fetching linuxdeploy-${arch}.AppImage into ${tools}" >&2
    curl -fsSL -o "${tool}.part" \
      "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${arch}.AppImage" || {
      rm -f "${tool}.part"
      echo "error: cannot download linuxdeploy; set RETRO_HUB_LINUXDEPLOY to a local copy" >&2
      return 1
    }
    chmod +x "${tool}.part"
    mv -f "${tool}.part" "${tool}"
  fi
  printf '%s\n' "${tool}"
}

rh_run_linuxdeploy() {
  local tool="$1"
  shift
  export NO_STRIP="${NO_STRIP:-1}"
  if "${tool}" --appimage-extract-and-run --version >/dev/null 2>&1; then
    "${tool}" --appimage-extract-and-run "$@"
  else
    "${tool}" "$@"
  fi
}

rh_extract_appimage() {
  local image="$1" dest="$2" offset
  rm -rf "${dest}"
  mkdir -p "${dest}"
  image="$(cd "$(dirname "${image}")" && pwd)/$(basename "${image}")"
  if (cd "${dest}" && "${image}" --appimage-extract >/dev/null 2>&1) &&
     [[ -d "${dest}/squashfs-root" ]]; then
    return 0
  fi
  rm -rf "${dest}/squashfs-root"
  command -v unsquashfs >/dev/null 2>&1 || {
    echo "error: cannot extract ${image}: --appimage-extract failed and unsquashfs is missing" >&2
    return 1
  }
  # Type-2 AppImage: ELF runtime + squashfs. Find the last little-endian
  # "hsqs" magic at or after the end of the ELF's segments (false positives
  # can appear earlier in the ELF).
  offset="$(python3 - "${image}" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
if data[:4] != b"\x7fELF":
    sys.exit("not ELF")
end = 0
if data[4] == 2:  # ELFCLASS64
    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize = struct.unpack_from("<H", data, 54)[0]
    e_phnum = struct.unpack_from("<H", data, 56)[0]
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_offset, _vaddr, _paddr, p_filesz = struct.unpack_from("<QQQQ", data, off + 8)
        end = max(end, p_offset + p_filesz)
cands = [i for i in range(len(data) - 3) if data[i : i + 4] == b"hsqs"]
if not cands:
    sys.exit("no hsqs magic")
after = [i for i in cands if i + 64 >= end]
print(after[-1] if after else cands[-1])
PY
)" || return 1
  echo "unsquashfs offset=${offset}" >&2
  unsquashfs -o "${offset}" -d "${dest}/squashfs-root" "${image}" >/dev/null
}
