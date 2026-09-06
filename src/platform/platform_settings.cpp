#include "retcomm/platform_settings.hpp"
#include "retcomm/psx_platform_settings.hpp"
#include "retcomm/snes_platform_settings.hpp"

namespace retcomm {

bool platform_has_config_section(const std::string& platform) {
    return is_psx_platform(platform) || is_snes_platform(platform);
}

const char* platform_config_button_label(const std::string& platform) {
    if (is_psx_platform(platform)) return "PSXrecomp Config";
    if (is_snes_platform(platform)) return "SNESrecomp Config";
    return "";
}

ApplyPlatformResult apply_platform_defaults(const Paths& paths, const AppState& state,
                                            const Title& title,
                                            const std::filesystem::path& game_cwd) {
    ApplyPlatformResult r;
    if (is_psx_platform(title.platform)) {
        const auto a = apply_psx_platform_defaults(paths, state, title, game_cwd);
        r.ok = a.ok;
        r.skipped = a.skipped;
        r.message = a.message;
        return r;
    }
    if (is_snes_platform(title.platform)) {
        const auto a = apply_snes_platform_defaults(paths, state, title, game_cwd);
        r.ok = a.ok;
        r.skipped = a.skipped;
        r.message = a.message;
        return r;
    }
    r.skipped = true;
    return r;
}

} // namespace retcomm
