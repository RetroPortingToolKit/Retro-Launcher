// retcomm-hub.exe -- the hub's old name on Windows, kept as a forwarder.
//
// The hub is retro-hub.exe as of 2026-09-25. Installs and portable runtimes
// from before carry shortcuts, Steam entries and -- the one that matters -- a
// self-updater that relaunches "retcomm-hub.exe" after it swaps the files.
// Symlinks are not dependable on Windows, so this tiny program starts
// retro-hub.exe beside it with the same command line and returns its exit
// code, waiting for it so a caller that waits on retcomm-hub.exe still waits
// for the hub. Linux and macOS use a symlink instead.

#include <windows.h>

#include <string>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return 1;
    std::wstring dir(self, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    const std::wstring target = dir + L"retro-hub.exe";

    // Keep the arguments exactly as given: everything after our own argv[0].
    const wchar_t* cmd = GetCommandLineW();
    const wchar_t* rest = cmd;
    if (*rest == L'"') {
        ++rest;
        while (*rest && *rest != L'"') ++rest;
        if (*rest) ++rest;
    } else {
        while (*rest && *rest != L' ' && *rest != L'\t') ++rest;
    }
    std::wstring line = L"\"" + target + L"\"" + rest;

    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(target.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi)) {
        MessageBoxW(nullptr, L"retro-hub.exe is missing beside retcomm-hub.exe.",
                    L"Retro Launcher", MB_ICONERROR);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
