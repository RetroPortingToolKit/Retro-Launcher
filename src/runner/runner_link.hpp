#pragma once

// The runner's LINK mode: the hub spawned it with the control socket at fd
// kSocketFd and the shared region at kSharedFd (src/corelink/link_protocol.hpp).

#include "core_library.hpp"
#include "core_manifest.hpp"
#include "host_session.hpp"

#include <map>
#include <optional>
#include <string>

namespace retcomm::runner {

struct LinkArgs {
    std::string rom;
    std::string title_dir = ".";
    fs::path out;            // session dir: core.log, events.tsv, the core's cache_dir
    bool gl = false;
    bool strict = false;
    std::map<std::string, std::string> overrides;
    std::optional<fs::path> load_state;
    std::string tpak_rom;
};

// Runs the session until the hub sends Quit or goes away. Returns the
// process exit code (the same table as headless mode).
int run_link_mode(const LoadedCore& core, const CoreManifest& manifest, const LinkArgs& args,
                  void (*lend_gl)(HostSession&));

} // namespace retcomm::runner
