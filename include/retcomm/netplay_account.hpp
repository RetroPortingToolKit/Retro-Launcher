#pragma once

// Discord sign-in against recomp-net-server's /auth endpoints, over libcurl.
//
// Flow (recomp-net-server/src/discord_auth.rs, routes/mod.rs):
//   POST /auth/discord/start  {}            -> {code, url}   (503: not offered)
//   open `url` in the browser; the player authorises; the server's callback
//   parks the result under `code`
//   POST /auth/discord/poll   {code}        -> 202 pending | 200 Completed
//                                              {session, netplay_secret,
//                                               player_id, handle,
//                                               discord_username} | 403 failed
// The device key `netplay_secret` is returned once and stored locally. Every
// later start re-mints a session without a browser:
//   POST /auth/challenge      {player_id}   -> {nonce}
//   POST /auth/session        {player_id, nonce, proof}
//     proof = HMAC-SHA256(key = hex(SHA-256(secret)), msg = nonce), hex
//   -> {session, player_id, handle, discord_username}
// A 401/403/404 on that round is the server saying "unknown key" and the
// stored key is deleted; anything else is "ask again later" and the key is
// kept (recomp-net/src/auth/rnet_auth.c learned that the hard way).
//
// Calls block; the hub runs them on a worker and polls, the CLI calls them
// directly.

#include <filesystem>
#include <functional>
#include <string>

namespace retcomm::netplay {

namespace fs = std::filesystem;

struct AccountInfo {
    std::string player_id;
    std::string handle;            // server-owned display name
    std::string discord_username;  // @handle, the disambiguator
    std::string session;           // JWT for hello{session}; ~45 min
};

class DiscordAccount {
public:
    // `http_base` is "http://host:port" or "https://host"; see base_from_ws_url.
    DiscordAccount(std::string http_base, fs::path secret_path);

    static std::string base_from_ws_url(const std::string& ws_url);

    bool has_stored_key() const;
    // Silent relogin with the stored key. On a rejection the key is deleted
    // and `rejected` is set; on unreachable it is kept.
    bool restore(AccountInfo* out, std::string* error, bool* rejected = nullptr);
    // Interactive login. `on_url` receives the authorisation URL; when
    // `open_browser` is true it is also opened with the OS handler.
    bool login(AccountInfo* out, std::string* error, int timeout_s = 300, bool open_browser = true,
               const std::function<void(const std::string&)>& on_url = {});
    // Revokes this device's key on the server (best effort) and deletes it.
    bool sign_out(std::string* error);
    // True when the last /auth/discord/start answered 503.
    bool unavailable() const { return unavailable_; }

private:
    bool post_json(const std::string& path, const std::string& body, long* status,
                   std::string* response, std::string* error);
    bool load_key(std::string* player_id, std::string* secret, std::string* handle) const;
    bool save_key(const std::string& player_id, const std::string& secret,
                  const std::string& handle, const std::string& username, std::string* error);
    bool prove(const std::string& player_id, const std::string& secret, std::string* nonce,
               std::string* proof, long* status, std::string* error);

    std::string http_base_;
    fs::path secret_path_;
    bool unavailable_ = false;
};

}  // namespace retcomm::netplay
