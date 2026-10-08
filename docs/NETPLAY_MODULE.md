# Netplay module

The netcode (recomp-net + rbengine) is a separate, versioned library, not part
of the runner or the cores: `librecomp_net_module.so` (recomp-net
`docs/module.md`). The launcher keeps it current the way it keeps the runner.

Code: `include/retcomm/netplay_module.hpp`, `src/update/netplay_module.cpp`.

- **Install location:** `<data>/netplay-module/<version>/`; the newest two are
  kept. A module beside the launcher (bundled) also counts. A running match
  keeps the module it started with.
- **Resolve:** `$RETRO_NETPLAY_MODULE`, else the newest usable bundled or
  updated module whose ABI is `kNetplayModuleAbi` (1). Opening a library to
  read `rnet_module_info` runs no netplay code.
- **Update:** `netplay-module-manifest.json` from the newest recomp-net release
  (`RETRO_NETPLAY_MODULE_MANIFEST_URL` overrides, `file://` too). Refused when
  the platform is missing, the ABI differs, `requires` is unmet, size or SHA-256
  differ, or the extracted library does not report the manifest's version, commit,
  ABI and wire version. Runs on startup and in the Update step beside the
  runtime update.
- **CLI:** `retcomm netplay-module [status|check|update]`.
- **Starting a match:** `start_net_session` sets `RETRO_NETPLAY_MODULE` for the
  runner. A runner built with netplay linked in ignores it; a runner built for
  the module (Retro-Runtime `-DRETRO_RUNTIME_NETPLAY_MODULE=ON`) opens it and
  refuses the match, naming the problem, when there is none.
- **Compatibility between players:** the runner folds the module's wire version
  into the build fingerprint, so two players whose modules cannot talk are
  refused at the handshake. The launcher does not yet show this beforehand; the
  lobby's `game_version` match does not carry the wire version.

Not done: Windows (no `LoadLibrary` probe), a UI line for the module's version,
and the lobby carrying the wire version.
