// portable_trailer.hpp — locate the RCM1 payload trailer in a portable exe.
//
// Layout: [PE stub][zip payload][uint64 LE payload_size]["RCM1"]
//
// Authenticode signing appends the certificate table to the END of the file,
// after our trailer: [stub][payload][trailer][cert table]. The last 12 bytes
// are then certificate data, so a signed portable exe is only found by asking
// the PE header where the certificate table starts and reading the trailer
// just before it. Both the stub (to extract itself) and the hub's self-update
// (to recognise a downloaded stub) use this one reader, so a signed release
// works everywhere the unsigned one did.
#pragma once

#include <cstdint>
#include <cstring>
#include <istream>

namespace retcomm_portable {

inline constexpr char kTrailerMagic[4] = {'R', 'C', 'M', '1'};
inline constexpr uint64_t kTrailerSize = 12;

// File offset of the Authenticode certificate table (IMAGE_DIRECTORY_ENTRY_SECURITY),
// or 0 when the image has none / is not a PE file.
inline uint64_t authenticode_table_offset(std::istream& in, uint64_t file_size) {
    auto rd32 = [&](uint64_t at, uint32_t* v) -> bool {
        if (at + 4 > file_size) return false;
        in.clear();
        in.seekg(static_cast<std::streamoff>(at));
        unsigned char b[4]{};
        in.read(reinterpret_cast<char*>(b), 4);
        if (!in || in.gcount() != 4) return false;
        *v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
             (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
        return true;
    };
    auto rd16 = [&](uint64_t at, uint16_t* v) -> bool {
        uint32_t w = 0;
        if (at + 2 > file_size) return false;
        in.clear();
        in.seekg(static_cast<std::streamoff>(at));
        unsigned char b[2]{};
        in.read(reinterpret_cast<char*>(b), 2);
        if (!in || in.gcount() != 2) return false;
        w = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8);
        *v = static_cast<uint16_t>(w);
        return true;
    };
    uint16_t mz = 0;
    if (!rd16(0, &mz) || mz != 0x5A4D) return 0;            // "MZ"
    uint32_t e_lfanew = 0;
    if (!rd32(0x3C, &e_lfanew)) return 0;
    uint32_t pe = 0;
    if (!rd32(e_lfanew, &pe) || pe != 0x00004550) return 0;  // "PE\0\0"
    const uint64_t opt = static_cast<uint64_t>(e_lfanew) + 4 + 20;
    uint16_t magic = 0;
    if (!rd16(opt, &magic)) return 0;
    // Data directory 4 (security): PE32+ at optional header +0x88, PE32 at +0x78.
    uint64_t dir = 0;
    if (magic == 0x20B) dir = opt + 0x88;
    else if (magic == 0x10B) dir = opt + 0x78;
    else return 0;
    uint32_t va = 0, size = 0;
    if (!rd32(dir, &va) || !rd32(dir + 4, &size)) return 0;
    if (va == 0 || size == 0) return 0;
    if (static_cast<uint64_t>(va) + size > file_size) return 0;
    return va;
}

// Finds the trailer. On success, *payload_size / *payload_offset describe the zip.
inline bool find_payload(std::istream& in, uint64_t file_size, uint64_t* payload_size,
                         uint64_t* payload_offset) {
    auto try_at = [&](uint64_t trailer_at) -> bool {
        if (trailer_at + kTrailerSize > file_size) return false;
        in.clear();
        in.seekg(static_cast<std::streamoff>(trailer_at));
        unsigned char buf[12]{};
        in.read(reinterpret_cast<char*>(buf), 12);
        if (!in || in.gcount() != 12) return false;
        if (std::memcmp(buf + 8, kTrailerMagic, 4) != 0) return false;
        uint64_t size = 0;
        for (int i = 7; i >= 0; --i) size = (size << 8) | buf[i];
        if (size == 0 || size > trailer_at) return false;
        if (payload_size) *payload_size = size;
        if (payload_offset) *payload_offset = trailer_at - size;
        return true;
    };
    if (file_size < kTrailerSize) return false;
    // Unsigned: the trailer is the last 12 bytes.
    if (try_at(file_size - kTrailerSize)) return true;
    // Signed: the trailer sits right before the certificate table.
    const uint64_t cert = authenticode_table_offset(in, file_size);
    if (cert >= kTrailerSize && try_at(cert - kTrailerSize)) return true;
    return false;
}

}  // namespace retcomm_portable
