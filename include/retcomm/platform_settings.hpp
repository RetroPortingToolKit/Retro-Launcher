#pragma once

// Platform-agnostic entry points over the per-platform Configure sections
// (psx_platform_settings.hpp, snes_platform_settings.hpp). Install, update,
// and launch call these so adding a platform section is one more case here
// rather than a new branch at every call site.

#include "retcomm/app_state.hpp"
#include "retcomm/catalog.hpp"
#include "retcomm/paths.hpp"

#include <filesystem>
#include <string>

namespace retcomm {

// True when the hub has a global Configure page for this catalog platform slug
// (and therefore the per-title "Exclude from platform config" toggle applies).
bool platform_has_config_section(const std::string& platform);

// Short label for the library header button ("PSXrecomp Config", ...).
const char* platform_config_button_label(const std::string& platform);

struct ApplyPlatformResult {
    bool ok = true;
    bool skipped = false;
    std::string message;
};

// Merge that platform's global prefs into a title install cwd. Skipped (not an
// error) for platforms without a section.
ApplyPlatformResult apply_platform_defaults(const Paths& paths, const AppState& state,
                                            const Title& title,
                                            const std::filesystem::path& game_cwd);

} // namespace retcomm
