// RetComM Windows portable stub — single .exe with appended zip payload.
//
// Trailer layout (little-endian):
//   [PE stub bytes][zip payload][uint64 payload_size][magic "RCM1"]
//
// No args  → launch retcomm-hub.exe from the extracted runtime
// cli …    → launch retcomm.exe with the remaining arguments
//
// Portable layout — everything lives beside the .exe:
//   <exe_dir>\RetComM-Data\runtime\   extracted hub + CLI
//   <exe_dir>\RetComM-Data\config\    config.json
//   <exe_dir>\RetComM-Data\data\      apps, toolchains, engines, catalog, …
// RETCOMM_HOME is exported to the child so it resolves the same folder.
//
// When setup moves the RetComM folder elsewhere, the hub records the new root
// in <exe_dir>\retcomm-root.json. We then unpack into <root>\runtime\ instead
// and delete the old RetComM-Data, so the launcher ends up as a lone .exe with
// one folder holding the runtime, config and data together.
//
// When the exe folder is not writable (Downloads with MotW, a network share, a
// read-only stick) we fall back to %LOCALAPPDATA%\retcomm\portable\current\ for
// the runtime and leave RETCOMM_HOME unset, so config+data stay at the historical
// %LOCALAPPDATA%\retcomm and an existing portable user's library still resolves.
// The appended zip is unpacked in-process (miniz); no helper process is run.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "portable_trailer.hpp"
#include "retcomm/zip_extract.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(RETCOMM_VERSION)
#define RETCOMM_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;

namespace {

// Trailer magic / layout: portable_trailer.hpp (shared with the hub's self-update).

void fail(const std::wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), L"RetComM Launcher", MB_OK | MB_ICONERROR);
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr,
                                      nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

fs::path exe_path() {
    // Grow past MAX_PATH so deep install directories still resolve.
    DWORD cap = MAX_PATH;
    std::wstring buf(cap, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), cap);
        if (n == 0) return {};
        if (n < cap) {
            buf.resize(n);
            return fs::path(buf);
        }
        if (cap >= 32768) return {};
        cap *= 2;
        buf.assign(cap, L'\0');
    }
}

// Can we write inside `dir`? Probes an EXISTING directory only — it never
// creates one. The data folder must not appear beside the .exe before the
// setup wizard has asked where the user wants it; creating it here made the
// wizard offer a folder that already existed.
bool dir_is_writable(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    const fs::path probe = dir / L".retcomm-write-test";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "ok";
        if (!out) return false;
    }
    fs::remove(probe, ec);
    return true;
}

bool same_path(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;
    const std::wstring aw = a.lexically_normal().wstring();
    const std::wstring bw = b.lexically_normal().wstring();
    return _wcsicmp(aw.c_str(), bw.c_str()) == 0;
}

// Pull the "root" string out of retcomm-root.json. The stub links nothing but
// shell32 on purpose, so this reads the single field it needs rather than
// pulling in a JSON library — the mirror of read_data_root_marker() in
// data_root.cpp, including resolving a relative root against the marker's own
// directory so a moved stick still works.
fs::path read_root_marker(const fs::path& marker) {
    std::error_code ec;
    if (!fs::is_regular_file(marker, ec)) return {};
    std::ifstream in(marker, std::ios::binary);
    if (!in) return {};
    const std::string text((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());

    const std::string key = "\"root\"";
    const size_t k = text.find(key);
    if (k == std::string::npos) return {};
    size_t p = text.find(':', k + key.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' ||
                               text[p] == '\n'))
        ++p;
    if (p >= text.size() || text[p] != '"') return {};
    ++p;

    std::string raw;
    for (; p < text.size() && text[p] != '"'; ++p) {
        if (text[p] == '\\' && p + 1 < text.size()) ++p; // \\ \" \/ → literal
        raw.push_back(text[p]);
    }
    if (raw.empty()) return {};

    fs::path root(std::wstring(raw.begin(), raw.end()));
    if (root.is_relative()) {
        fs::path joined = fs::absolute(marker.parent_path() / root, ec);
        root = ec ? (marker.parent_path() / root) : joined;
    }
    return root.lexically_normal();
}

// True when `dir` holds nothing but a previously unpacked runtime. Anything
// else — config/, data/, a file the user dropped in — means it is not ours to
// delete, and we leave the whole folder alone.
bool holds_only_runtime(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (_wcsicmp(name.c_str(), L"runtime") == 0) continue;
        if (_wcsicmp(name.c_str(), L"version.txt") == 0) continue;
        return false;
    }
    return !ec;
}

