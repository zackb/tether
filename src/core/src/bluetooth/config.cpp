#include "tether/bluetooth/config.hpp"
#include "tether/log.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <pwd.h>
#include <unistd.h>

namespace tether::bluetooth {

    namespace {

        std::string home_dir() {
            if (const char* home = getenv("HOME"); home && *home)
                return home;
            if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir)
                return pw->pw_dir;
            return {};
        }

    } // namespace

    const char* to_string(AuthStrategy strategy) {
        return strategy == AuthStrategy::ExplicitPair ? "explicit-pair" : "connect-first";
    }

    AuthStrategy auth_strategy_from_string(const std::string& value) {
        return value == "explicit-pair" ? AuthStrategy::ExplicitPair : AuthStrategy::ConnectFirst;
    }

    std::string config_path() {
        std::string home = home_dir();
        if (home.empty())
            return {};
        return (std::filesystem::path(home) / ".config" / "tether" / "bluetooth.json").string();
    }

    std::string serialize_config(const Config& config) {
        nlohmann::json j;
        j["device_address"] = config.device_address;
        j["auth_strategy"] = to_string(config.auth_strategy);
        j["ancs_enabled"] = config.ancs_enabled;
        j["ancs_content_enabled"] = config.ancs_content_enabled;
        j["group_messages_enabled"] = config.group_messages_enabled;
        return j.dump(2);
    }

    Config deserialize_config(const std::string& text) {
        Config config;
        try {
            auto j = nlohmann::json::parse(text);
            if (!j.is_object())
                return config;
            config.device_address = j.value("device_address", "");
            config.auth_strategy = auth_strategy_from_string(j.value("auth_strategy", ""));
            config.ancs_enabled = j.value("ancs_enabled", true);
            config.ancs_content_enabled = j.value("ancs_content_enabled", false);
            config.group_messages_enabled = j.value("group_messages_enabled", false);
        } catch (const std::exception&) {
            // A corrupt file must not stop the daemon; defaults are safe.
        }
        return config;
    }

    Config load_config() {
        std::string path = config_path();
        if (path.empty())
            return {};
        std::ifstream in(path);
        if (!in.is_open())
            return {};
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return deserialize_config(text);
    }

    bool save_config(const Config& config) {
        std::string path = config_path();
        if (path.empty())
            return false;

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

        // Write-then-rename so a crash mid-write cannot leave a truncated config,
        // matching how Crypto persists known_hosts.json.
        std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out.is_open()) {
                debug::log(ERR, "bluetooth: cannot write {}", tmp);
                return false;
            }
            out << serialize_config(config);
            if (!out) {
                debug::log(ERR, "bluetooth: failed writing {}", tmp);
                return false;
            }
        }

        std::filesystem::permissions(tmp,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace,
                                     ec);
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            debug::log(ERR, "bluetooth: cannot replace {}: {}", path, ec.message());
            std::filesystem::remove(tmp, ec);
            return false;
        }
        return true;
    }

} // namespace tether::bluetooth
