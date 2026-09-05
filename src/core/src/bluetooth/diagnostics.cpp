#include "tether/bluetooth/diagnostics.hpp"
#include <tether/secret_store.hpp>

#include "tether/bluetooth/config.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/bluetooth/pairing.hpp"
#include "tether/paths.hpp"
#include "tether/version.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <glib.h>
#include <mutex>
#include <regex>
#include <sys/utsname.h>

namespace tether::bluetooth {

    namespace {

        // A pairing attempt is a few dozen events; this holds several attempts
        // without letting a long-running daemon accumulate a report nobody reads.
        constexpr size_t TIMELINE_CAPACITY = 200;

        // BlueZ has no version on D-Bus, installed version has to come from the tooling.
        std::string bluez_version() {
            static const std::string cached = [] {
                gchar* out = nullptr;
                gint status = 0;
                GError* error = nullptr;
                if (!g_spawn_command_line_sync("bluetoothctl --version", &out, nullptr, &status, &error)) {
                    g_clear_error(&error);
                    g_free(out);
                    return std::string();
                }
                // "bluetoothctl: 5.86"
                std::string text = out ? out : "";
                g_free(out);
                const size_t colon = text.rfind(':');
                if (colon == std::string::npos)
                    return std::string();
                std::string version = text.substr(colon + 1);
                const size_t begin = version.find_first_not_of(" \t\r\n");
                const size_t end = version.find_last_not_of(" \t\r\n");
                if (begin == std::string::npos)
                    return std::string();
                return version.substr(begin, end - begin + 1);
            }();
            return cached;
        }

        std::string kernel_release() {
            utsname info{};
            return uname(&info) == 0 ? std::string(info.release) : std::string();
        }

        const std::regex kDevNode(R"(dev_[0-9A-Fa-f]{2}(?:_[0-9A-Fa-f]{2}){5})");
        const std::regex kAddress(R"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})");
        const std::regex kEmail(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})");
        const std::regex kNumber(R"(\+[0-9]{7,15})");
        const std::regex kRuntimeDir(R"(/run/user/[0-9]+)");

        // Device names that identify a model rather than a person.
        bool is_generic_device_name(const std::string& name) {
            static const char* const GENERIC[] = {
                "iphone", "ipad", "ipod", "mac", "macbook", "apple watch", "watch", "airpods", "tether", "phone"};
            std::string lowered = name;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            for (const char* generic : GENERIC)
                if (lowered == generic)
                    return true;
            return false;
        }

        std::string upper(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
            return s;
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        std::string replace_all(const std::string& in,
                                const std::regex& re,
                                const std::function<std::string(const std::string&)>& fn) {
            std::string out;
            size_t last = 0;
            for (auto it = std::sregex_iterator(in.begin(), in.end(), re); it != std::sregex_iterator(); ++it) {
                out.append(in, last, static_cast<size_t>(it->position()) - last);
                out += fn(it->str());
                last = static_cast<size_t>(it->position()) + static_cast<size_t>(it->length());
            }
            out.append(in, last, std::string::npos);
            return out;
        }

        std::string replace_literal(std::string in, const std::string& needle, const std::string& with) {
            if (needle.empty())
                return in;
            for (size_t at = in.find(needle); at != std::string::npos; at = in.find(needle, at + with.size()))
                in.replace(at, needle.size(), with);
            return in;
        }

        // Events describing link and pairing state. Everything else the daemon
        // broadcasts carries message or notification content.
        bool is_state_event(const std::string& command) {
            return command == "bt_pair_progress" || command == "bt_pair_result" || command == "bt_unpair_result" ||
                   command == "bt_connection_changed" || command == "bt_send_result" || command == "bt_message_read";
        }

        std::mutex g_timeline_mutex;
        std::deque<nlohmann::json> g_timeline;
        std::chrono::steady_clock::time_point g_timeline_origin;
        bool g_timeline_started = false;

    } // namespace

    bool is_content_key(const std::string& key) {
        return key == "body" || key == "sender" || key == "subject" || key == "name" || key == "title" ||
               key == "preview" || key == "alias";
    }

    std::string Redactor::placeholder(const std::string& kind, const std::string& key) {
        const std::string prefix = "<" + kind + "-";
        if (auto it = assigned_.find(key); it != assigned_.end())
            return it->second;

        size_t n = 1;
        for (const auto& [existing, token] : assigned_)
            if (token.rfind(prefix, 0) == 0)
                ++n;

        std::string token = prefix + std::to_string(n) + ">";
        assigned_.emplace(key, token);
        return token;
    }

