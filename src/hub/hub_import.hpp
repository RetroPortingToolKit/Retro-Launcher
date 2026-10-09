#pragma once

// Bringing a picked file into the hub's own storage: a ROM, a save, a BIOS, or
// a whole title bundle.
//
// A "picked file" is what SDL_ShowOpenFileDialog hands back: a path on a
// desktop, and on Android a content:// URI from the Storage Access Framework,
// which std::filesystem cannot open (SDL can, through SDL_IOFromFile), and
// which carries no file name of its own (the provider's display name is asked
// for over JNI).

#include <filesystem>
#include <string>

namespace retcomm::hub {

namespace fs = std::filesystem;

// The name a picked file goes by: its file name, or for a content:// URI the
// provider's display name; "" when it has none. Never a path: no separators.
std::string picked_name(const std::string& pick);

// Copy a picked file to `dest` (overwriting it).
bool copy_picked(const std::string& pick, const fs::path& dest, std::string* error);

// A TITLE BUNDLE is a zip holding a schema 1 title.json (docs/RELEASES.md,
// "Title-app mode") at its root or in one folder, with everything it names
// beside it: the core and its .rcore.toml, the game package and game.toml. It
// is how a title reaches a launcher that cannot run a port's build -- Android
// above all, where the core and the package must be that platform's.
struct InstalledTitle {
    std::string id;
    std::string name;
    fs::path manifest; // <titles_root>/<id>/title.json
};

// Unpack `pick` into <titles_root>/<id>/ (replacing an earlier install of the
// same id), after checking that its title.json reads and that its core is in
// it. `scratch` holds the copy of a content:// pick while it is read.
bool install_title_bundle(const std::string& pick, const fs::path& titles_root,
                          const fs::path& scratch, InstalledTitle* out, std::string* error);

} // namespace retcomm::hub