fs::path local_app_data() {
    wchar_t* raw = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&raw, &len, L"LOCALAPPDATA") == 0 && raw && len > 1) {
        fs::path p(raw);
        free(raw);
        return p;
    }
    if (raw) free(raw);
    return {};
}

bool read_trailer(const fs::path& self, uint64_t* payload_size, uint64_t* payload_offset) {
    std::ifstream in(self, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto file_size = static_cast<uint64_t>(in.tellg());
    // The trailer is at the end of the file, or -- once the release is
    // Authenticode-signed -- just before the appended certificate table.
    return retcomm_portable::find_payload(in, file_size, payload_size, payload_offset);
}

// The shared zip reader reports errors as UTF-8; the stub shows them in a
// MessageBox, so decode them properly rather than assuming ASCII.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

// Unpack the appended payload into `dest`, read straight out of this .exe.
//
// This used to write payload.zip beside the runtime and then shell out to
// System32\tar.exe, falling back to
//   powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive ..."
// Dropping an archive to disk and spawning a system interpreter to unpack it is
// the shape Defender's ML heuristics score as a dropper, and it was getting
// released builds quarantined. Nothing is written now but the extracted files,
// and no child process runs before the hub itself launches.
//
// The reader -- and the zip-slip guard on entry names -- is shared with
// retcomm_core, which unpacks downloaded archives the same way. The stub reads
// through a stream at the payload offset, so the zip is never copied anywhere
// first. See include/retcomm/zip_extract.hpp.
bool extract_payload(const fs::path& self, uint64_t offset, uint64_t size, const fs::path& dest,
                     std::wstring* err) {
    std::error_code ec;
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    if (ec) {
        *err = L"Cannot create extract directory.";
        return false;
    }

    std::ifstream in(self, std::ios::binary);
    if (!in) {
        *err = L"Cannot read portable executable.";
        return false;
    }

    std::string zip_err;
    if (retcomm::zip::extract_stream(in, offset, size, dest, &zip_err)) return true;
    *err = L"Could not unpack the portable payload: " + widen(zip_err);
    return false;
}

std::string json_escape(std::string s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

void write_channel(const fs::path& dir, const fs::path& portable_exe,
                   const fs::path& data_root) {
    std::ofstream out(dir / "channel.json", std::ios::trunc);
    if (!out) return;
    out << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"channel\": \"portable\",\n"
        << "  \"portable_exe\": \"" << json_escape(narrow(portable_exe.wstring())) << "\",\n"
        << "  \"data_root\": \"" << json_escape(narrow(data_root.wstring())) << "\"\n"
        << "}\n";
}

bool launch(const fs::path& binary, const std::wstring& args, const fs::path& portable_exe,
            const fs::path& data_root, bool attach_console, std::wstring* err) {
    std::wstring cmd = L"\"" + binary.wstring() + L"\"";
    if (!args.empty()) cmd += L" " + args;

    std::wstring channel = L"portable";
    SetEnvironmentVariableW(L"RETCOMM_INSTALL_CHANNEL", channel.c_str());
    SetEnvironmentVariableW(L"RETCOMM_PORTABLE_EXE", portable_exe.wstring().c_str());
    // Hub and CLI resolve config+data under this root (see data_root.cpp).
    if (!data_root.empty())
        SetEnvironmentVariableW(L"RETCOMM_HOME", data_root.wstring().c_str());

    if (attach_console) AttachConsole(ATTACH_PARENT_PROCESS);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    // Hub is GUI (/SUBSYSTEM:WINDOWS); still hide any console for older builds.
    // CLI attaches to the parent console and waits.
    const DWORD flags = attach_console ? 0 : (CREATE_NO_WINDOW | DETACHED_PROCESS);
    const BOOL ok =
        CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, attach_console ? TRUE : FALSE,
                       flags, nullptr, binary.parent_path().wstring().c_str(), &si, &pi);
    if (!ok) {
        *err = L"Failed to launch " + binary.filename().wstring();
        return false;
    }
    if (attach_console) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::wstring join_args(int start, int argc, wchar_t** argv) {
    std::wstring out;
    for (int i = start; i < argc; ++i) {
        if (i > start) out += L' ';
        const std::wstring a = argv[i];
        if (a.find(L' ') != std::wstring::npos) out += L'"' + a + L'"';
        else out += a;
    }
    return out;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        fail(L"Failed to parse command line.");
        return 1;
    }

    const fs::path self = exe_path();
    if (self.empty()) {
        LocalFree(argv);
        fail(L"Cannot resolve portable executable path.");
        return 1;
    }

    uint64_t payload_size = 0, payload_offset = 0;
    if (!read_trailer(self, &payload_size, &payload_offset)) {
        LocalFree(argv);
        fail(L"This file is not a valid RetComM portable package (missing payload).");
        return 1;
    }

    // Prefer a self-contained folder beside the .exe; fall back to the historical
    // LOCALAPPDATA cache when this medium is read-only.
    // Probe the exe's own directory — it exists already, so this answers "can
    // we put a data folder here" without creating one.
    const fs::path default_base = self.parent_path() / L"RetComM-Data";
    fs::path base = default_base;
    bool portable_base = dir_is_writable(self.parent_path());

    // Setup may have moved the RetComM folder elsewhere; retcomm-root.json
    // beside this .exe records where. Unpack the runtime into that folder too,
    // so the whole install stays one directory and nothing is stranded next to
    // the launcher. Missing drive or unwritable target → fall back to the
    // default beside the .exe rather than refusing to start.
    const fs::path marked = read_root_marker(self.parent_path() / L"retcomm-root.json");
    if (!marked.empty() && dir_is_writable(marked)) {
        base = marked;
        portable_base = true;
    }

    if (!portable_base) {
        const fs::path fallback = local_app_data();
        if (fallback.empty()) {
            LocalFree(argv);
            fail(L"Cannot write beside the executable, and %LOCALAPPDATA% is unset.");
            return 1;
        }
        base = fallback / L"retcomm" / L"portable";
    }
    // The runtime now lives under the chosen folder, so the one we used to
    // unpack beside the .exe is dead weight. Remove it — but only once it holds
    // nothing but that old runtime, so a folder still containing config/ or
    // data/ (an interrupted move, a user's own files) is never touched.
    if (portable_base && !same_path(base, default_base) && holds_only_runtime(default_base)) {
        std::error_code rm_ec;
        fs::remove_all(default_base, rm_ec);
    }

    const fs::path current = base / (portable_base ? L"runtime" : L"current");
    const fs::path version_file = base / L"version.txt";
    // Only the beside-the-exe layout redirects config+data. In the LOCALAPPDATA
    // fallback we leave RETCOMM_HOME unset so the hub resolves its normal
    // %LOCALAPPDATA%\retcomm default — which is where an existing portable
    // user's library already lives.
    const fs::path data_root = portable_base ? base : fs::path();
    const std::string want_ver = RETCOMM_VERSION;

    bool need_extract = true;
    {
        std::ifstream in(version_file);
        std::string have;
        if (in && std::getline(in, have) && have == want_ver) {
            std::error_code ec;
            if (fs::is_regular_file(current / "retcomm-hub.exe", ec)) need_extract = false;
        }
    }

    if (need_extract) {
        std::wstring err;
        if (!extract_payload(self, payload_offset, payload_size, current, &err)) {
            LocalFree(argv);
            fail(err.empty() ? L"Payload extract failed." : err);
            return 1;
        }
        std::error_code ec;
        fs::create_directories(base, ec);
        std::ofstream out(version_file, std::ios::trunc);
        out << want_ver << "\n";
    }

    write_channel(current, self, data_root);

    const bool cli = argc >= 2 && _wcsicmp(argv[1], L"cli") == 0;
    std::wstring err;
    bool ok = false;
    if (cli) {
        const fs::path bin = current / "retcomm.exe";
        std::error_code ec;
        if (!fs::is_regular_file(bin, ec)) {
            LocalFree(argv);
            fail(L"retcomm.exe missing from portable runtime.");
            return 1;
        }
        ok = launch(bin, join_args(2, argc, argv), self, data_root, true, &err);
    } else {
        const fs::path hub = current / "retcomm-hub.exe";
        std::error_code ec;
        if (!fs::is_regular_file(hub, ec)) {
            LocalFree(argv);
            fail(L"retcomm-hub.exe missing from portable runtime.");
            return 1;
        }
        ok = launch(hub, join_args(1, argc, argv), self, data_root, false, &err);
    }

    LocalFree(argv);
    if (!ok) {
        fail(err.empty() ? L"Launch failed." : err);
        return 1;
    }
    return 0;
}
