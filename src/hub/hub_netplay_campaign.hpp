#pragma once

#include "retcomm/hash.hpp"
#include "retcomm/netplay_client.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <set>

namespace retcomm::hub {

inline constexpr char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string b64_encode(const std::string& in) {
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out += kBase64Alphabet[v >> 18];
        out += kBase64Alphabet[(v >> 12) & 63];
        out += kBase64Alphabet[(v >> 6) & 63];
        out += kBase64Alphabet[v & 63];
    }
    if (i < in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += kBase64Alphabet[v >> 18];
        out += kBase64Alphabet[(v >> 12) & 63];
        out += i + 1 < in.size() ? kBase64Alphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

inline bool b64_decode(const std::string& in, std::string* out) {
    out->clear();
    unsigned v = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const char* at = std::strchr(kBase64Alphabet, c);
        if (!at || !c) return false;
        v = (v << 6) | static_cast<unsigned>(at - kBase64Alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<char>((v >> bits) & 0xff));
        }
    }
    return true;
}

struct CampaignSave {
    static constexpr int signal_type = 0x4e53;
    using Read = std::function<bool(std::string* data, std::string* error)>;
    using Send = std::function<void(const std::string& text, const std::string& recipient)>;

    void sync(const netplay::Snapshot& room, std::size_t size, const Read& read, const Send& send) {
        using nlohmann::json;
        if (!size || !room.in_room) {
            *this = {};
            return;
        }
        if (lobby != room.lobby_id || host != room.host_player_id) {
            *this = {};
            lobby = room.lobby_id;
            host = room.host_player_id;
        }
        if (host.empty() || room.local_slot < 0) return;
        if (room.is_host && digest.empty() && error.empty()) {
            if (!read || !read(&data, &error) || data.size() != size) {
                if (error.empty()) error = "The host's cartridge save could not be read.";
                return;
            }
            digest = sha256_hex(data);
        }
        if (room.is_host && !digest.empty()) {
            const auto left = [&](const std::string& player) {
                return std::none_of(room.members.begin(), room.members.end(), [&](const auto& member) {
                    return !member.is_spectator && !member.is_local && member.player_id == player;
                });
            };
            for (auto it = sent.begin(); it != sent.end();) {
                if (!left(*it)) {
                    ++it;
                    continue;
                }
                received.erase(*it);
                it = sent.erase(it);
            }
            for (const auto& member : room.members) {
                if (member.is_spectator || member.is_local || member.player_id.empty() ||
                    !sent.insert(member.player_id).second) continue;
                const json offer = {{"room", lobby}, {"sha256", digest}, {"data", b64_encode(data)}};
                send(offer.dump(), member.player_id);
            }
        }
        for (const auto& signal : room.signals) {
            if (signal.seq <= signal_seen) continue;
            signal_seen = signal.seq;
            if (signal.type != signal_type || signal.text.size() > 4096) continue;
            const auto msg = json::parse(signal.text, nullptr, false);
            if (!msg.is_object() || !msg.contains("room") || msg["room"] != lobby ||
                !msg.contains("sha256") || !msg["sha256"].is_string()) continue;
            const auto& sha = msg["sha256"].get_ref<const std::string&>();
            if (room.is_host) {
                if (!msg.contains("data") && sha == digest && sent.count(signal.from_player_id)) {
                    received.insert(signal.from_player_id);
                }
                continue;
            }
            if (signal.from_player_id != host || !msg.contains("data") || !msg["data"].is_string()) continue;
            const auto& encoded = msg["data"].get_ref<const std::string&>();
            std::string decoded;
            if (encoded.size() != 4 * ((size + 2) / 3) || !b64_decode(encoded, &decoded) ||
                decoded.size() != size || sha256_hex(decoded) != sha || (!digest.empty() && digest != sha)) continue;
            data = std::move(decoded);
            digest = sha;
            const json ack = {{"room", lobby}, {"sha256", digest}};
            send(ack.dump(), host);
        }
    }

    std::string problem(const netplay::Snapshot& room) const {
        if (!error.empty()) return error;
        if (lobby != room.lobby_id || host != room.host_player_id || digest.empty()) {
            return "Waiting for the host's campaign save";
        }
        if (room.is_host) {
            for (const auto& member : room.members) {
                if (!member.is_spectator && !member.is_local && !received.count(member.player_id)) {
                    return "Waiting for " + member.display_name + " to receive the campaign save";
                }
            }
        }
        return {};
    }

    std::string data;

private:
    std::string lobby, host, digest, error;
    std::set<std::string> sent, received;
    std::uint64_t signal_seen = 0;
};

} // namespace retcomm::hub
