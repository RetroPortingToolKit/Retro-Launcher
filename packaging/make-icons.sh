#!/usr/bin/env bash
# assets/retcomm.png (the master icon) → assets/retcomm.ico.
# Master PNG is 512x512 — linuxdeploy rejects 1024 (and other non-hicolor sizes).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PNG="${ROOT}/assets/retcomm.png"
ICO="${ROOT}/assets/retcomm.ico"
SIZE=512

# assets/retcomm.png is the master (the mascot, 512x512; linuxdeploy rejects 1024). It is
# not rasterized from retcomm.svg any more -- that was the old mark -- so this only
# rebuilds the .ico from it. Replace the PNG to change the icon.
if [[ ! -f "${PNG}" ]]; then
  echo "missing ${PNG}" >&2
  exit 1
fi

echo "PNG: ${PNG} (${SIZE}x${SIZE})"

if command -v magick >/dev/null 2>&1; then
  magick "${PNG}" -define icon:auto-resize=256,128,64,48,32,16 "${ICO}"
  echo "ICO: ${ICO}"
elif command -v convert >/dev/null 2>&1; then
  convert "${PNG}" -define icon:auto-resize=256,128,64,48,32,16 "${ICO}"
  echo "ICO: ${ICO}"
fi
