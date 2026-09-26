#pragma once

// Loading an rcore core: the one exported symbol, the ABI handshake, and the
// identity the host computes itself (docs/CORE_ABI.md, "Savestates": a
// self-reported identity can be wrong; a hash of the file cannot).

#include "rcore/rcore.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace retro::runner {

namespace fs = std::filesystem;

struct LoadedCore {
    fs::path path;               // as given
    std::string sha256;          // of the bytes actually loaded (lowercase hex)
    void* handle = nullptr;      // dlopen / LoadLibrary handle
    const rcore_core_api* api = nullptr;
    const rcore_core_info* info = nullptr;
};

// Opens the library once, hashes it through that open file, and loads that
// same file, so the hash names the code that runs even if the path is
// replaced mid-load. Refuses a core that exports no rcore_entry or will not
// serve RCORE_ABI_MAJOR. Returns false and sets *error on any refusal.
bool load_core(const fs::path& path, LoadedCore& out, std::string* error);

// True when the core's table is long enough to hold `member` -- the
// append-only rule: a core built against an older draft revision simply ends
// earlier, and a field past its struct_size does not exist.
template <typename T>
bool has_field(const T* s, std::size_t member_offset, std::size_t member_size) {
    return s && s->struct_size >= member_offset + member_size;
}
#define RCORE_HAS(ptr, type, member) \
    ::retro::runner::has_field((ptr), offsetof(type, member), sizeof(((type*)0)->member))

// The capability bits by their manifest names, in bit order.
std::string capability_names(std::uint64_t caps, std::uint64_t* unnamed = nullptr);

} // namespace retro::runner
