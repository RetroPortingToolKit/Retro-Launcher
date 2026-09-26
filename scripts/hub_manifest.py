#!/usr/bin/env python3
"""Merge each platform's bare-hub entry into hub-manifest.json.

The manifest is what a tool (n64lle's new-project scaffolder, in "release"
mode) fetches to download a bare retro-hub and run it in Direct mode; see
docs/RELEASES.md for the format. It is published as a release asset, so the
newest one is always at

    https://github.com/<repo>/releases/latest/download/hub-manifest.json

The shape follows Retro-Runtime's runtime-manifest.json (its
scripts/release_manifest.py), plus a `direct_mode` object. Every entry was
written by scripts/package_hub_archive.sh from the hub inside its own archive.
This refuses to write a manifest when entries disagree about the version, the
commit or the contracts, when two serve the same key, or when an expected
platform is missing.

usage: hub_manifest.py --version 0.1.2 --tag v0.1.2 --repo OWNER/NAME \
           --expect linux-x86_64 --out DIR ENTRY.json...
"""

import argparse
import datetime
import json
import pathlib
import sys

SCHEMA = 1

# Platforms a tool may ask for that this release cannot serve, and why. A tool
# reads the reason instead of finding the platform silently absent.
_INSTALLER_ONLY = ("no bare archive is built for this platform yet; the installer and DMG "
                   "assets carry retro-hub with Direct mode")
UNAVAILABLE = {
    "linux-arm64": "not built yet",
    "windows-x86_64": _INSTALLER_ONLY,
    "macos-arm64": _INSTALLER_ONLY,
    "macos-x86_64": _INSTALLER_ONLY,
}

# Fields every platform of one release must agree on.
SHARED = ("version", "commit", "link_protocol", "rcore_abi", "direct_mode")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--expect", required=True, help="comma-separated platforms")
    ap.add_argument("--out", required=True)
    ap.add_argument("entries", nargs="+")
    a = ap.parse_args()

    def refuse(msg: str) -> int:
        print(f"hub_manifest: {msg}", file=sys.stderr)
        return 1

    entries = [json.loads(pathlib.Path(p).read_text()) for p in a.entries]
    by_platform = {}
    for e in entries:
        for key in e.get("serves", [e["platform"]]):
            if key in by_platform:
                return refuse(f"{by_platform[key]['platform']} and {e['platform']} both serve {key}")
            by_platform[key] = e

    expected = {p for p in a.expect.split(",") if p}
    if set(by_platform) != expected:
        return refuse(f"platforms {sorted(by_platform)} but expected {sorted(expected)}")
    if e_ver := [p for p, e in by_platform.items() if e["version"] != a.version]:
        return refuse(f"{e_ver} are not version {a.version}")
    first = entries[0]
    for e in entries[1:]:
        for k in SHARED:
            if e[k] != first[k]:
                return refuse(f"{k}: {first['platform']} has {first[k]!r}, "
                              f"{e['platform']} has {e[k]!r}")

    base = f"https://github.com/{a.repo}/releases/download/{a.tag}"
    platforms = {}
    for name in sorted(by_platform):
        e = by_platform[name]
        platforms[name] = {
            "url": f"{base}/{e['archive']}",
            "archive": e["archive"],
            "sha256": e["sha256"],
            "size": e["size"],
            "executable": e["executable"],
            "files": e["files"],
            "requires": e["requires"],
            "system_libraries": e["system_libraries"],
        }
    for name, why in UNAVAILABLE.items():
        if name not in platforms:
            platforms[name] = {"unavailable": why}

    manifest = {
        "schema": SCHEMA,
        "name": "retro-hub",
        "version": a.version,
        "tag": a.tag,
        "commit": first["commit"],
        "published_utc": datetime.datetime.now(datetime.timezone.utc)
                                  .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "link_protocol": first["link_protocol"],
        "rcore_abi": first["rcore_abi"],
        "direct_mode": first["direct_mode"],
        "platforms": platforms,
    }

    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "hub-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    sums = "".join(f"{e['sha256']}  {e['archive']}\n"
                   for e in sorted(entries, key=lambda e: e["archive"]))
    (out / "SHA256SUMS").write_text(sums)
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
