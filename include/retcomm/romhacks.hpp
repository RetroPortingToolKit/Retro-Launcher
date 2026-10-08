// Romhack BASES beside an n64lle game package (n64lle docs/ROMHACK-PLAN.md R5).
//
// A base is the stock game or exactly one romhack. The n64lle generic core
// loads one when its option `system.romhack` names it: an installed romhack is
// <title_dir>/romhacks/<id>/ holding base.toml, the patch and delta.<ext>
// (written by n64lle's tools/n64romhack.py install). The hub's part:
//
//   - list what is installed, for the Base picker (scan_romhacks);
//   - keep a base's saves apart from the stock game's (base_save_dir);
//   - say where a mod stands on the chosen base (mod_base_standing).
//
// Windowless, so it is tested without a GPU (tests/hub_romhack_test.cpp).
#pragma once

#include "retcomm/mods.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace retcomm {
namespace fs = std::filesystem;

// The core option a base is selected by. Empty: the stock game.
inline constexpr const char* kRomhackOption = "system.romhack";

struct InstalledRomhack {
    std::string id;       // = its directory name under romhacks/
    std::string version;
    std::string title;    // falls back to id
    // base.toml [base] saves: "separate" (the default) or "shared-with-stock",
    // the hack author's statement that the save layout is unchanged.
    bool saves_shared = false;
    fs::path dir;
    std::string error;    // non-empty: listed, but the core would refuse it
};

// Every directory under <title_dir>/romhacks/ with a base.toml, sorted by title.
// One whose base.toml is unreadable, whose id is not its directory's name, or
// whose delta is missing is listed with `error` set, so the picker can say why
// instead of hiding it.
std::vector<InstalledRomhack> scan_romhacks(const fs::path& title_dir);

// The installed romhack with this id, or null.
const InstalledRomhack* find_romhack(const std::vector<InstalledRomhack>& list,
                                     const std::string& id);

// Where a title's save files live on a base: <data_dir>/saves/<title_key> for
// the stock game (and for a hack whose saves are shared with it), else
// <data_dir>/saves/<title_key>@<id>. A hack changes the save layout as often
// as not, so a stock save fed to it -- or the reverse -- is a corruption path.
fs::path base_save_dir(const fs::path& data_dir, const std::string& title_key,
                       const std::string& base_id, bool saves_shared);

// A mod package on a base (n64lle docs/MODDING.md §5.8): declared when its
// manifest's [target].bases names the base ("<id>" or "<id>@<version>"),
// unverified otherwise. The core still refuses a mechanical conflict at load.
enum class ModBaseStanding { Stock, Declared, Unverified };
ModBaseStanding mod_base_standing(const ModPackageInfo& pkg, const InstalledRomhack* base);

}  // namespace retcomm
