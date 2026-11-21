#pragma once

#include "hub/hub_core_settings.hpp"
#include "retcomm/mods.hpp"
#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>

namespace retcomm::hub {

inline constexpr const char* kRoomModPlan = "netplay.mod_plan";

inline bool room_mod_plan_option(const CoreOptionDecl& o) {
    return o.netplay && o.key == kRoomModPlan && o.type == "string";
}

inline bool valid_room_mod_plan(const std::string& value) {
    return value.empty() || (value.size() == 16 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }));
}

inline nlohmann::json write_room_mods(const std::vector<N64MatchMod>& mods) {
    auto result = nlohmann::json::array();
    for (const auto& mod : mods) {
        std::string features;
        for (const auto& id : mod.features) features += (features.empty() ? "" : ",") + id;
        result.push_back({{"id", mod.id}, {"ver", mod.version}, {"feats", features}, {"options", mod.options}});
    }
    return result;
}

inline bool read_room_mods(const nlohmann::json& caps, std::vector<N64MatchMod>* mods) {
    if (!caps.is_object() || !caps.contains("mod_plan")) return false;
    const auto& entries = caps["mod_plan"];
    if (!entries.is_array() || entries.size() > 16) return false;
    std::vector<N64MatchMod> rows;
    for (const auto& item : entries) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
            !item.contains("ver") || !item["ver"].is_string() ||
            !item.contains("feats") || !item["feats"].is_string()) return false;
        N64MatchMod row;
        row.id = item["id"].get<std::string>();
        row.version = item["ver"].get<std::string>();
        const auto& features = item["feats"].get_ref<const std::string&>();
        if (row.id.empty() || row.id.size() >= 96 || row.version.empty() || row.version.size() >= 32 ||
            features.empty() || features.size() >= 192 || features.back() == ',') return false;
        std::istringstream input(features);
        for (std::string feature; std::getline(input, feature, ',');) {
            if (feature.empty() || row.features.size() >= 32) return false;
            row.features.push_back(std::move(feature));
        }
        if (const auto opts = item.find("options"); opts != item.end()) {
            if (!opts->is_object() || opts->size() > 32) return false;
            for (auto it = opts->begin(); it != opts->end(); ++it) {
                if (it.key().size() > 128 || !it->is_string() || it->get_ref<const std::string&>().size() > 128)
                    return false;
                row.options.emplace(it.key(), it->get<std::string>());
            }
        }
        rows.push_back(std::move(row));
    }
    *mods = std::move(rows);
    return true;
}

// Explicit room controls use the netplay.* namespace. Other simulation
// options retain their existing common defaults; display options stay local.
inline bool room_option(const CoreOptionDecl& o) {
    return o.netplay && o.key.rfind("netplay.", 0) == 0 && o.type == "enum" && !o.values.empty();
}

inline bool read_room_options(const nlohmann::json& caps,
                              std::map<std::string, std::string>* out) {
    out->clear();
    if (!caps.is_object() || !caps.contains("core_options")) return true;
    const auto& options = caps["core_options"];
    if (!options.is_object() || options.size() > 32) return false;
    for (auto it = options.begin(); it != options.end(); ++it) {
        if (!it.value().is_string() || it.key().size() > 128 || it.value().get_ref<const std::string&>().size() > 128)
            return false;
        out->emplace(it.key(), it.value().get<std::string>());
    }
    return true;
}

inline bool apply_room_options(const std::vector<CoreOptionDecl>& decl,
                               const std::map<std::string, std::string>& room,
                               std::map<std::string, std::string>* local,
                               std::string* error) {
    for (const auto& [key, value] : room) {
        const auto found = std::find_if(decl.begin(), decl.end(), [&](const auto& o) { return o.key == key; });
        const bool accepted = found != decl.end() &&
            (room_mod_plan_option(*found) ? valid_room_mod_plan(value) :
             room_option(*found) && std::find(found->values.begin(), found->values.end(), value) != found->values.end());
        if (!accepted) {
            *error = "The room selected an unsupported match option: " + key;
            return false;
        }
    }
    for (const auto& o : decl) {
        if (o.netplay && o.key != "system.romhack") local->erase(o.key);
    }
    local->insert(room.begin(), room.end());
    return true;
}

} // namespace retcomm::hub
