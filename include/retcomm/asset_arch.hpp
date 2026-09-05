#pragma once

#include <string>

namespace retcomm {

// True when this launcher binary runs on a 64-bit ARM host (Apple Silicon,
// aarch64 Linux, Windows on ARM). Wine installs inherit the host arch too:
// there is no separate Windows-on-ARM target, so the host is the best guess.
inline constexpr bool host_is_arm64() {
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)
    return true;
#else
    return false;
#endif
}

// Score a release asset name (already lower-cased) by CPU architecture.
// The asset for the host arch scores 2; the other family scores 1 so a
// release that ships only one arch is still picked (Rosetta / box64 can run it).
// A name with no arch token scores 0.
inline int asset_arch_score(const std::string& lower_name) {
    const bool arm = lower_name.find("arm64") != std::string::npos ||
                     lower_name.find("aarch64") != std::string::npos;
    const bool x64 = lower_name.find("x64") != std::string::npos ||
                     lower_name.find("amd64") != std::string::npos ||
                     lower_name.find("x86_64") != std::string::npos ||
                     lower_name.find("win64") != std::string::npos;
    if (host_is_arm64()) {
        if (arm) return 2;
        if (x64) return 1;
    } else {
        if (x64) return 2;
        if (arm) return 1;
    }
    return 0;
}

} // namespace retcomm
