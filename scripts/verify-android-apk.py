#!/usr/bin/env python3
"""Fail packaging if the APK omits/mis-targets its native runtime or resources."""
import json
import struct
import sys
import zipfile


def verify(apk, abi):
    machine = {"arm64-v8a": 183, "x86_64": 62}[abi]
    with zipfile.ZipFile(apk) as archive:
        for name in ("libmain.so", "libSDL3.so", "libc++_shared.so", "libretro-core-runner.so"):
            data = archive.read(f"lib/{abi}/{name}")
            if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from('<H', data, 18)[0] != machine:
                raise ValueError(f"{name} is not a 64-bit {abi} ELF")
            if struct.unpack_from('<H', data, 16)[0] != 3:
                raise ValueError(f"{name} is not position independent")
            if name == "libretro-core-runner.so":
                # A PIE child must retain PT_INTERP. A regular shared library
                # with this filename would package successfully but cannot run.
                offset = struct.unpack_from('<Q', data, 32)[0]
                size, count = struct.unpack_from('<HH', data, 54)
                types = [struct.unpack_from('<I', data, offset + i * size)[0] for i in range(count)]
                if 3 not in types:
                    raise ValueError('Bundled runner is not an executable (PT_INTERP missing)')
        for name in ('fonts/LatoLatin-Regular.ttf', 'fonts/LatoLatin-Bold.ttf',
                     'licenses/Retro-Runtime.txt'):
            if not archive.read('assets/retcomm/' + name):
                raise ValueError(f'Empty asset: {name}')
        manifest = json.loads(archive.read(f'assets/retcomm/build-{abi}.json'))
        if manifest['abi'] != abi or len(manifest['runtime']['commit']) != 40:
            raise ValueError('Invalid runtime build provenance')
        print(f"Verified {abi} APK; Retro-Runtime {manifest['runtime']['commit']}")


if __name__ == '__main__':
    verify(*sys.argv[1:])
