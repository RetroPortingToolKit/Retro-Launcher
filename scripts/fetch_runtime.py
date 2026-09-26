#!/usr/bin/env python3
"""Prebake the published retro-core-runner into a Retro Launcher install prefix.

Release packaging runs this after `cmake --install`: it replaces the runner
CMake built from the Retro-Runtime submodule with the newest one Retro-Runtime
has published for the platform, so every launcher ships the runner that
players' updaters will also see (Retro-Runtime docs/RELEASES.md).

It trusts nothing it has not checked -- the same rule the launcher's own
updater follows (src/update/runtime_update.cpp):

  * the manifest's link and rcore ABI majors must be the ones THIS launcher
    speaks, read from the submodule's headers -- a mismatch fails the build;
  * the platform must be served, not listed as unavailable;
  * the archive's size and SHA-256 must match before it is opened;
  * the archive holds exactly the manifest's file list, nothing outside it;
  * the extracted runner must itself report the manifest's version and commit.

It writes <prefix>/bin/retro-core-runner(.exe), the runtime's licenses under
<prefix>/share/licenses/retro-runtime/, and a record of what was baked in
<prefix>/share/retcomm/runtime-baked.json.

usage: fetch_runtime.py --platform linux-x86_64 --prefix out
       [--manifest URL]   default: the newest published release, prereleases
                          included; RETRO_RUNTIME_MANIFEST_URL also sets it
"""

import argparse
import hashlib
import io
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

REPO = "RetroPortingToolKit/Retro-Runtime"
SCHEMA = 1
ROOT = pathlib.Path(__file__).resolve().parent.parent
RUNTIME_SRC = ROOT / "third_party" / "Retro-Runtime"


def die(msg: str) -> None:
    print(f"fetch_runtime: {msg}", file=sys.stderr)
    sys.exit(1)


def get(url: str, accept: str = "") -> bytes:
    headers = {"User-Agent": "retro-launcher-release"}
    if accept:
        headers["Accept"] = accept
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        headers["Authorization"] = f"Bearer {token}"
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=120) as r:
        return r.read()


def newest_manifest_url() -> str:
    # The newest non-draft release, prereleases included: "latest" here means
    # the newest runner published, which releases/latest (stable only) is not.
    releases = json.loads(get(f"https://api.github.com/repos/{REPO}/releases?per_page=20",
                              "application/vnd.github+json"))
    for rel in releases:
        if rel.get("draft"):
            continue
        for a in rel.get("assets", []):
            if a["name"] == "runtime-manifest.json":
                print(f"fetch_runtime: newest release {rel['tag_name']}"
                      f"{' (prerelease)' if rel.get('prerelease') else ''}")
                return a["browser_download_url"]
    die(f"no release of {REPO} carries a runtime-manifest.json")


def header_number(path: pathlib.Path, pattern: str) -> int:
    m = re.search(pattern, path.read_text())
    if not m:
        die(f"cannot read {pattern!r} from {path}")
    return int(m.group(1))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", required=True)
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--manifest", default=os.environ.get("RETRO_RUNTIME_MANIFEST_URL", ""))
    a = ap.parse_args()
    prefix = pathlib.Path(a.prefix).resolve()

    # What this launcher speaks: the submodule it was built against.
    link_major = header_number(RUNTIME_SRC / "corelink" / "link_protocol.hpp",
                               r"kProtocolMajor\s*=\s*(\d+)")
    abi_major = header_number(RUNTIME_SRC / "include" / "rcore" / "rcore.h",
                              r"#define\s+RCORE_ABI_MAJOR\s+(\d+)")

    url = a.manifest or newest_manifest_url()
    m = json.loads(get(url))
    if m.get("schema") != SCHEMA:
        die(f"manifest schema {m.get('schema')}, this script reads {SCHEMA}")
    version, commit = m["version"], m["commit"]
    if m["link_protocol"]["major"] != link_major or m["rcore_abi"]["major"] != abi_major:
        die(f"runner {version} speaks link {m['link_protocol']['major']}, ABI "
            f"{m['rcore_abi']['major']}; this launcher speaks link {link_major}, ABI {abi_major}. "
            "Bump the Retro-Runtime submodule, or pin an older manifest.")
    e = m["platforms"].get(a.platform)
    if e is None:
        die(f"the manifest has no entry for {a.platform}")
    if "unavailable" in e:
        die(f"{a.platform} is not available in runner {version}: {e['unavailable']}")

    blob = get(e["url"])
    sha = hashlib.sha256(blob).hexdigest()
    if len(blob) != e["size"] or sha != e["sha256"].lower():
        die(f"{e['archive']}: {len(blob)} bytes sha256 {sha}; the manifest says "
            f"{e['size']} bytes sha256 {e['sha256']}")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        want = sorted(e["files"])
        if e["archive"].endswith(".zip"):
            with zipfile.ZipFile(io.BytesIO(blob)) as z:
                names = sorted(n for n in z.namelist() if not n.endswith("/"))
                if names != want:
                    die(f"archive holds {names}, the manifest lists {want}")
                z.extractall(tmp)
        else:
            with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as t:
                members = [x for x in t.getmembers() if x.isfile()]
                names = sorted(x.name for x in members)
                if names != want:
                    die(f"archive holds {names}, the manifest lists {want}")
                for x in members:
                    target = tmp / x.name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(t.extractfile(x).read())
                    target.chmod(0o755 if x.mode & 0o111 else 0o644)

        runner = tmp / e["executable"]
        report = subprocess.run([str(runner), "--version"], capture_output=True, text=True,
                                timeout=60)
        fields = dict(line.split(" ", 1) for line in report.stdout.splitlines() if " " in line)
        if report.returncode != 0 or fields.get("version") != version or \
                fields.get("commit") != commit:
            die(f"the downloaded runner reports version {fields.get('version')!r} commit "
                f"{fields.get('commit')!r} (exit {report.returncode}); the manifest says "
                f"{version} {commit}")

        bin_dir = prefix / "bin"
        bin_dir.mkdir(parents=True, exist_ok=True)
        dest = bin_dir / e["executable"]
        shutil.copy2(runner, dest)
        dest.chmod(0o755)
        lic_dir = prefix / "share" / "licenses" / "retro-runtime"
        if lic_dir.exists():
            shutil.rmtree(lic_dir)
        lic_dir.mkdir(parents=True)
        for f in e["files"]:
            if f != e["executable"]:
                (lic_dir / pathlib.Path(f).name).write_bytes((tmp / f).read_bytes())

    record = prefix / "share" / "retcomm" / "runtime-baked.json"
    record.parent.mkdir(parents=True, exist_ok=True)
    record.write_text(json.dumps({
        "platform": a.platform, "version": version, "commit": commit,
        "archive": e["archive"], "sha256": sha, "manifest": url,
    }, indent=2) + "\n")
    print(f"fetch_runtime: baked retro-core-runner {version} ({commit[:10]}) for {a.platform} "
          f"into {dest}")


if __name__ == "__main__":
    main()
