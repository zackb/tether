#include "tether/bluetooth/config.hpp"
#include "tether/log.hpp"
#include "tether/paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace tether::bluetooth {

    namespace {
        // 2 since 0.2.24 how to tell "never had a retention setting" from "lost one".
        constexpr int CONFIG_VERSION = 2;
    } // namespace

    const char* to_string(AuthStrategy strategy) {
        return strategy == AuthStrategy::ExplicitPair ? "explicit-pair" : "connect-first";
    }

    AuthStrategy auth_strategy_from_string(const std::string& value) {
        return value == "explicit-pair" ? AuthStrategy::ExplicitPair : AuthStrategy::ConnectFirst;
    }

    std::string config_path() {
        const std::filesystem::path dir = paths::config_dir();
        if (dir.empty())
            return {};
        return (dir / "bluetooth.json").string();
    }

    std::string serialize_config(const Config& config) {
        nlohmann::json j;
        j["config_version"] = CONFIG_VERSION;
        j["device_address"] = config.device_address;
        j["auth_strategy"] = to_string(config.auth_strategy);
        j["ancs_enabled"] = config.ancs_enabled;
        j["ancs_content_enabled"] = config.ancs_content_enabled;
        j["group_messages_enabled"] = config.group_messages_enabled;
        j["calls_enabled"] = config.calls_enabled;
        j["enabled"] = config.enabled;
        j["adapter"] = config.adapter;
        j["retention"] = to_string(config.retention);
        j["desktop_popups_enabled"] = config.desktop_popups_enabled;
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
            const int version = j.value("config_version", 0);
            config.ancs_content_enabled = version < 1 ? true : j.value("ancs_content_enabled", true);
            config.group_messages_enabled = j.value("group_messages_enabled", false);
            config.calls_enabled = j.value("calls_enabled", false);
            config.enabled = j.value("enabled", true);
            config.adapter = j.value("adapter", "");
            config.retention = retention_from_string(j.value("retention", "encrypted"));
            config.desktop_popups_enabled = j.value("desktop_popups_enabled", true);
        } catch (const std::exception&) {
            // A corrupt file must not stop the daemon; defaults are safe.
        }
        return config;
    }

    std::string supervised_address(const Config& config) {
        return config.enabled ? config.device_address : std::string{};
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
