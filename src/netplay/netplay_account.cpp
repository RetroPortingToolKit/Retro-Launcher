#include "retcomm/netplay_account.hpp"

#include "retcomm/hash.hpp"
#include "retcomm/http.hpp"
#include "retcomm/paths.hpp"

#include "nlohmann/json.hpp"

#include <chrono>
#include <fstream>
#include <system_error>
#include <thread>

namespace retcomm::netplay {

using json = nlohmann::json;

DiscordAccount::DiscordAccount(std::string http_base, fs::path secret_path)
    : http_base_(std::move(http_base)), secret_path_(std::move(secret_path)) {
    while (!http_base_.empty() && http_base_.back() == '/') http_base_.pop_back();
}

std::string DiscordAccount::base_from_ws_url(const std::string& ws_url) {
    // ws://host:port/path -> http://host:port ; wss:// -> https://
    std::string rest = ws_url;
    std::string scheme = "http://";
    if (rest.rfind("wss://", 0) == 0) {
        scheme = "https://";
        rest = rest.substr(6);
    } else if (rest.rfind("ws://", 0) == 0) {
        rest = rest.substr(5);
    } else if (rest.rfind("https://", 0) == 0) {
        scheme = "https://";
        rest = rest.substr(8);
    } else if (rest.rfind("http://", 0) == 0) {
        rest = rest.substr(7);
    }
    const auto slash = rest.find('/');
    if (slash != std::string::npos) rest.resize(slash);
    return scheme + rest;
}

bool DiscordAccount::post_json(const std::string& path, const std::string& body, long* status,
                               std::string* response, std::string* error) {
    const HttpResponse res = http_post_json(http_base_ + path, body);
    if (status) *status = res.status;
    if (response) *response = res.body;
    if (res.status == 0) {
        if (error) *error = res.error.empty() ? "unreachable" : res.error;
        return false;
    }
    return true;
}

bool DiscordAccount::has_stored_key() const {
    std::error_code ec;
    return fs::is_regular_file(secret_path_, ec);
}

bool DiscordAccount::load_key(std::string* player_id, std::string* secret,
                              std::string* handle) const {
    std::ifstream in(secret_path_);
    if (!in) return false;
    json j = json::parse(in, nullptr, false);
    if (!j.is_object()) return false;
    if (player_id) *player_id = j.value("player_id", "");
    if (secret) *secret = j.value("secret", "");
    if (handle) *handle = j.value("handle", "");
    return !j.value("player_id", "").empty() && !j.value("secret", "").empty();
}

bool DiscordAccount::save_key(const std::string& player_id, const std::string& secret,
                              const std::string& handle, const std::string& username,
                              std::string* error) {
    std::error_code ec;
    fs::create_directories(secret_path_.parent_path(), ec);
    {
        std::ofstream out(secret_path_, std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + secret_path_.string();
            return false;
        }
        out << json{{"player_id", player_id},
                    {"secret", secret},
                    {"handle", handle},
                    {"discord_username", username}}
                   .dump(2)
            << "\n";
    }
#if !defined(_WIN32)
    fs::permissions(secret_path_, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
#endif
    return true;
}

bool DiscordAccount::prove(const std::string& player_id, const std::string& secret,
                           std::string* nonce, std::string* proof, long* status,
                           std::string* error) {
    std::string body;
    const json req = {{"player_id", player_id}};
    if (!post_json("/auth/challenge", req.dump(), status, &body, error)) return false;
    if (*status != 200) {
        if (error) *error = "challenge: HTTP " + std::to_string(*status);
        return false;
    }
    const json j = json::parse(body, nullptr, false);
    *nonce = j.is_object() ? j.value("nonce", "") : "";
    if (nonce->empty()) {
        if (error) *error = "challenge: no nonce in reply";
        return false;
    }
    // The key never leaves this process; only the HMAC does.
    const std::string verifier_hex = sha256_hex(secret);
    *proof = hmac_sha256_hex(verifier_hex, *nonce);
    return true;
}

bool DiscordAccount::restore(AccountInfo* out, std::string* error, bool* rejected) {
    if (rejected) *rejected = false;
    std::string player_id, secret, handle;
    if (!load_key(&player_id, &secret, &handle)) {
        if (error) *error = "no stored key";
        return false;
    }
    long status = 0;
    std::string nonce, proof;
    if (!prove(player_id, secret, &nonce, &proof, &status, error)) {
        if (status == 401 || status == 403 || status == 404) {
            std::error_code ec;
            fs::remove(secret_path_, ec);
            if (rejected) *rejected = true;
        }
        return false;
    }
    std::string body;
    const json req = {{"player_id", player_id}, {"nonce", nonce}, {"proof", proof}};
    if (!post_json("/auth/session", req.dump(), &status, &body, error)) return false;
    if (status != 200) {
        if (status == 401 || status == 403 || status == 404) {
            std::error_code ec;
            fs::remove(secret_path_, ec);
            if (rejected) *rejected = true;
            if (error) *error = "the server no longer knows this device key";
        } else if (error) {
            *error = "session: HTTP " + std::to_string(status);
        }
        return false;
    }
    const json j = json::parse(body, nullptr, false);
    if (!j.is_object() || j.value("session", "").empty()) {
        if (error) *error = "session: malformed reply";
        return false;  // 200 without a session is the server misbehaving
    }
    if (out) {
        out->session = j.value("session", "");
        out->player_id = j.value("player_id", player_id);
        out->handle = j.value("handle", handle);
        out->discord_username = j.value("discord_username", "");
    }
    return true;
}

bool DiscordAccount::login(AccountInfo* out, std::string* error, int timeout_s, bool open_browser,
                           const std::function<void(const std::string&)>& on_url) {
    long status = 0;
    std::string body;
    if (!post_json("/auth/discord/start", "{}", &status, &body, error)) return false;
    if (status == 503) {
        unavailable_ = true;
        if (error) *error = "this lobby server does not offer Discord sign-in";
        return false;
    }
    unavailable_ = false;
    const json start = json::parse(body, nullptr, false);
    const std::string code = start.is_object() ? start.value("code", "") : "";
    const std::string url = start.is_object() ? start.value("url", "") : "";
    if (status != 200 || code.empty() || url.empty()) {
        if (error) *error = "start: HTTP " + std::to_string(status);
        return false;
    }
    if (on_url) on_url(url);
    if (open_browser) {
        std::string berr;
        if (!open_url_in_browser(url, &berr) && error) *error = "could not open browser: " + berr;
        // Keep polling anyway: the URL was reported and can be opened by hand.
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    const json poll = {{"code", code}};
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        std::string perr;
        if (!post_json("/auth/discord/poll", poll.dump(), &status, &body, &perr)) continue;
        if (status == 202) continue;  // pending
        if (status == 200) {
            const json j = json::parse(body, nullptr, false);
            if (!j.is_object() || j.value("session", "").empty()) {
                if (error) *error = "poll: malformed completion";
                return false;
            }
            const std::string player_id = j.value("player_id", "");
            const std::string secret = j.value("netplay_secret", "");
            const std::string handle = j.value("handle", "");
            const std::string username = j.value("discord_username", "");
            if (!player_id.empty() && !secret.empty()) {
                std::string serr;
                if (!save_key(player_id, secret, handle, username, &serr) && error) *error = serr;
            }
            if (out) {
                out->session = j.value("session", "");
                out->player_id = player_id;
                out->handle = handle;
                out->discord_username = username;
            }
            if (error) error->clear();
            return true;
        }
        // 403: cancelled or refused; anything else: give up with the text.
        const json j = json::parse(body, nullptr, false);
        const std::string why = j.is_object() ? j.value("error", j.value("message", "")) : "";
        if (error) *error = "sign-in failed (HTTP " + std::to_string(status) + (why.empty() ? ")" : "): " + why);
        return false;
    }
    if (error) *error = "sign-in timed out waiting for the browser";
    return false;
}

bool DiscordAccount::sign_out(std::string* error) {
    std::string player_id, secret, handle;
    const bool had = load_key(&player_id, &secret, &handle);
    bool ok = true;
    if (had) {
        long status = 0;
        std::string nonce, proof, err;
        if (prove(player_id, secret, &nonce, &proof, &status, &err)) {
            std::string body;
            const json req = {{"player_id", player_id}, {"nonce", nonce}, {"proof", proof}};
            post_json("/auth/secret/revoke", req.dump(), &status, &body, &err);
            if (status != 200) {
                ok = false;
                if (error) *error = "revoke: HTTP " + std::to_string(status) + " " + err;
            }
        } else {
            ok = false;
            if (error) *error = err;
        }
    }
    std::error_code ec;
    fs::remove(secret_path_, ec);
    return ok;
}

}  // namespace retcomm::netplay
