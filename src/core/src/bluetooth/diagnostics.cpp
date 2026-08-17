#include "tether/bluetooth/diagnostics.hpp"

#include "tether/bluetooth/config.hpp"
#include "tether/version.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <regex>

namespace tether::bluetooth {

    namespace {

        // A pairing attempt is a few dozen events; this holds several attempts
        // without letting a long-running daemon accumulate a report nobody reads.
        constexpr size_t TIMELINE_CAPACITY = 200;

        const std::regex kDevNode(R"(dev_[0-9A-Fa-f]{2}(?:_[0-9A-Fa-f]{2}){5})");
        const std::regex kAddress(R"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})");
        const std::regex kEmail(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})");
        const std::regex kNumber(R"(\+[0-9]{7,15})");
        const std::regex kRuntimeDir(R"(/run/user/[0-9]+)");

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

    std::string Redactor::text(const std::string& in) {
        std::string out = in;

        // Directories first: a path under $HOME can contain anything below.
        if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
            out = replace_literal(out, runtime, "<runtime>");
        out = replace_all(out, kRuntimeDir, [](const std::string&) { return "<runtime>"; });
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

        nlohmann::json report;
        report["command"] = "bt_diagnostics";
        report["version"] = TETHER_VERSION;
        report["auth_strategy"] = to_string(config.auth_strategy);
        // The address itself is never reported; whether one is selected is what
        // matters for reading the rest.
        report["device_selected"] = !config.device_address.empty();
        report["ancs_enabled"] = config.ancs_enabled;
        report["ancs_content_enabled"] = config.ancs_content_enabled;
        report["group_messages_enabled"] = config.group_messages_enabled;
        report["status"] = redactor.value(status);
        report["connection"] = redactor.value(connection);
        report["timeline"] = redactor.value(diagnostic_timeline());
        return report;
    }

} // namespace tether::bluetooth
