# Vosk (libvosk) and its English model

Nothing of Vosk is vendored here. `retro-hub` loads **libvosk** at runtime
(`dlopen` / `LoadLibrary`; `src/hub/hub_vru_mic.cpp`) for the VRU Microphone's
speech recognition, and downloads the English model on first use. A hub built
or installed without the library still runs; its VRU panel says where it looked.

- **vosk-api** — <https://github.com/alphacep/vosk-api>. Copyright 2019-2024
  Alpha Cephei Inc. Licensed under the Apache License, Version 2.0.
  The hub declares the handful of `vosk_*` C functions it calls itself
  (`VoskLibrary`, `hub_vru_mic.hpp`); `vosk_api.h` is not copied.
- **Kaldi** — <https://github.com/kaldi-asr/kaldi>, the speech recognition
  toolkit libvosk is built on. Copyright 2009-2020 the Kaldi contributors
  (Johns Hopkins University, Microsoft Corporation, Brno University of
  Technology, Go Vivace Inc., and others named in Kaldi's `COPYING`).
  Licensed under the Apache License, Version 2.0. libvosk also carries
  **OpenFst** (Google; Apache-2.0) and, in the published binaries, OpenBLAS
  (BSD-3-Clause) or Intel MKL.
- **vosk-model-small-en-us-0.15** —
  <https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip>,
  Alpha Cephei Inc., Apache License, Version 2.0. Downloaded by the hub into
  `<data dir>/vru/models/` (about 40 MB); never bundled in a release.

## Where the library comes from

Vosk publishes prebuilt zips per release (`vosk-linux-x86_64-<v>.zip`,
`vosk-osx-<v>.zip`, `vosk-win64-<v>.zip`). Point CMake's `RETCOMM_VOSK_DIR`
at the unpacked directory and the build copies the library beside `retro-hub`
and installs it with it. Without that option the hub looks beside itself, in
`<data dir>/vru/`, and finally through the system loader (a distribution
package). A system or vcpkg package of vosk did not exist when this was written
(2026-10-01; checked `pkg-config vosk` and the vcpkg registry).

## Apache License 2.0 — redistribution

A release that ships `libvosk` must carry the Apache License text and this
NOTICE (Apache-2.0 §4). The hub's own license (MIT, `LICENSE`) is unaffected:
nothing of Vosk is linked into or derived from in the hub's sources.
Obtaining Kaldi's and Vosk's source trees is not needed to build the hub, and
nothing under GPL is involved: vosk-api, Kaldi, OpenFst and the model are all
Apache-2.0.