    void Redactor::hide(const std::string& literal, const std::string& kind) {
        // A one- or two-character alias would shred unrelated text.
        if (literal.size() < 3)
            return;

        if (is_generic_device_name(literal))
            return;

        for (const auto& [existing, token] : literals_)
            if (existing == literal)
                return;
        literals_.emplace_back(literal, placeholder(kind, literal));
        std::sort(literals_.begin(), literals_.end(), [](const auto& a, const auto& b) {
            return a.first.size() > b.first.size();
        });
    }

    std::string Redactor::text(const std::string& in) {
        std::string out = in;

        for (const auto& [literal, token] : literals_)
            out = replace_literal(out, literal, token);

        // Directories first: a path under $HOME can contain anything below.
        if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
            out = replace_literal(out, runtime, "<runtime>");
        out = replace_all(out, kRuntimeDir, [](const std::string&) { return "<runtime>"; });
        // Before $HOME, so an XDG directory outside the home is still covered.
        if (const std::string config = paths::config_dir().string(); !config.empty())
            out = replace_literal(out, config, "<config>");
        if (const std::string data = paths::data_dir().string(); !data.empty())
            out = replace_literal(out, data, "<data>");
        if (const char* home = std::getenv("HOME"); home && *home)
            out = replace_literal(out, home, "<home>");

        // Normalized to the colon form so one device gets one placeholder
        // however the address was spelled.
        out = replace_all(out, kDevNode, [this](const std::string& match) {
            std::string address = match.substr(4);
            std::replace(address.begin(), address.end(), '_', ':');
            return "dev_" + placeholder("address", upper(address));
        });
        out = replace_all(
            out, kAddress, [this](const std::string& match) { return placeholder("address", upper(match)); });
        out = replace_all(out, kEmail, [this](const std::string& match) { return placeholder("email", lower(match)); });
        out = replace_all(out, kNumber, [this](const std::string& match) { return placeholder("number", match); });
        return out;
    }

    nlohmann::json Redactor::value(const nlohmann::json& in) {
        if (in.is_string())
            return text(in.get<std::string>());

        if (in.is_array()) {
            nlohmann::json out = nlohmann::json::array();
            for (const auto& element : in)
                out.push_back(value(element));
            return out;
        }

        if (in.is_object()) {
            nlohmann::json out = nlohmann::json::object();
            for (const auto& [key, element] : in.items()) {
                if (is_content_key(key))
                    continue;
                out[key] = value(element);
            }
            return out;
        }

        return in;
    }

    void record_diagnostic_event(const nlohmann::json& event) {
        if (!event.is_object() || !is_state_event(event.value("command", "")))
            return;

        nlohmann::json entry = nlohmann::json::object();
        for (const auto& [key, value] : event.items()) {
            if (!is_content_key(key))
                entry[key] = value;
        }

        std::lock_guard<std::mutex> lock(g_timeline_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (!g_timeline_started) {
            g_timeline_origin = now;
            g_timeline_started = true;
        }
        entry["at_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_timeline_origin).count();

        g_timeline.push_back(std::move(entry));
        if (g_timeline.size() > TIMELINE_CAPACITY)
            g_timeline.pop_front();
    }

    nlohmann::json diagnostic_timeline() {
        std::lock_guard<std::mutex> lock(g_timeline_mutex);
        nlohmann::json out = nlohmann::json::array();
        for (const auto& entry : g_timeline)
            out.push_back(entry);
        return out;
    }

    void clear_diagnostic_timeline() {
        std::lock_guard<std::mutex> lock(g_timeline_mutex);
        g_timeline.clear();
        g_timeline_started = false;
    }

    nlohmann::json build_diagnostics(const nlohmann::json& status, const nlohmann::json& connection) {
        const Config config = load_config();
        Redactor redactor;

        if (g_bluez) {
            for (const auto& device : g_bluez->snapshot().devices)
                redactor.hide(device.name, "name");
            for (const auto& adapter : g_bluez->snapshot().adapters)
                redactor.hide(adapter.name, "name");
        }

        nlohmann::json report;
        report["command"] = "bt_diagnostics";
        report["version"] = TETHER_VERSION;
        report["bluez_version"] = bluez_version();
        report["kernel"] = kernel_release();
        report["auth_strategy"] = to_string(config.auth_strategy);
        // The address itself is never reported; whether one is selected is what
        // matters for reading the rest.
        report["device_selected"] = !config.device_address.empty();
        report["ancs_enabled"] = config.ancs_enabled;
        report["ancs_content_enabled"] = config.ancs_content_enabled;
        report["group_messages_enabled"] = config.group_messages_enabled;
        report["enabled"] = config.enabled;
        report["ancs_soliciting"] = ancs_solicitation_active();
        report["retention"] = to_string(config.retention);
        report["retention_ready"] = secret::have_key();
        report["status"] = redactor.value(status);
        report["connection"] = redactor.value(connection);
        report["timeline"] = redactor.value(diagnostic_timeline());
        return report;
    }

} // namespace tether::bluetooth
