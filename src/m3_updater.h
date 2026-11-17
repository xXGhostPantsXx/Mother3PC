#pragma once
#include <string>
#include <vector>

namespace m3updater {

using StrVec = std::vector<std::string>;

inline constexpr const char* kLauncherVersion = "2.0.0";
inline constexpr const char* kDefaultBuildVersion = "2026.11.13-rc1";

struct Config {
    std::string manifest_url;
    std::string channel = "stable";
    std::string build_version = kDefaultBuildVersion;
};

struct State {
    bool checking = false;
    bool downloading = false;
    bool update_available = false;
    bool ready_to_restart = false;
    std::string latest_version;
    std::string download_url;
    std::string status = "Up to date";
    StrVec patch_notes;
};

Config load_config(const std::string& dir);
StrVec load_local_notes(const std::string& dir);
void begin_check(const std::string& dir);
void begin_download_and_apply(const std::string& dir);
void poll(State& st);
void launch_restart_script(const std::string& dir);

}  // namespace m3updater
