#!/usr/bin/env python3
"""Record exact source commits in the APK, including dirty local checkouts."""
import json
from pathlib import Path
import subprocess
import sys


def source(path):
    def git(*args):
        return subprocess.check_output(["git", "-C", path, *args], text=True).strip()
    return {"commit": git("rev-parse", "HEAD"),
            "dirty": bool(git("status", "--porcelain", "--untracked-files=no"))}


if __name__ == "__main__":
    output, launcher, runtime, sdl, version, abi = sys.argv[1:]
    Path(output).write_text(json.dumps({
        "schema": 1, "version": version, "abi": abi,
        "launcher": source(launcher), "runtime": source(runtime), "sdl": source(sdl),
        "curl": "8.21.0", "openssl": "3.6.3",
    }, indent=2) + "\n")
