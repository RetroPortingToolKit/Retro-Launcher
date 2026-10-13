#!/usr/bin/env python3
"""Package the bare (non-installer) retro-hub archive for Windows or macOS.

The Linux archive is scripts/package_hub_archive.sh (ELF-specific gates). This is
the same contract (docs/RELEASES.md, "The bare hub archive") for the other two
OSes, from the FLAT HUB PREFIX scripts/build-local.sh / build-local.ps1 write --
the layout n64lle's tools and the title-app kit already run from -- built with
`--release-version <v>` so the hub reports this release:

    scripts/build-local.sh   --release-version 0.9.0 --out out/flat      # macOS
    scripts/build-local.ps1  -ReleaseVersion 0.9.0 -Out out\\flat         # Windows
    scripts/package_bare_hub.py --version 0.9.0 --platform macos-arm64 \\
        --prefix out/flat --out dist-hub

Output in --out, named as the Linux job's:
    retro-hub-<version>-<platform>.zip       (windows-*)
    retro-hub-<version>-<platform>.tar.gz   (macos-*)
    <archive>.sha256
    retro-hub-<version>-<platform>.json     this build's manifest entry, merged by
                                            scripts/hub_manifest.py

Gates, each failing the package rather than warning:
  * the prefix holds the hub, its title-app kit, the fonts and the art, and (on
    Windows) retcomm-portable.exe, which the kit needs;
  * the archive never holds retro-core-runner (a tool takes that from
    Retro-Runtime's release), no symlink, and nothing outside the prefix;
  * the hub INSIDE the archive, extracted to a clean directory, runs --version
    and reports this release's version (and commit when given), Direct mode, the
    runner lookup the contract names, a title-app mode and an Update page, and
    refuses --run-core without --rom with exit 2.

UNVERIFIED ON WINDOWS AND macOS: written 2026-10-13 on Linux, where its
packaging, gates and manifest entry were run against a stand-in prefix. The
first release run on each OS is its first real test.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import zipfile

EXPECT_RUNNER_LOOKUP = "RETRO_CORE_RUNNER exe_dir/retro-core-runner data_dir/runtime/<version>/retro-core-runner"
PLATFORMS = {
    "windows-x86_64": ("zip", ".exe"),
    "macos-arm64": ("tar.gz", ""),
    "macos-x86_64": ("tar.gz", ""),
}
# Never in the archive, wherever they sit in the prefix.
EXCLUDE_NAMES = {"retro-core-runner", "retro-core-runner.exe"}


def die(msg):
    sys.exit("package_bare_hub: " + msg)


def stage(prefix, exe_suffix):
    """The allowlist: every regular file of the flat prefix but the runner."""
    hub = prefix / ("retro-hub" + exe_suffix)
    if not hub.is_file():
        die("%s: not in the prefix (build it with build-local --release-version)" % hub)
    files = []
    for root, dirs, names in os.walk(prefix):
        dirs.sort()
        for n in sorted(names):
            p = pathlib.Path(root) / n
            rel = p.relative_to(prefix).as_posix()
            if p.is_symlink():
                die("%s is a symlink; the archive holds real files only" % rel)
            if n in EXCLUDE_NAMES:
                continue
            files.append(rel)
    have = set(files)
    for need in ("retcomm.png", "fonts/LatoLatin-Regular.ttf", "fonts/NOTICE.md", "LICENSE"):
        if need not in have:
            die("the prefix has no %s" % need)
    if not any(f.startswith("platforms/") for f in files):
        die("platform art missing (platforms/)")
    kit = "packaging/title/build-title-app." + ("ps1" if exe_suffix else "sh")
    if kit not in have:
        die("the title-app kit is missing (%s)" % kit)
    if exe_suffix and ("retcomm-portable.exe" not in have or not any(f.lower().endswith(".dll") for f in files)):
        die("the Windows prefix needs retcomm-portable.exe (the kit's stub) and the hub's DLLs beside the exe")
    return sorted(files)


def write_archive(prefix, files, archive, kind):
    tmp = archive.with_name(archive.name + ".part")
    if kind == "zip":
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for rel in files:
                zi = zipfile.ZipInfo.from_file(prefix / rel, rel)   # forward slashes, mode kept
                zi.compress_type = zipfile.ZIP_DEFLATED
                with open(prefix / rel, "rb") as f:
                    z.writestr(zi, f.read())
    else:
        with tarfile.open(tmp, "w:gz") as t:
            for rel in files:
                ti = t.gettarinfo(str(prefix / rel), arcname=rel)
                ti.uid = ti.gid = 0
                ti.uname = ti.gname = ""
                with open(prefix / rel, "rb") as f:
                    t.addfile(ti, f)
    os.replace(tmp, archive)


def extract(archive, kind, dest):
    if kind == "zip":
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                z.extract(info, dest)
    else:
        with tarfile.open(archive) as t:
            for m in t.getmembers():
                if not m.isfile() and not m.isdir():
                    die("%s: member %s is not a regular file" % (archive.name, m.name))
            t.extractall(dest)
    for p in pathlib.Path(dest).rglob("*"):       # a zip drops the mode bits; the hub must run
        if p.is_file() and p.name in ("retro-hub", "retro-hub.exe"):
            p.chmod(p.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def run_hub(hub, *args):
    env = {k: v for k, v in os.environ.items() if k not in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH")}
    r = subprocess.run([str(hub), *args], capture_output=True, text=True, env=env,
                       cwd=str(hub.parent), timeout=120)
    return r.returncode, r.stdout, r.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--version", required=True)
    ap.add_argument("--platform", required=True, choices=sorted(PLATFORMS))
    ap.add_argument("--prefix", required=True, help="the flat hub prefix (build-local's --out)")
    ap.add_argument("--commit", help="the commit the hub must report (default: not checked)")
    ap.add_argument("--out", default="dist-hub")
    a = ap.parse_args()
    if not re.match(r"^[0-9]+\.[0-9]+\.[0-9]+([.+-][A-Za-z0-9.+-]*)?$", a.version):
        die("--version %s: expected semver without a leading v" % a.version)
    kind, exe_suffix = PLATFORMS[a.platform]
    prefix = pathlib.Path(a.prefix).resolve()
    if not prefix.is_dir():
        die("--prefix %s: not a directory" % prefix)
    out = pathlib.Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    name = "retro-hub-%s-%s" % (a.version, a.platform)
    archive = out / ("%s.%s" % (name, kind))
    files = stage(prefix, exe_suffix)
    write_archive(prefix, files, archive, kind)

    # ---- read the claims back out of the archive, in a clean directory ----
    with tempfile.TemporaryDirectory() as clean:
        clean = pathlib.Path(clean)
        extract(archive, kind, clean)
        got = sorted(p.relative_to(clean).as_posix() for p in clean.rglob("*") if p.is_file())
        if got != files:
            die("the extracted archive differs from the allowlist")
        if any(n in EXCLUDE_NAMES for n in (p.name for p in clean.rglob("*"))):
            die("retro-core-runner must not be in the hub archive (it comes from Retro-Runtime's release)")
        hub = clean / ("retro-hub" + exe_suffix)
        rc, out_text, err = run_hub(hub, "--version")
        if rc != 0:
            die("the archived hub failed --version (exit %d): %s" % (rc, err.strip()))
        fields = {}
        for line in out_text.splitlines():
            k, _, v = line.partition(" ")
            fields[k] = v.strip()
        if fields.get("version") != a.version:
            die("the archived hub says version %r, this release is %s" % (fields.get("version"), a.version))
        if a.commit and fields.get("commit") != a.commit:
            die("the archived hub says commit %r, this release is %s" % (fields.get("commit"), a.commit))
        if not re.match(r"^[1-9][0-9]*$", fields.get("direct_mode", "")):
            die("the archived hub was built without Direct mode")
        if fields.get("runner_lookup") != EXPECT_RUNNER_LOOKUP:
            die("the archived hub looks for the runner at %r" % fields.get("runner_lookup"))
        if not re.match(r"^[1-9][0-9]*$", fields.get("title_app", "")):
            die("the archived hub has no title-app mode")
        if not re.match(r"^[1-9][0-9]*$", fields.get("updates", "")):
            die("the archived hub has no Update page")
        rc, _, err = run_hub(hub, "--run-core", str(clean / "none"))
        if rc != 2 or "--run-core needs --rom" not in err:
            die("the archived hub did not refuse --run-core without --rom (exit %d: %s)" % (rc, err.strip()))
        if exe_suffix == "":
            r = subprocess.run(["bash", str(clean / "packaging/title/build-title-app.sh"), "--help"],
                               capture_output=True, text=True)
            if r.returncode != 0:
                die("the archived title-app kit does not run: " + r.stderr.strip())

    sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    pathlib.Path(str(archive) + ".sha256").write_text("%s  %s\n" % (sha, archive.name))
    link = fields["link_protocol"].split(".")
    entry = {
        "platform": a.platform,
        "serves": [a.platform],
        "version": a.version,
        "commit": fields["commit"],
        "archive": archive.name,
        "sha256": sha,
        "size": archive.stat().st_size,
        "executable": "retro-hub" + exe_suffix,
        "files": files,
        # No glibc floor off Linux; the OS's own libraries are not listed.
        "requires": {},
        "system_libraries": [],
        "link_protocol": {"major": int(link[0]), "minor": int(link[1])},
        "rcore_abi": {"major": int(fields["rcore_abi_major"]),
                      "draft_revision": int(fields["rcore_draft_revision"])},
        "direct_mode": {"cli_revision": int(fields["direct_mode"]),
                        "updates": int(fields["updates"]),
                        "flags": fields["direct_mode_flags"].split(),
                        "runner_lookup": fields["runner_lookup"]},
    }
    (out / (name + ".json")).write_text(json.dumps(entry, indent=2) + "\n")
    print("package_bare_hub: %s" % archive)
    print("  sha256 %s, %d bytes, link %s" % (sha, entry["size"], fields["link_protocol"]))


if __name__ == "__main__":
    main()
