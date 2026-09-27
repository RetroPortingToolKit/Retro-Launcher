#include "retcomm/fs_util.hpp"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace retcomm {

namespace {

// A copy, then removal of the source. A failed copy is undone.
bool copy_then_remove(const fs::path& from, const fs::path& to, std::error_code& ec) {
    ec.clear();
    const bool dir = fs::is_directory(from, ec);
    if (ec) return false;
    if (dir) {
        if (fs::exists(to, ec)) {
            ec = std::make_error_code(std::errc::file_exists);
            return false;
        }
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    } else {
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
        std::error_code undo;
        if (dir) fs::remove_all(to, undo);
        return false;
    }
    // The move is done once the copy is; a source that will not go (held open
    // on the share) is left behind rather than failing the caller.
    std::error_code rm;
    fs::remove_all(from, rm);
    return true;
}

bool retrying_cannot_help(const std::error_code& ec) {
    const std::error_condition c = ec.default_error_condition();
    return c == std::errc::no_such_file_or_directory || c == std::errc::file_exists ||
           c == std::errc::directory_not_empty || c == std::errc::invalid_argument ||
           c == std::errc::is_a_directory || c == std::errc::not_a_directory ||
           c == std::errc::filename_too_long || c == std::errc::read_only_file_system ||
           c == std::errc::no_space_on_device;
}

} // namespace

bool robust_rename(const fs::path& from, const fs::path& to, std::error_code& ec) {
    constexpr int kAttempts = 12;
    constexpr auto kPause = std::chrono::milliseconds(250); // ~3 s in all
    for (int attempt = 1;; ++attempt) {
        ec.clear();
        fs::rename(from, to, ec);
        if (!ec) return true;
        if (ec.default_error_condition() == std::errc::cross_device_link)
            return copy_then_remove(from, to, ec);
        if (retrying_cannot_help(ec) || attempt >= kAttempts) return false;
        std::this_thread::sleep_for(kPause);
    }
}

#if defined(_WIN32)
std::wstring local_cmd_working_dir() {
    wchar_t buf[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L"C:\\Windows\\System32");
}
#endif

} // namespace retcomm
