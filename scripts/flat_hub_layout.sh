# shellcheck shell=bash
# What a FLAT hub prefix holds, taken from a CMake install prefix: one list,
# shared by the bare hub archive (scripts/package_hub_archive.sh) and the
# local build (scripts/build-local.sh). Source this file.
#
#   flat_hub_entries <install prefix> [<exe suffix>]
#       One `<source>\t<path in the flat prefix>\t<mode>` line per file:
#       retro-hub, the assets it finds beside itself (src/hub/hub_main.cpp,
#       find_hub_asset_file: <exe dir>/<kind>/ and <exe dir>/retcomm.png), and
#       the title-app packaging kit at packaging/title/ with the scripts it
#       shares at packaging/common/. Fails when one is not installed.
#
# Not listed, because each caller decides: SDL3, the runner, licences, and
# (Windows) the portable stub.

flat_hub_entries() {
  local prefix="$1" exe="${2:-}" assets kind f mode
  assets="${prefix}/share/retcomm"
  [[ -f "${prefix}/bin/retro-hub${exe}" ]] || { echo "flat_hub_entries: ${prefix}/bin/retro-hub${exe}: not installed" >&2; return 1; }
  printf '%s\t%s\t%s\n' "${prefix}/bin/retro-hub${exe}" "retro-hub${exe}" 0755
  printf '%s\t%s\t%s\n' "${assets}/retcomm.png" retcomm.png 0644
  for f in LatoLatin-Regular.ttf LatoLatin-Bold.ttf NOTICE.md; do
    printf '%s\t%s\t%s\n' "${assets}/fonts/${f}" "fonts/${f}" 0644
  done
  for kind in platforms controllers setup packaging/title packaging/common; do
    [[ -d "${assets}/${kind}" ]] || { echo "flat_hub_entries: ${assets}/${kind}: not installed" >&2; return 1; }
    while IFS= read -r f; do
      mode=0644
      [[ -x "${assets}/${kind}/${f}" ]] && mode=0755
      printf '%s\t%s\t%s\n' "${assets}/${kind}/${f}" "${kind}/${f}" "${mode}"
    done < <(cd "${assets}/${kind}" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)
  done
}
