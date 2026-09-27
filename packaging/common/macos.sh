# shellcheck shell=bash
# macOS bundle helpers shared by the launcher's .app (packaging/macos/
# build-app.sh) and the title-app kit (packaging/title/build-title-app.sh).
# Source this file; it defines functions only. bundle_dylibs.sh beside it
# copies non-system dylibs into the bundle and signs it.
#
#   rh_render_plist <Info.plist.in> <out> <name> <bundle id> <version>
#   rh_make_icns <png> <out.icns> <work dir>     (sips + iconutil)
#   rh_create_dmg <app> <volume name> <out.dmg> <work dir>
#       A drag-to-Applications DMG (.app + /Applications link), UDZO.
#       RETCOMM_DMG_FINDER_LAYOUT=1 (interactive, not CI) arranges the icons.

rh_xml_escape() {
  local s="$1"
  s="${s//&/&amp;}"
  s="${s//</&lt;}"
  s="${s//>/&gt;}"
  printf '%s' "${s}"
}

rh_render_plist() {
  local template="$1" out="$2" name id version
  name="$(rh_xml_escape "$3")"
  id="$(rh_xml_escape "$4")"
  version="$(rh_xml_escape "$5")"
  python3 - "${template}" "${out}" "${name}" "${id}" "${version}" <<'PY'
import sys
src, out, name, bid, ver = sys.argv[1:6]
text = open(src).read()
for k, v in (("@NAME@", name), ("@BUNDLE_ID@", bid), ("@VERSION@", ver)):
    text = text.replace(k, v)
open(out, "w").write(text)
PY
}

rh_make_icns() {
  local png="$1" out="$2" work="$3" iconset
  iconset="${work}/app.iconset"
  rm -rf "${iconset}"
  mkdir -p "${iconset}"
  sips -z 16 16     "${png}" --out "${iconset}/icon_16x16.png" >/dev/null
  sips -z 32 32     "${png}" --out "${iconset}/icon_16x16@2x.png" >/dev/null
  sips -z 32 32     "${png}" --out "${iconset}/icon_32x32.png" >/dev/null
  sips -z 64 64     "${png}" --out "${iconset}/icon_32x32@2x.png" >/dev/null
  sips -z 128 128   "${png}" --out "${iconset}/icon_128x128.png" >/dev/null
  sips -z 256 256   "${png}" --out "${iconset}/icon_128x128@2x.png" >/dev/null
  sips -z 256 256   "${png}" --out "${iconset}/icon_256x256.png" >/dev/null
  sips -z 512 512   "${png}" --out "${iconset}/icon_256x256@2x.png" >/dev/null
  sips -z 512 512   "${png}" --out "${iconset}/icon_512x512.png" >/dev/null
  sips -z 1024 1024 "${png}" --out "${iconset}/icon_512x512@2x.png" >/dev/null
  iconutil -c icns "${iconset}" -o "${out}"
  rm -rf "${iconset}"
}

rh_create_dmg() {
  local app="$1" volume="$2" dmg="$3" work="$4"
  local app_name stage rw_dmg mount_dir use_finder=0
  app_name="$(basename "${app}")"
  stage="${work}/dmg-staging"
  rw_dmg="${work}/.dmg-rw.dmg"
  mount_dir="${work}/dmg-mount"
  rm -rf "${stage}" "${mount_dir}"
  rm -f "${dmg}" "${rw_dmg}"
  mkdir -p "${stage}"
  # ditto preserves resource forks / signatures better than cp -R.
  ditto "${app}" "${stage}/${app_name}"
  ln -s /Applications "${stage}/Applications"

  _rh_udzo() {
    # Direct UDZO — no attach/Finder. Reliable on headless CI runners.
    hdiutil create -srcfolder "${stage}" -volname "${volume}" -fs HFS+ \
      -fsargs "-c c=64,a=16,e=16" -format UDZO -imagekey zlib-level=9 -ov "${dmg}" >/dev/null
  }
  _rh_detach() {
    local mount="$1"
    sync || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      hdiutil detach "${mount}" -quiet 2>/dev/null && return 0
      hdiutil detach "${mount}" -force -quiet 2>/dev/null && return 0
      sleep 1
    done
    # Last resort: detach by image path if mountpoint is already gone/confused.
    hdiutil detach "${rw_dmg}" -force -quiet 2>/dev/null || true
    if mount | grep -F " on ${mount} " >/dev/null 2>&1; then
      return 1
    fi
    return 0
  }

  # CI / headless: skip Finder icon layout (osascript "tell disk …" flakes and
  # leaves the RW image busy so convert fails). Local interactive builds can opt
  # into the classic drag-install sheet with RETCOMM_DMG_FINDER_LAYOUT=1.
  if [[ "${RETCOMM_DMG_FINDER_LAYOUT:-}" == "1" && -z "${CI:-}" && -z "${GITHUB_ACTIONS:-}" ]]; then
    use_finder=1
  fi

  if [[ "${use_finder}" -eq 1 ]]; then
    hdiutil create -srcfolder "${stage}" -volname "${volume}" -fs HFS+ \
      -fsargs "-c c=64,a=16,e=16" -format UDRW -ov "${rw_dmg}" >/dev/null
    mkdir -p "${mount_dir}"
    if hdiutil attach -readwrite -noverify -noautoopen \
        -mountpoint "${mount_dir}" "${rw_dmg}" >/dev/null \
        && [[ -d "${mount_dir}" ]]; then
      # Address the mount by POSIX path — volume-name lookup fails when attached
      # at a custom mountpoint (and on runners without a working Finder).
      set +e
      osascript <<EOF
tell application "Finder"
  set volAlias to (POSIX file "${mount_dir}") as alias
  open volAlias
  set win to container window of volAlias
  set current view of win to icon view
  set toolbar visible of win to false
  set statusbar visible of win to false
  set the bounds of win to {200, 120, 780, 480}
  set theViewOptions to the icon view options of win
  set arrangement of theViewOptions to not arranged
  set icon size of theViewOptions to 128
  set position of item "${app_name}" of win to {160, 180}
  set position of item "Applications" of win to {480, 180}
  update without registering applications
  delay 1
  close win
end tell
EOF
      set -e
      if ! _rh_detach "${mount_dir}"; then
        echo "warning: could not detach temporary DMG; falling back to plain UDZO" >&2
        rm -f "${rw_dmg}"
        _rh_udzo
      else
        hdiutil convert "${rw_dmg}" -format UDZO -imagekey zlib-level=9 -o "${dmg}" >/dev/null
        rm -f "${rw_dmg}"
      fi
    else
      echo "warning: failed to mount temporary DMG; falling back to plain UDZO" >&2
      rm -f "${rw_dmg}"
      _rh_udzo
    fi
  else
    _rh_udzo
  fi
  rm -rf "${stage}" "${mount_dir}"
  rm -f "${rw_dmg}"
}
