#pragma once

#include "hub/hub_core_settings.hpp"
#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>

namespace retcomm::hub {

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
        if (found == decl.end() || !room_option(*found) ||
            std::find(found->values.begin(), found->values.end(), value) == found->values.end()) {
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
