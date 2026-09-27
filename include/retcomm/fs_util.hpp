#pragma once

// Filesystem operations that have to survive a library on a network share
// (Retro-Launcher issue #6). On SMB a rename that works on a local disk can
// fail for a few moments: the SMB client, an antivirus scanner or the search
// indexer still holds a file that was just written or run, and Windows
// answers "Access is denied" or a sharing violation. And a staging folder on
// one drive cannot be renamed onto another.

#include <filesystem>
#include <string>
#include <system_error>

namespace retcomm {

namespace fs = std::filesystem;

// fs::rename, retried for a few seconds while the failure looks transient,
// and a copy then removal of the source when `from` and `to` are on
// different devices. `ec` is the last error when it returns false; errors
// that retrying cannot fix (no such file, the destination exists) return at
// once. A copy that fails leaves nothing at `to`.
bool robust_rename(const fs::path& from, const fs::path& to, std::error_code& ec);

#if defined(_WIN32)
// A working directory for a cmd.exe child (mklink /J): the Windows system
// folder, which is always local. cmd.exe cannot start in a UNC directory, and
// a child inherits the hub's, which is on the share when the portable launcher
// lives there.
std::wstring local_cmd_working_dir();
#endif

} // namespace retcomm
