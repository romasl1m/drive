#pragma once

#include <string>
#include <cstdlib>
#include <filesystem>

namespace drive {

// Telegram application credentials (registered at https://my.telegram.org)
// These belong to the Drive application, not individual users.
constexpr int APP_API_ID = 23815619;
constexpr const char *APP_API_HASH = "f2895bc3380cbb20af7a139914df259d";

constexpr const char *APP_NAME = "Drive";
constexpr const char *APP_VERSION = "1.0.0";
constexpr const char *APP_DESCRIPTION = "Private backup to Telegram";

inline std::string data_dir() {
    const char *xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && xdg[0])
        return std::string(xdg) + "/drive";
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.local/share/drive";
}

inline std::string config_dir() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        return std::string(xdg) + "/drive";
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.config/drive";
}

} // namespace drive
