#include "hub/hub_netplay_options.hpp"
#include <cstdio>

using namespace retcomm::hub;

int main() {
    CoreOptionDecl aspect;
    aspect.key = "netplay.aspect";
    aspect.type = "enum";
    aspect.netplay = aspect.restart = true;
    aspect.values = {"16:9", "21:9", "32:9"};
    CoreOptionDecl scale;
    scale.key = "video.internal_resolution";
    scale.type = "enum";
    scale.values = {"1x", "4x", "16x"};
    CoreOptionDecl cpu;
    cpu.key = "system.cpu_mode";
    cpu.netplay = true;
    const std::vector<CoreOptionDecl> decl = {aspect, scale, cpu};
    int failed = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failed; }
    };
    for (const auto& selected : aspect.values) {
        nlohmann::json caps = {{"core_options", {{"netplay.aspect", selected}}}};
        std::map<std::string, std::string> room;
        check(read_room_options(caps, &room), "read host room options");
        std::map<std::string, std::string> local = {{"netplay.aspect", "16:9"},
            {"video.internal_resolution", "16x"}, {"video.aspect", "fit"},
            {"engine.internal_scale", "8"}, {"system.cpu_mode", "Fast"},
            {"system.romhack", "same-base"}};
        std::string error;
        check(apply_room_options(decl, room, &local, &error), "adopt host aspect");
        check(local["netplay.aspect"] == selected, "host overrides guest camera choice");
        check(local["video.internal_resolution"] == "16x" && local["engine.internal_scale"] == "8",
              "peer scale remains local");
        check(local["video.aspect"] == "fit", "offline preference is preserved");
        check(!local.count("system.cpu_mode"), "other simulation settings retain common defaults");
        check(local["system.romhack"] == "same-base", "romhack identity preserved");
    }
    for (const auto& room : std::vector<std::map<std::string, std::string>>{
            {{"netplay.aspect", "fit"}}, {{"video.internal_resolution", "1x"}}, {{"unknown", "x"}}}) {
        std::map<std::string, std::string> local = {{"video.internal_resolution", "16x"}};
        const auto before = local;
        std::string error;
        check(!apply_room_options(decl, room, &local, &error) && local == before,
              "invalid room cannot alter options");
    }
    std::map<std::string, std::string> room;
    check(!read_room_options({{"core_options", {{"netplay.aspect", 1}}}}, &room), "reject non-string value");
    check(!read_room_options({{"core_options", "16:9"}}, &room), "reject non-object options");
    check(read_room_options(nlohmann::json::object(), &room) && room.empty(), "old room uses shared defaults");
    return failed ? 1 : 0;
}
