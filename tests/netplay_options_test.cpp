#include "hub/hub_netplay_campaign.hpp"
#include "hub/hub_netplay_options.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>

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
    CoreOptionDecl mod_plan;
    mod_plan.key = kRoomModPlan;
    mod_plan.type = "string";
    mod_plan.netplay = true;
    const std::vector<CoreOptionDecl> decl = {aspect, scale, cpu, mod_plan};
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
    for (const std::string selected : {"", "7d101a186e5098c8"}) {
        const nlohmann::json caps = {{"core_options", {{kRoomModPlan, selected}}}};
        std::map<std::string, std::string> room, local = {{kRoomModPlan, "different"}};
        std::string error;
        check(read_room_options(caps, &room) && apply_room_options(decl, room, &local, &error) &&
              local.at(kRoomModPlan) == selected, "adopt the verified host gameplay plan");
    }
    for (const auto& room : std::vector<std::map<std::string, std::string>>{
            {{"netplay.aspect", "fit"}}, {{"video.internal_resolution", "1x"}}, {{"unknown", "x"}},
            {{kRoomModPlan, "1234"}}, {{kRoomModPlan, "7z101a186e5098c8"}}}) {
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

    retcomm::ModScanResult mods;
    mods.layout = retcomm::ModLayout::N64lle;
    retcomm::ModPackageInfo story;
    story.id = "yoshisstory.story-coop";
    story.name = "Story Co-op";
    story.version = "0.2.0";
    retcomm::ModFeatureInfo active;
    active.id = "native-actor";
    active.enabled = true;
    story.features = {active};
    retcomm::ModOptionInfo camera;
    camera.feature_id = active.id;
    camera.id = "camera";
    camera.type = "choice";
    camera.default_value = camera.value = "unified";
    camera.choices = {{"unified", "Unified"}, {"split", "Split"}};
    story.options = {camera};
    retcomm::ModPackageInfo bonus = story;
    bonus.id = "yoshisstory.bonus-roster";
    bonus.features[0].id = "main";
    bonus.features[0].enabled = false;
    auto fit = story;
    fit.id = "yoshisstory.adaptive-widescreen";
    fit.presentation_only = true;
    fit.features[0].id = "fit";
    fit.options.clear();
    mods.packages = {story, bonus, fit};
    const auto choices = retcomm::selected_n64_match_mods(mods);
    const nlohmann::json caps = {{"mod_plan", write_room_mods(choices)}};
    std::vector<retcomm::N64MatchMod> received;
    check(read_room_mods(caps, &received) && received.size() == 1, "receive only host-enabled mods");
    mods.packages[0].features[0].enabled = false;
    mods.packages[1].features[0].enabled = true;
    std::string toml, error;
    check(retcomm::n64_match_mods_toml(mods, received, &toml, &error) &&
          toml.find("[yoshisstory.story-coop]\nenabled = \"native-actor\"") != std::string::npos &&
          toml.find("[yoshisstory.bonus-roster]\nenabled = \"\"") != std::string::npos,
          "materialize the host plan over a different guest selection");
    check(!mods.packages[0].features[0].enabled && mods.packages[1].features[0].enabled,
          "preserve the guest selection");
    check(toml.find("[yoshisstory.adaptive-widescreen]\nenabled = \"fit\"") != std::string::npos,
          "preserve a local presentation default");
    mods.packages[2].features[0].enabled = false;
    check(retcomm::n64_match_mods_toml(mods, received, &toml, &error) &&
          toml.find("[yoshisstory.adaptive-widescreen]\nenabled = \"\"") != std::string::npos,
          "preserve an explicit local presentation opt-out");
    auto bad = received;
    bad.push_back({fit.id, fit.version, {"fit"}, {}});
    check(!retcomm::n64_match_mods_toml(mods, bad, &toml, &error),
          "reject a host override of local presentation");
    if (!received.empty()) {
        received[0].version = "unavailable";
        check(!retcomm::n64_match_mods_toml(mods, received, &toml, &error), "refuse missing host package version");
        received[0].version = story.version;
        received[0].features.push_back("native-actor");
        check(!retcomm::n64_match_mods_toml(mods, received, &toml, &error), "refuse repeated features");
    }

    const auto profile = retcomm::fs::temp_directory_path() /
        ("retcomm-match-options-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (retcomm::fs::create_directory(profile)) {
        mods.selection = profile / "mods.toml";
        const auto read_selection = [&] {
            std::ifstream in(mods.selection);
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        };
        const std::string other = "[unrelated]\nenabled = \"item\"\n";
        std::ofstream(mods.selection) << other;
        const std::map<std::string, std::string> overrides = {{story.id, "camera=split"}};
        check(stage_room_mod_options(mods, overrides, &error) && read_selection() == other,
              "a room option does not turn on a disabled mod");
        mods.packages[0].features[0].enabled = true;
        check(stage_room_mod_options(mods, overrides, &error), "stage the enabled mod's match camera");
        const auto staged = read_selection();
        check(staged.find(other) != std::string::npos &&
              staged.find("options = \"camera=split\"") != std::string::npos &&
              mods.packages[0].options[0].value == "unified",
              "the disposable profile gets Split and leaves other choices intact");
        check(!stage_room_mod_options(mods, {{story.id, "camera=unknown"}}, &error) &&
              read_selection() == staged, "reject unsupported declared match values");
        retcomm::fs::remove(mods.selection);
        retcomm::fs::remove(profile);
    } else {
        check(false, "create a disposable match profile");
    }

    using retcomm::netplay::Member;
    using retcomm::netplay::Snapshot;
    Snapshot host_view;
    host_view.in_room = host_view.is_host = true;
    host_view.lobby_id = "story-room";
    host_view.player_id = host_view.host_player_id = "host";
    host_view.local_slot = 0;
    Member host_member, guest_member, spectator;
    host_member.player_id = "host";
    host_member.is_host = host_member.is_local = true;
    guest_member.player_id = "guest";
    guest_member.display_name = "Partner";
    spectator.player_id = "spectator";
    spectator.is_spectator = true;
    host_view.members = {host_member, guest_member, spectator};
    Snapshot guest_view = host_view;
    guest_view.is_host = guest_view.members[0].is_local = false;
    guest_view.player_id = "guest";
    guest_view.local_slot = 1;
    guest_view.members[1].is_local = true;

    std::string host_save(2048, '\xff');
    host_save.replace(42, 5, "story");
    CampaignSave from_host, at_guest;
    auto read_host = [&](std::string* data, std::string*) { *data = host_save; return true; };
    std::string offer, ack;
    from_host.sync(host_view, host_save.size(), read_host, [&](const auto& text, const auto& target) {
        check(offer.empty() && target == "guest", "share the campaign only with the seated guest");
        offer = text;
    });
    check(!offer.empty() && !from_host.problem(host_view).empty(), "wait for the guest receipt");
    auto send_ack = [&](const auto& text, const auto& target) {
        check(target == "host", "send the receipt only to the host");
        ack = text;
    };
    const int kind = CampaignSave::signal_type;
    auto damaged = nlohmann::json::parse(offer, nullptr, false);
    if (damaged.is_object()) damaged["sha256"] = "invalid";
    guest_view.signals = {{1, "spectator", kind, offer}, {2, "host", kind, damaged.dump()}};
    at_guest.sync(guest_view, host_save.size(), {}, send_ack);
    check(ack.empty() && at_guest.data.empty() && !at_guest.problem(guest_view).empty(),
          "reject a forged sender and a mismatched digest");
    guest_view.signals.push_back({3, "host", kind, offer});
    at_guest.sync(guest_view, host_save.size(), {}, send_ack);
    check(!ack.empty() && at_guest.problem(guest_view).empty() && at_guest.data == host_save,
          "accept the verified host's exact EEPROM");
    host_view.signals = {{4, "spectator", kind, ack}};
    auto unexpected_offer = [&](const auto&, const auto&) { check(false, "resend to the same waiting seat"); };
    from_host.sync(host_view, host_save.size(), read_host, unexpected_offer);
    check(!from_host.problem(host_view).empty(), "ignore the spectator receipt");
    host_view.signals.push_back({5, "guest", kind, ack});
    from_host.sync(host_view, host_save.size(), read_host, unexpected_offer);
    check(from_host.problem(host_view).empty(), "permit launch after the guest acknowledges the snapshot");
    guest_view.lobby_id = "different-room";
    at_guest.sync(guest_view, host_save.size(), {}, [&](const auto&, const auto&) {
        check(false, "accept an offer from another room");
    });
    check(at_guest.data.empty() && !at_guest.problem(guest_view).empty(), "drop the old room's EEPROM");
    return failed ? 1 : 0;
}
