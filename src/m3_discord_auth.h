#pragma once
#include <string>

namespace m3discord {

struct Config {
    std::string client_id = "1570322072373170177";
    std::string guild_id = "1570576347481251940";
    std::string playtester_role_id = "1570577186069422122";
    std::string redirect_uri = "http://127.0.0.1:48912/callback";
    bool require_playtester = false;
};

struct Session {
    bool signed_in = false;
    bool in_guild = false;
    bool is_playtester = false;
    std::string user_id;
    std::string username;
    std::string global_name;
    std::string access_token;
    std::string refresh_token;
};

Config load_config(const std::string& dir);
Session load_session(const std::string& dir);
void save_session(const std::string& dir, const Session& s);
void clear_session(const std::string& dir);

void begin_login(const std::string& dir);
void cancel_login();
bool poll(const std::string& dir, Session& out_session);
void begin_refresh(const std::string& dir);

bool is_busy();
std::string status_text();

}  // namespace m3discord
