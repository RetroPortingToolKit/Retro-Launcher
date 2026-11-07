#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace retcomm::hub {
// Optional private display data. The movie remains the runner's authority.
struct ReplayInfo {
    std::filesystem::path path;
    std::string title, seal, preview_status;
    std::int64_t saved_unix = 0;
    std::uint64_t fields = 0;
    std::uint32_t rate_num = 0, rate_den = 0;
    float aspect = 4.f / 3.f;
    bool sealed = false;
    std::vector<std::uint8_t> pixels; // existing 160x120 RGBA thumbnail
};
std::filesystem::path replay_preview_path(const std::filesystem::path& movie);
ReplayInfo read_replay_info(const std::filesystem::path& movie);
bool write_replay_preview(const ReplayInfo& info, std::string& error);
std::string replay_duration(const ReplayInfo& info);
std::vector<ReplayInfo> list_replays(const std::filesystem::path& directory);
} // namespace retcomm::hub
