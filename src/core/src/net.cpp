#include "tether/net.hpp"
#include <tether/i18n.hpp>

#include <arpa/inet.h>
#include <csignal>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tether/base64.hpp"
#include "tether/bluetooth/airpods.hpp"
#include "tether/bluetooth/config.hpp"
#include "tether/bluetooth/connection.hpp"
#include "tether/bluetooth/contacts.hpp"
#include "tether/bluetooth/diagnostics.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/bluetooth/pairing.hpp"
#include "tether/client.hpp"
#include "tether/crypto.hpp"
#include "tether/discovery.hpp"
#include "tether/file_transfer.hpp"
#include "tether/otp.hpp"
#include "tether/paths.hpp"
#include "tether/wayland.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <openssl/err.h>
#include <optional>
#include <set>
#include <sstream>
#include <tether/bluetooth/journal.hpp>
#include <tether/log.hpp>
#include <tether/secret_store.hpp>
#include <tether/version.hpp>
#include <thread>
#include <vector>

namespace tether {

    struct ClientSession {
        int fd;
        SSL* ssl;                      // nullptr for plain unix clients
        bool clipboard_images = false; // announced in hello
    };

    // Global list of active connected sessions
    static std::map<int, ClientSession> active_sessions;
    static std::mutex g_sessions_mutex;
    static std::set<int> local_subscribers;
    static std::mutex g_subscribers_mutex;

    // last known mdns peers.
    static std::mutex g_discovered_mutex;
    static nlohmann::json g_discovered_devices = nlohmann::json::array();

    struct ReceivedFileInfo {
        std::string path;
        std::string filename;
        size_t bytes_written = 0;
    };

    struct ConnectedClientSnapshot {
        std::string address;
        std::string fingerprint;
        std::string device_name;
        bool paired = false;
    };

    static std::vector<ReceivedFileInfo> recent_received_files;
    static constexpr size_t kMaxRecentReceivedFiles = 16;
    static std::map<int, ConnectedClientSnapshot> connected_remote_clients;

    void register_client_fd(int fd) {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        active_sessions[fd] = {fd, nullptr};
    }

    void register_client_ssl(int fd, SSL* ssl) {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        active_sessions[fd] = {fd, ssl};
    }

    void unregister_client_fd(int fd) {
        {
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            active_sessions.erase(fd);
        }
        std::lock_guard<std::mutex> lock(g_subscribers_mutex);
        local_subscribers.erase(fd);
    }

    // The extension may send the code as a JSON string or a JSON number. Reading a
    // number straight into std::string throws type_error.302, which the bare catch in
    // the message loop swallowed, silently dropping the OTP.
    static std::string otp_from_json(const nlohmann::json& value) {
        if (value.is_string())
            return value.get<std::string>();
        if (value.is_number_integer())
            return std::to_string(value.get<long long>());
        if (value.is_number_unsigned())
            return std::to_string(value.get<unsigned long long>());
        return "";
    }

    // Opens a link from a paired device in the default browser. Only http(s) with a
    // host is accepted so a peer can never reach file:, custom-scheme or other handlers.
    static void open_web_url(const std::string& url) {
        if (url.size() > 8192) {
            debug::log(WARN, "open_url: rejected oversized URL");
            return;
        }
        GUri* uri = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, nullptr);
        if (!uri) {
            debug::log(WARN, "open_url: rejected unparsable URL");
            return;
        }
        const char* host = g_uri_get_host(uri);
        const char* scheme = g_uri_get_scheme(uri);
        if (!host || !*host || (g_ascii_strcasecmp(scheme, "http") != 0 && g_ascii_strcasecmp(scheme, "https") != 0)) {
            debug::log(WARN, "open_url: rejected non-web URL with scheme '{}'", scheme ? scheme : "");
            g_uri_unref(uri);
            return;
        }
        // Launch the re-serialized parse, not the raw input.
        char* normalized = g_uri_to_string(uri);
        g_uri_unref(uri);
        GError* error = nullptr;
        if (!g_app_info_launch_default_for_uri(normalized, nullptr, &error)) {
            debug::log(ERR, "open_url: failed to open '{}': {}", normalized, error ? error->message : "unknown");
            g_clear_error(&error);
        }
        g_free(normalized);
    }

    static nlohmann::json make_otp_event(const Otp& otp) {
        nlohmann::json event;
        event["command"] = "otp_available";
        event["otp"] = otp.code;
        event["otp_id"] = otp.id;
        if (!otp.sender_domain.empty())
            event["sender_domain"] = otp.sender_domain;
        return event;
    }

    void otp_publish(const std::string& code, const std::string& sender_domain, int exclude_fd) {
        if (code.empty())
            return;
        if (Otp current = otp_peek(); current && current.code == code)
            return;
        uint64_t id = otp_store(code, sender_domain);
        broadcast_local_event(make_otp_event({code, sender_domain, id}).dump(), exclude_fd);
    }

    void set_discovered_devices(const nlohmann::json& devices) {
        std::lock_guard<std::mutex> lock(g_discovered_mutex);
        g_discovered_devices = devices.is_array() ? devices : nlohmann::json::array();
    }

    void register_local_subscriber(int fd) {
        std::lock_guard<std::mutex> lock(g_subscribers_mutex);
        local_subscribers.insert(fd);
    }

    void unregister_local_subscriber(int fd) {
        std::lock_guard<std::mutex> lock(g_subscribers_mutex);
        local_subscribers.erase(fd);
    }

    // Helper for robust SSL writes on non-blocking sockets.
    // Handles SSL_ERROR_WANT_WRITE by retrying.
    static int robust_ssl_write(SSL* ssl, const void* buf, int num) {
        if (!ssl)
            return -1;
        int total_written = 0;
        const char* p = static_cast<const char*>(buf);
        while (total_written < num) {
            int n = SSL_write(ssl, p + total_written, num - total_written);
            if (n <= 0) {
                int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                    usleep(1000); // Yield to avoid busy-wait
                    continue;
                }
                return n; // Fatal error
            }
            total_written += n;
        }
        return total_written;
    }

    // Plain-socket counterpart to robust_ssl_write. These packets are
    // newline-delimited JSON and the local client fds are non-blocking, so a
    // short write splits one event across the framing boundary: every later
    // event on that socket is then misparsed and the client goes silently deaf
    // rather than erroring. Either the whole packet goes out or none of it does.
    static std::mutex g_write_locks_mutex;
    static std::map<int, std::mutex> g_write_locks;

    static std::mutex& write_lock_for(int fd) {
        std::lock_guard<std::mutex> lock(g_write_locks_mutex);
        return g_write_locks[fd];
    }

    static bool robust_plain_write(int fd, const void* buf, size_t num) {
        std::lock_guard<std::mutex> lock(write_lock_for(fd));
        const char* p = static_cast<const char*>(buf);
        size_t written = 0;
        int stalls = 0;
        // Before the first byte, a socket that will not drain just costs us the
        // packet. After it there is no way back, so a committed write waits far
        // longer before abandoning a stream it has already framed.
        constexpr int STALL_LIMIT_UNCOMMITTED = 200;
        constexpr int STALL_LIMIT_COMMITTED = 5000;

        while (written < num) {
            const ssize_t n = write(fd, p + written, num - written);
            if (n > 0) {
                written += static_cast<size_t>(n);
                stalls = 0;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (++stalls > (written == 0 ? STALL_LIMIT_UNCOMMITTED : STALL_LIMIT_COMMITTED)) {
                    debug::log(ERR, "net: fd {} is not draining; {} of {} bytes written", fd, written, num);
                    return false;
                }
                usleep(1000);
                continue;
            }
            debug::log(ERR, "net write error on fd {}: {}", fd, std::strerror(errno));
            return false;
        }
        return true;
    }

    void broadcast_message(const std::string& msg, int exclude_fd) {
        std::string packet = msg;
        if (!packet.empty() && packet.back() != '\n')
            packet += '\n';

        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        for (auto const& [fd, session] : active_sessions) {
            if (fd == exclude_fd)
                continue;
            if (session.ssl) {
                robust_ssl_write(session.ssl, packet.c_str(), packet.size());
            } else {
                robust_plain_write(fd, packet.c_str(), packet.size());
            }
        }
    }

    static void write_plain_packet(int fd, const std::string& packet) {
        robust_plain_write(fd, packet.c_str(), packet.size());
    }

    void broadcast_local_event(const std::string& msg, int exclude_fd) {
        std::string packet = msg;
        if (packet.empty() || packet.back() != '\n')
            packet += '\n';

        // Bluetooth state events feed the diagnostic timeline.
        if (packet.find("\"bt_") != std::string::npos) {
            try {
                bluetooth::record_diagnostic_event(nlohmann::json::parse(packet));
            } catch (...) {
            }
        }

        std::lock_guard<std::mutex> lock(g_subscribers_mutex);
        for (int fd : local_subscribers) {
            if (fd == exclude_fd)
                continue;
            write_plain_packet(fd, packet);
        }
    }

    // How long an unanswered pairing request stays offerable.
    static constexpr int64_t PENDING_PAIR_TTL_SECONDS = 3600;

    static void save_pending_pairs(const nlohmann::json& pending) {
        std::ofstream ofs(get_runtime_dir() + "/pending_pairs.json");
        ofs << pending.dump(4);
    }

    nlohmann::json prune_pending_pairs(const nlohmann::json& raw, int64_t now) {
        nlohmann::json live = nlohmann::json::object();
        if (!raw.is_object())
            return live;

        for (const auto& [fingerprint, value] : raw.items()) {
            if (value.is_string()) {
                live[fingerprint] = {{"name", value.get<std::string>()}, {"ts", now}};
                continue;
            }
            if (!value.is_object() || !value.contains("name"))
                continue;
            if (now - value.value("ts", static_cast<int64_t>(0)) > PENDING_PAIR_TTL_SECONDS)
                continue;
            live[fingerprint] = value;
        }
        return live;
    }

    // Expired entries are dropped on read rather than on a timer.
    static nlohmann::json load_pending_pairs() {
        std::ifstream ifs(get_runtime_dir() + "/pending_pairs.json");
        if (!ifs.is_open())
            return nlohmann::json::object();

        nlohmann::json raw;
        try {
            raw = nlohmann::json::parse(ifs);
        } catch (...) {
            return nlohmann::json::object();
        }
        ifs.close();

        nlohmann::json live = prune_pending_pairs(raw, static_cast<int64_t>(std::time(nullptr)));
        if (live.size() != raw.size() || live != raw)
            save_pending_pairs(live);
        return live;
    }

    static std::string pending_pair_name(const nlohmann::json& pending, const std::string& fingerprint) {
        if (!pending.contains(fingerprint))
            return "";
        return pending[fingerprint].value("name", "");
    }

    // A request that was answered, or whose peer has gone, is not pending.
    static void erase_pending_pair(const std::string& fingerprint) {
        auto pending = load_pending_pairs();
        if (!pending.contains(fingerprint))
            return;
        pending.erase(fingerprint);
        save_pending_pairs(pending);
    }

    static std::string lookup_known_host_name(const std::string& fingerprint) {
        try {
            auto known = nlohmann::json::parse(Crypto::instance().get_known_hosts_dump());
            if (known.contains(fingerprint) && known[fingerprint].is_string()) {
                return known[fingerprint].get<std::string>();
            }
        } catch (...) {
        }

        return "";
    }

    static nlohmann::json build_bt_connection_status() {
        if (bluetooth::g_bt_connections)
            return bluetooth::g_bt_connections->status();
        return nlohmann::json{{"command", "bt_connection_changed"},
                              {"device_present", false},
                              {"link_reason", _("Bluetooth is unavailable.")},
                              {"profile_reason", ""}};
    }

    nlohmann::json build_protocol_info() {
        nlohmann::json capabilities = {"airpods",
                                       "bluetooth.connection",
                                       "bluetooth.diagnostics",
                                       "bluetooth.pairing",
                                       "contacts",
                                       "files",
                                       "messages",
                                       "notifications",
                                       "otp",
                                       "peers",
                                       "settings"};
        const auto config = bluetooth::load_config();
        if (config.calls_enabled && bluetooth::g_bluez && bluetooth::g_bluez->running())
            capabilities.push_back("calls");
        if (g_wayland && g_wayland->clipboard_available())
            capabilities.push_back("clipboard");
        return {{"command", "protocol_info"}, {"version", 1}, {"capabilities", std::move(capabilities)}};
    }

    nlohmann::json build_bt_status() {
        nlohmann::json status;
        status["command"] = "bt_status";
        status["available"] = bluetooth::g_bluez != nullptr && bluetooth::g_bluez->running();
        const auto config = bluetooth::load_config();
        status["device_address"] = config.device_address;
        status["ancs_enabled"] = config.ancs_enabled;
        status["adapter"] = config.adapter;
        status["ancs_content_enabled"] = config.ancs_content_enabled;
        status["calls_enabled"] = config.calls_enabled;
        status["enabled"] = config.enabled;
        status["retention"] = to_string(config.retention);
        status["retention_ready"] = secret::have_key();
        status["desktop_popups_enabled"] = config.desktop_popups_enabled;
        status["airpods_enabled"] = config.airpods_enabled;
        status["airpods_pause"] = to_string(config.airpods_pause);
        status["airpods_handoff"] = config.airpods_handoff;
        status["lock_on_away"] = config.lock_on_away;
        status["lock_away_seconds"] = config.lock_away_seconds;
        status["version"] = TETHER_VERSION;
        if (!bluetooth::g_bluez) {
            status["capability"] = nullptr;
            status["adapters"] = nlohmann::json::array();
            return status;
        }

        status["capability"] = bluetooth::to_json(bluetooth::g_bluez->capability());
        const auto objects = bluetooth::g_bluez->snapshot();
        nlohmann::json adapters = nlohmann::json::array();
        for (const auto& adapter : objects.adapters)
            adapters.push_back(bluetooth::to_json(adapter));
        status["adapters"] = adapters;
        // AirPods handoff needs the controller to present itself as Apple hardware.
        const auto* adapter = bluetooth::preferred_adapter(objects, bluetooth::g_bluez->preferred_adapter_id());
        status["apple_device_id"] = adapter != nullptr && adapter->presents_as_apple();
        return status;
    }

    static std::atomic<bool> g_desktop_popups_enabled{true};

    bool desktop_popups_enabled() { return g_desktop_popups_enabled.load(); }

    void set_desktop_popups_enabled(bool enabled) { g_desktop_popups_enabled.store(enabled); }

    static std::atomic<bool> g_mdns_available{false};

    bool mdns_available() { return g_mdns_available.load(); }

    nlohmann::json build_mdns_status() {
        return nlohmann::json{{"command", "mdns_status"}, {"available", g_mdns_available.load()}};
    }

    void set_mdns_available(bool available) {
        if (g_mdns_available.exchange(available) == available)
            return;
        if (available)
            debug::log(INFO, "mDNS: avahi-daemon is available");
        else
            debug::log(ERR, "mDNS: avahi-daemon is unavailable; this machine cannot be discovered");
        broadcast_local_event(build_mdns_status().dump());
    }

    bool ufw_enabled(std::string_view ufw_conf) {
        std::istringstream lines{std::string(ufw_conf)};
        for (std::string line; std::getline(lines, line);) {
            const auto begin = line.find_first_not_of(" \t");
            if (begin == std::string::npos || line[begin] == '#')
                continue;
            const auto eq = line.find('=', begin);
            if (eq == std::string::npos)
                continue;

            auto trim = [](std::string_view v) {
                const auto first = v.find_first_not_of(" \t\r");
                if (first == std::string_view::npos)
                    return std::string_view{};
                return v.substr(first, v.find_last_not_of(" \t\r") - first + 1);
            };
            const std::string_view key = trim(std::string_view(line).substr(begin, eq - begin));
            if (key != "ENABLED")
                continue;

            std::string value(trim(std::string_view(line).substr(eq + 1)));
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value == "yes";
        }
        return false;
    }

    // True when firewalld owns its well-known name on the system bus.
    static bool firewalld_running() {
        GError* error = nullptr;
        GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
        if (!bus) {
            g_clear_error(&error);
            return false;
        }

        GVariant* reply = g_dbus_connection_call_sync(bus,
                                                      "org.freedesktop.DBus",
                                                      "/org/freedesktop/DBus",
                                                      "org.freedesktop.DBus",
                                                      "NameHasOwner",
                                                      g_variant_new("(s)", "org.fedoraproject.FirewallD1"),
                                                      G_VARIANT_TYPE("(b)"),
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      1000,
                                                      nullptr,
                                                      &error);
        g_object_unref(bus);
        if (!reply) {
            g_clear_error(&error);
            return false;
        }

        gboolean owned = FALSE;
        g_variant_get(reply, "(b)", &owned);
        g_variant_unref(reply);
        return owned == TRUE;
    }

    bool firewall_active() {
        constexpr auto TTL = std::chrono::seconds(30);
        static std::mutex mutex;
        static std::chrono::steady_clock::time_point checked_at;
        static bool cached = false;

        std::lock_guard<std::mutex> lock(mutex);
        const auto now = std::chrono::steady_clock::now();
        if (checked_at != std::chrono::steady_clock::time_point{} && now - checked_at < TTL)
            return cached;

        cached = false;
        std::ifstream conf("/etc/ufw/ufw.conf");
        if (conf) {
            std::string contents((std::istreambuf_iterator<char>(conf)), std::istreambuf_iterator<char>());
            cached = ufw_enabled(contents);
        }
        if (!cached)
            cached = firewalld_running();
        checked_at = now;
        return cached;
    }

    // Guards against two pairing transactions racing for the same agent and
    // advertisement object paths.
    static std::atomic<bool> g_bt_pair_busy{false};

    constexpr int BT_CONFIRM_TIMEOUT_SECONDS = 60;
    static std::mutex g_bt_confirm_mutex;
    static std::condition_variable g_bt_confirm_cv;
    static int g_bt_confirm_answer = -1; // -1 pending, 0 declined, 1 accepted
    static std::string g_bt_confirm_operation_id;

    // A second StartDiscovery while one is running just gets stopped early by the
    // first one's StopDiscovery.
    static std::atomic<bool> g_bt_scan_busy{false};

    static constexpr int BT_SCAN_SECONDS = 20;

    static void run_bt_scan() {
        nlohmann::json event;
        event["command"] = "bt_scan_result";

        if (g_bt_scan_busy.exchange(true)) {
            event["success"] = false;
            event["message"] = _("A Bluetooth scan is already running.");
            broadcast_local_event(event.dump());
            return;
        }

        std::string err;
        // Each tick republishes the device list, so the UI fills in as the phone
        // shows up rather than only at the end of the scan.
        const bool ok =
            bluetooth::g_bluez &&
            bluetooth::scan_devices(
                *bluetooth::g_bluez, BT_SCAN_SECONDS, []() { broadcast_local_event(build_bt_devices().dump()); }, err);

        g_bt_scan_busy = false;
        event["success"] = ok;
        event["message"] = ok ? _("Bluetooth scan finished.") : (err.empty() ? _("Bluetooth is unavailable.") : err);
        broadcast_local_event(build_bt_devices().dump());
        broadcast_local_event(event.dump());
    }

    static void set_operation_id(nlohmann::json& event, const std::string& operation_id) {
        if (!operation_id.empty())
            event["operation_id"] = operation_id;
    }

    // Runs on the BlueZ monitor's GLib thread, same as the local dialog it replaces.
    static bool ask_client_to_confirm(const std::string& code, const std::string& operation_id) {
        {
            std::lock_guard<std::mutex> lock(g_bt_confirm_mutex);
            g_bt_confirm_answer = -1;
            g_bt_confirm_operation_id = operation_id;
        }

        nlohmann::json event;
        event["command"] = "bt_pair_confirm_request";
        event["code"] = code;
        set_operation_id(event, operation_id);
        broadcast_local_event(event.dump());

        std::unique_lock<std::mutex> lock(g_bt_confirm_mutex);
        g_bt_confirm_cv.wait_for(
            lock, std::chrono::seconds(BT_CONFIRM_TIMEOUT_SECONDS), [] { return g_bt_confirm_answer >= 0; });
        const bool accepted = g_bt_confirm_answer == 1;
        g_bt_confirm_operation_id.clear();
        return accepted;
    }

    static void run_bt_pair(const std::string& address,
                            std::optional<bluetooth::AuthStrategy> strategy,
                            const std::string& operation_id) {
        if (!bluetooth::g_bluez) {
            nlohmann::json event;
            event["command"] = "bt_pair_result";
            event["success"] = false;
            event["status"] = "error";
            event["message"] = _("Bluetooth is unavailable.");
            set_operation_id(event, operation_id);
            broadcast_local_event(event.dump());
            return;
        }

        if (g_bt_pair_busy.exchange(true)) {
            nlohmann::json event;
            event["command"] = "bt_pair_result";
            event["success"] = false;
            event["status"] = "busy";
            event["message"] = _("Another pairing attempt is already in progress.");
            set_operation_id(event, operation_id);
            broadcast_local_event(event.dump());
            return;
        }

        auto config = bluetooth::load_config();
        auto result = bluetooth::pair_device(
            *bluetooth::g_bluez,
            address,
            strategy.value_or(config.auth_strategy),
            [operation_id](const std::string& step, const std::string& detail) {
                nlohmann::json event;
                event["command"] = "bt_pair_progress";
                event["step"] = step;
                event["detail"] = detail;
                set_operation_id(event, operation_id);
                broadcast_local_event(event.dump());
            },
            [operation_id](const std::string& code) { return ask_client_to_confirm(code, operation_id); },
            config.calls_enabled);

        if (result.success) {
            config.device_address = result.device_address;
            // A dual bond proves mirroring can work, so it turns the preference back on.
            // A BR/EDR-only bond must not turn it off.
            if (result.dual_bond)
                config.ancs_enabled = true;
            // Remember the transaction that bonded only when it produced a bond worth repeating.
            if (result.status == "paired" && result.dual_bond)
                config.auth_strategy = result.auth_strategy_used;
            config.enabled = true;
            bluetooth::save_config(config);
            // Point supervision at the device we just bonded with.
            if (bluetooth::g_bt_connections)
                bluetooth::g_bt_connections->set_device(bluetooth::supervised_address(config), config.ancs_enabled);
        }

        g_bt_pair_busy = false;
        nlohmann::json event = bluetooth::to_json(result);
        set_operation_id(event, operation_id);
        broadcast_local_event(event.dump());
        broadcast_local_event(build_bt_status().dump());
    }

    static void restart_supervision(bluetooth::Config config) {
        if (bluetooth::g_bt_connections)
            bluetooth::g_bt_connections->set_device(bluetooth::supervised_address(config), config.ancs_enabled);
        broadcast_local_event(build_bt_status().dump());
    }

    static void run_bt_unpair(const std::string& address, const std::string& operation_id) {
        if (!bluetooth::g_bluez) {
            nlohmann::json event{{"command", "bt_unpair_result"},
                                 {"success", false},
                                 {"message", _("Bluetooth is unavailable.")}};
            set_operation_id(event, operation_id);
            broadcast_local_event(event.dump());
            return;
        }
        auto result = bluetooth::unpair_device(*bluetooth::g_bluez, address);
        nlohmann::json event = bluetooth::to_json(result);
        event["command"] = "bt_unpair_result";
        set_operation_id(event, operation_id);
        broadcast_local_event(event.dump());
        broadcast_local_event(build_bt_status().dump());
    }

    // AirPods only: a manual disconnect of the supervised iPhone would fight its reconnects.
    static void run_bt_airpods_connect(const std::string& address, bool connect) {
        bool ok = false;
        std::string err;
        if (!bluetooth::g_bluez) {
            err = _("Bluetooth is unavailable on this machine.");
        } else {
            bool airpods = false;
            for (const auto& device : bluetooth::g_bluez->snapshot().devices)
                airpods = airpods || (device.address == address && device.looks_like_airpods());
            if (!airpods)
                err = _("Not an AirPods device.");
            else if (connect)
                ok = bluetooth::connect_device(*bluetooth::g_bluez, address, err);
            else
                ok = bluetooth::disconnect_device(*bluetooth::g_bluez, address, err);
        }
        broadcast_local_event(
            nlohmann::json{{"command", "bt_airpods_connect_result"}, {"success", ok}, {"message", err}}.dump());
    }

    nlohmann::json build_bt_threads() {
        nlohmann::json result;
        result["command"] = "bt_threads";
        nlohmann::json threads = nlohmann::json::array();
        {
            std::lock_guard<std::mutex> lock(bluetooth::message_store_mutex());
            for (const auto& thread : bluetooth::message_store().threads()) {
                auto entry = bluetooth::to_json(thread);
                // Contact names are applied for display only. Key on addresses, so a renamed or ambiguous contact can
                // never change which conversation a message belongs to.
                if (auto name = bluetooth::contact_store().name_for(thread.key); !name.empty())
                    entry["name"] = name;
                entry["group"] = thread.key.rfind("group:", 0) == 0;
                threads.push_back(std::move(entry));
            }
        }

        // Resolved after the store lock is released, not underneath it: deciding
        // who a group reply would go to reads the contacts, which the same lock
        // guards, and std::mutex is not recursive.
        for (auto& entry : threads) {
            if (!entry.value("group", false)) {
                // An alphanumeric sender ID has no route back, so the composer must not accept text the phone would
                // bounce.
                bluetooth::Recipient recipient;
                std::string reason;
                const bool repliable =
                    bluetooth::recipient_from_thread_key(entry.value("thread", ""), recipient, reason);
                entry["repliable"] = repliable;
                if (!repliable)
                    entry["reply_reason"] = reason;
                continue;
            }
            std::string reason;
            const auto eligibility = bluetooth::group_reply_status(entry.value("thread", ""), reason);
            entry["repliable"] = eligibility == bluetooth::ReplyEligibility::Allowed;
            entry["reply_status"] = bluetooth::to_string(eligibility);
            if (!reason.empty())
                entry["reply_reason"] = reason;
        }

        result["threads"] = threads;
        return result;
    }

    nlohmann::json build_bt_messages(const std::string& thread_key) {
        nlohmann::json result;
        result["command"] = "bt_messages";
        result["thread"] = thread_key;
        nlohmann::json messages = nlohmann::json::array();
        {
            std::lock_guard<std::mutex> lock(bluetooth::message_store_mutex());
            const std::string name = bluetooth::contact_store().name_for(thread_key);
            for (const auto& message : bluetooth::message_store().messages(thread_key)) {
                auto entry = bluetooth::to_json(message);
                if (!name.empty() && !message.outgoing)
                    entry["name"] = name;
                messages.push_back(std::move(entry));
            }
        }
        result["messages"] = messages;
        return result;
    }

    nlohmann::json build_bt_contacts(const std::string& query, size_t limit) {
        nlohmann::json result;
        result["command"] = "bt_contacts";
        result["query"] = query;
        nlohmann::json contacts = nlohmann::json::array();
        {
            std::lock_guard<std::mutex> lock(bluetooth::message_store_mutex());
            for (const auto& card : bluetooth::contact_store().search(query, limit)) {
                nlohmann::json entry;
                entry["name"] = card.name;
                // Namespaced the same way threads are, so an address here can be
                // handed straight back as the thread of a bt_send_message.
                nlohmann::json addresses = nlohmann::json::array();
                for (const auto& tel : card.tels)
                    if (std::string normalized = bluetooth::normalize_phone(tel); !normalized.empty())
                        addresses.push_back("tel:" + normalized);
                for (const auto& email : card.emails)
                    if (std::string normalized = bluetooth::normalize_email(email); !normalized.empty())
                        addresses.push_back("email:" + normalized);
                if (addresses.empty())
                    continue;
                entry["addresses"] = std::move(addresses);
                contacts.push_back(std::move(entry));
            }
        }
        result["contacts"] = contacts;
        return result;
    }

    nlohmann::json build_bt_devices() {
        nlohmann::json result;
        result["command"] = "bt_devices";
        nlohmann::json devices = nlohmann::json::array();
        if (bluetooth::g_bluez) {
            for (const auto& device : bluetooth::g_bluez->snapshot().devices)
                devices.push_back(bluetooth::to_json(device));
        }
        result["devices"] = devices;
        return result;
    }

    nlohmann::json build_bt_airpods() {
        if (!bluetooth::g_airpods)
            return bluetooth::to_json(bluetooth::AirPodsState{});
        return bluetooth::to_json(bluetooth::g_airpods->state());
    }

    static nlohmann::json build_local_state_snapshot() {
        nlohmann::json snapshot;
        snapshot["command"] = "state_snapshot";

        nlohmann::json paired_devices = nlohmann::json::array();
        try {
            auto known = nlohmann::json::parse(Crypto::instance().get_known_hosts_dump());
            for (auto& [fingerprint, name_value] : known.items()) {
                nlohmann::json device;
                device["fingerprint"] = fingerprint;
                device["device_name"] = name_value.is_string() ? name_value.get<std::string>() : "Unknown Device";
                paired_devices.push_back(device);
            }
        } catch (...) {
        }
        snapshot["paired_devices"] = paired_devices;

        nlohmann::json pending_pairs = nlohmann::json::array();
        auto pending = load_pending_pairs();
        for (auto& [fingerprint, entry] : pending.items()) {
            nlohmann::json item;
            item["fingerprint"] = fingerprint;
            const std::string name = entry.value("name", "");
            item["device_name"] = name.empty() ? "Unknown Device" : name;
            pending_pairs.push_back(item);
        }
        snapshot["pending_pairs"] = pending_pairs;

        nlohmann::json connected_clients = nlohmann::json::array();
        for (auto const& [fd, client] : connected_remote_clients) {
            nlohmann::json item;
            item["fd"] = fd;
            item["address"] = client.address;
            item["fingerprint"] = client.fingerprint;
            item["device_name"] = client.device_name;
            item["paired"] = client.paired;
            connected_clients.push_back(item);
        }
        snapshot["connected_clients"] = connected_clients;
        snapshot["recent_received_files"] = nlohmann::json::array();
        for (const auto& file : recent_received_files) {
            nlohmann::json item;
            item["path"] = file.path;
            item["filename"] = file.filename;
            item["bytes_written"] = file.bytes_written;
            snapshot["recent_received_files"].push_back(item);
        }

        {
            std::lock_guard<std::mutex> lock(g_discovered_mutex);
            snapshot["discovered_devices"] = g_discovered_devices;
        }

        snapshot["mdns_available"] = g_mdns_available.load();
        snapshot["clipboard_available"] = g_wayland && g_wayland->clipboard_available();
        snapshot["firewall_active"] = firewall_active();

        return snapshot;
    }

    void record_received_file(const std::filesystem::path& path, size_t bytes_written) {
        ReceivedFileInfo file;
        file.path = path.string();
        file.filename = path.filename().string();
        file.bytes_written = bytes_written;

        recent_received_files.insert(recent_received_files.begin(), file);
        if (recent_received_files.size() > kMaxRecentReceivedFiles) {
            recent_received_files.resize(kMaxRecentReceivedFiles);
        }

        nlohmann::json event;
        event["command"] = "file_received";
        event["path"] = file.path;
        event["filename"] = file.filename;
        event["bytes_written"] = file.bytes_written;
        broadcast_local_event(event.dump());
    }

    size_t broadcast_tcp_message(const std::string& msg, int exclude_fd) {
        std::string packet = msg;
        if (!packet.empty() && packet.back() != '\n')
            packet += '\n';

        size_t recipients = 0;
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        for (auto const& [fd, session] : active_sessions) {
            if (fd == exclude_fd || !session.ssl)
                continue;

            robust_ssl_write(session.ssl, packet.c_str(), packet.size());
            recipients++;
        }

        return recipients;
    }

    std::vector<std::string> clipboard_image_messages(const std::string& png, const std::string& kind) {
        constexpr size_t CHUNK_BYTES = 512 * 1024;
        if (png.size() > CLIPBOARD_IMAGE_MAX_BYTES) {
            debug::log(WARN, "clipboard image of {} bytes exceeds the transfer cap; not sent", png.size());
            return {};
        }
        if (png.empty())
            return {};

        static std::atomic<unsigned> seq{0};
        const std::string id = "clip_" + std::to_string(time(nullptr)) + "_" + std::to_string(seq++);

        nlohmann::json start{{"command", "file_start"}, {"filename", "clipboard.png"}, {"transfer_id", id}};
        start["size"] = png.size();
        start["clipboard"] = kind;
        std::vector<std::string> out{start.dump()};
        for (size_t off = 0, idx = 0; off < png.size(); off += CHUNK_BYTES, ++idx) {
            const size_t len = std::min(CHUNK_BYTES, png.size() - off);
            nlohmann::json chunk{{"command", "file_chunk"}, {"transfer_id", id}, {"chunk_index", idx}};
            chunk["data"] = base64_encode(reinterpret_cast<const unsigned char*>(png.data() + off), len);
            out.push_back(chunk.dump());
        }
        out.push_back(nlohmann::json{{"command", "file_end"}, {"transfer_id", id}}.dump());
        return out;
    }

    void broadcast_clipboard_image(const std::string& png, int exclude_fd) {
        const auto msgs = clipboard_image_messages(png, "updated");
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        for (auto const& [fd, session] : active_sessions) {
            if (fd == exclude_fd || !session.ssl || !session.clipboard_images)
                continue;
            for (const auto& m : msgs) {
                const std::string packet = m + "\n";
                robust_ssl_write(session.ssl, packet.c_str(), packet.size());
            }
        }
    }

    bool session_accepts_clipboard_images(int fd) {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        auto it = active_sessions.find(fd);
        return it != active_sessions.end() && it->second.clipboard_images;
    }

    static void set_session_clipboard_images(int fd) {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        if (auto it = active_sessions.find(fd); it != active_sessions.end())
            it->second.clipboard_images = true;
    }

    bool ClipboardImageReceiver::start(const std::string& transfer_id, size_t size) {
        id_.clear();
        data_.clear();
        if (transfer_id.empty() || size == 0 || size > CLIPBOARD_IMAGE_MAX_BYTES) {
            debug::log(WARN, "clipboard image of {} bytes rejected", size);
            return false;
        }
        id_ = transfer_id;
        expected_ = size;
        data_.reserve(size);
        return true;
    }

    bool ClipboardImageReceiver::chunk(const std::string& transfer_id, const std::string& b64_data) {
        if (id_.empty() || transfer_id != id_)
            return false;
        auto bytes = base64_decode(b64_data);
        if (data_.size() + bytes.size() > expected_) {
            debug::log(WARN, "clipboard image overran its declared size; dropped");
            id_.clear();
            data_.clear();
            return true;
        }
        data_.append(bytes.begin(), bytes.end());
        return true;
    }

    std::string ClipboardImageReceiver::finish(const std::string& transfer_id) {
        if (id_.empty() || transfer_id != id_)
            return {};
        id_.clear();
        std::string png = std::move(data_);
        data_.clear();
        static constexpr std::string_view PNG_SIGNATURE{"\x89PNG\r\n\x1a\n", 8};
        if (png.size() != expected_ || !png.starts_with(PNG_SIGNATURE)) {
            debug::log(WARN, "clipboard image incomplete or not a PNG; dropped");
            return {};
        }
        return png;
    }

    std::string get_runtime_dir() {
        std::filesystem::path base;
        if (const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR")) {
            base = xdg_runtime;
        } else {
            // login managers create this, but only a login session exports the variable.
            const std::string fallback = "/run/user/" + std::to_string(getuid());
            struct stat st{};
            if (lstat(fallback.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid())
                throw std::runtime_error("XDG_RUNTIME_DIR is not set");
            base = fallback;
        }
        std::filesystem::path tether_dir = base / "tether";
        if (!std::filesystem::exists(tether_dir)) {
            std::filesystem::create_directories(tether_dir);
            std::filesystem::permissions(
                tether_dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
        }
        return tether_dir.string();
    }

    std::string get_state_dir() {
        std::filesystem::path tether_dir = paths::state_dir();
        if (tether_dir.empty())
            tether_dir = "/tmp/tether";
        std::filesystem::create_directories(tether_dir);
        return tether_dir.string();
    }

    void ensure_single_instance() {
        std::string lock_file = get_runtime_dir() + "/tetherd.lock";
        int fd = open(lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            throw std::system_error(errno, std::system_category(), "Failed to open lock file");
        }

        if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
            if (errno == EWOULDBLOCK) {
                debug::log(ERR, "tetherd is already running.");
                exit(1);
            } else {
                throw std::system_error(errno, std::system_category(), "Failed to lock file");
            }
        }
        // we intentionally leak the fd so the lock is held for the lifetime of the process.
        // lock is automatically released by OS on exit.
    }

    // --- UnixServer ---

    UnixServer::UnixServer(EpollEventLoop& loop, TcpServer& tcp_server) : loop_(loop), tcp_server_(tcp_server) {
        socket_path_ = get_runtime_dir() + "/tetherd.sock";
    }

    UnixServer::~UnixServer() { stop(); }

    bool UnixServer::start() {
        server_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (server_fd_ < 0) {
            debug::log(ERR, "Failed to create unix socket");
            return false;
        }

        // Set non-blocking
        int flags = fcntl(server_fd_, F_GETFL, 0);
        fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

        unlink(socket_path_.c_str());

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        // sun_path is 108 bytes. strncpy would silently truncate a longer path and we
        // would end up listening somewhere nobody is looking.
        if (socket_path_.size() >= sizeof(addr.sun_path)) {
            debug::log(ERR,
                       "Socket path too long ({} bytes, max {}): {}",
                       socket_path_.size(),
                       sizeof(addr.sun_path) - 1,
                       socket_path_);
            return false;
        }
        std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            debug::log(ERR, "Failed to bind unix socket: {}", std::strerror(errno));
            return false;
        }

        if (listen(server_fd_, SOMAXCONN) < 0) {
            debug::log(ERR, "Failed to listen on unix socket");
            return false;
        }

        loop_.addFd(server_fd_, [this](int fd) { handle_accept(fd); });
        debug::log(INFO, "UnixServer listening on {}", socket_path_);
        return true;
    }

    void UnixServer::stop() {
        if (server_fd_ >= 0) {
            loop_.removeFd(server_fd_);
            close(server_fd_);
            server_fd_ = -1;
            unlink(socket_path_.c_str());
        }

        // Clean up any remaining clients
        std::vector<int> client_fds;
        for (auto const& [fd, _] : client_buffers_) {
            client_fds.push_back(fd);
        }
        for (int fd : client_fds) {
            loop_.removeFd(fd);
            close(fd);
            unregister_client_fd(fd);
            client_buffers_.erase(fd);
        }
    }

    void UnixServer::handle_accept(int fd) {
        int client_fd = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                debug::log(ERR, "UnixServer accept error: {}", std::strerror(errno));
            }
            return;
        }

        // Set non-blocking
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        // Register client for broadcasts
        register_client_fd(client_fd);
        loop_.addFd(client_fd, [this](int cfd) { handle_client(cfd); });
        debug::log(INFO, "UnixServer: New connection (fd: {})", client_fd);
    }

    // Generous against the ~683 KB base64 payloads file transfers push through,
    // and far short of what an unframed stream would cost.
    static constexpr size_t MAX_CLIENT_BUFFER_BYTES = 16u * 1024 * 1024;

    void UnixServer::handle_client(int client_fd) {
        // 64k buffer sizes for chunks
        char buf[65536];
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n > 0) {
            std::string& buffer = client_buffers_[client_fd];
            buffer.append(buf, n);
            if (buffer.size() > MAX_CLIENT_BUFFER_BYTES) {
                debug::log(ERR, "UnixServer: client {} sent an unframed stream; dropping it", client_fd);
                client_buffers_.erase(client_fd);
                unregister_client_fd(client_fd);
                loop_.removeFd(client_fd);
                close(client_fd);
                return;
            }

            size_t start = 0;
            size_t pos;
            while ((pos = buffer.find('\n', start)) != std::string::npos) {
                std::string msg = buffer.substr(start, pos - start);
                start = pos + 1;

                try {
                    nlohmann::json j = nlohmann::json::parse(msg);

                    if (j.contains("command") && j["command"] == "subscribe") {
                        register_local_subscriber(client_fd);
                        write_plain_packet(client_fd, build_protocol_info().dump() + "\n");
                        std::string payload = build_local_state_snapshot().dump() + "\n";
                        write_plain_packet(client_fd, payload);

                        if (Otp otp = otp_peek(); otp && !otp_is_claimed()) {
                            std::string otp_payload = make_otp_event(otp).dump() + "\n";
                            write_plain_packet(client_fd, otp_payload);
                        }

                        write_plain_packet(client_fd, build_bt_connection_status().dump() + "\n");
                        continue;
                    } else if (j.contains("command") && j["command"] == "unsubscribe") {
                        unregister_local_subscriber(client_fd);
                        std::string payload = "{\"command\":\"unsubscribed\"}\n";
                        write_plain_packet(client_fd, payload);
                        continue;
                    } else if (j.contains("command") && j["command"] == "protocol_info") {
                        write_plain_packet(client_fd, build_protocol_info().dump() + "\n");
                        continue;
                    } else if (j.contains("command") && j["command"] == "state_snapshot") {
                        std::string payload = build_local_state_snapshot().dump() + "\n";
                        write_plain_packet(client_fd, payload);
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_status") {
                        std::string payload = build_bt_status().dump() + "\n";
                        write_plain_packet(client_fd, payload);
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_scan") {
                        std::thread(run_bt_scan).detach();
                    } else if (j.contains("command") && j["command"] == "bt_airpods") {
                        std::string payload = build_bt_airpods().dump() + "\n";
                        write_plain_packet(client_fd, payload);
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_airpods_enable" && j.contains("enabled")) {
                        auto config = bluetooth::load_config();
                        config.airpods_enabled = j.value("enabled", false);
                        bluetooth::save_config(config);
                        if (bluetooth::g_airpods)
                            bluetooth::g_airpods->set_enabled(config.airpods_enabled);
                        broadcast_local_event(build_bt_status().dump());
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_airpods_handoff" && j.contains("enabled")) {
                        auto config = bluetooth::load_config();
                        config.airpods_handoff = j.value("enabled", false);
                        bluetooth::save_config(config);
                        broadcast_local_event(build_bt_status().dump());
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_airpods_pause" && j.contains("mode")) {
                        auto config = bluetooth::load_config();
                        config.airpods_pause = bluetooth::pause_mode_from_string(j["mode"]);
                        bluetooth::save_config(config);
                        broadcast_local_event(build_bt_status().dump());
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_airpods_mode" && j.contains("mode")) {
                        const auto mode = bluetooth::anc_mode_from_string(j.value("mode", ""));
                        nlohmann::json reply;
                        reply["command"] = "bt_airpods_mode_result";
                        if (!mode) {
                            reply["success"] = false;
                            reply["message"] = _("Unknown listening mode.");
                        } else if (!bluetooth::g_airpods || bluetooth::g_airpods->state().address.empty()) {
                            reply["success"] = false;
                            reply["message"] = _("No AirPods are connected.");
                        } else {
                            bluetooth::g_airpods->set_anc(*mode);
                            reply["success"] = true;
                        }
                        write_plain_packet(client_fd, reply.dump() + "\n");
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_list_devices") {
                        std::string payload = build_bt_devices().dump() + "\n";
                        write_plain_packet(client_fd, payload);
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_pair" && j.contains("address")) {
                        // Pairing waits on the user and the phone, so it runs on
                        // its own thread; progress and the result arrive as events.
                        std::string address = j["address"];
                        std::optional<bluetooth::AuthStrategy> strategy;
                        if (j.contains("strategy"))
                            strategy = bluetooth::auth_strategy_from_string(j["strategy"]);
                        const std::string operation_id = j.value("operation_id", std::string{});
                        std::thread([address, strategy, operation_id]() {
                            run_bt_pair(address, strategy, operation_id);
                        }).detach();
                    } else if (j.contains("command") && j["command"] == "bt_pair_confirm") {
                        bool matched = false;
                        {
                            std::lock_guard<std::mutex> lock(g_bt_confirm_mutex);
                            const std::string operation_id = j.value("operation_id", std::string{});
                            matched = g_bt_confirm_operation_id.empty() || operation_id == g_bt_confirm_operation_id;
                            if (matched)
                                g_bt_confirm_answer = j.value("accept", false) ? 1 : 0;
                        }
                        if (matched)
                            g_bt_confirm_cv.notify_all();
                    } else if (j.contains("command") && j["command"] == "bt_unpair" && j.contains("address")) {
                        std::string address = j["address"];
                        const std::string operation_id = j.value("operation_id", std::string{});
                        std::thread([address, operation_id]() { run_bt_unpair(address, operation_id); }).detach();
                    } else if (j.contains("command") && j["command"] == "bt_airpods_connect" && j.contains("address")) {
                        std::string address = j["address"];
                        const bool connect = j.value("connect", true);
                        std::thread([address, connect]() { run_bt_airpods_connect(address, connect); }).detach();
                    } else if (j.contains("command") && j["command"] == "bt_set_device" && j.contains("address")) {
                        auto config = bluetooth::load_config();
                        config.device_address = j["address"];
                        bluetooth::save_config(config);
                        std::thread(restart_supervision, config).detach();
                    } else if (j.contains("command") && j["command"] == "bt_list_threads") {
                        std::string payload = build_bt_threads().dump() + "\n";
                        write_plain_packet(client_fd, payload);
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_list_contacts") {
                        std::string payload =
                            build_bt_contacts(j.value("query", ""), j.value("limit", BT_CONTACTS_DEFAULT_LIMIT))
                                .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) +
                            "\n";
                        write_plain_packet(client_fd, payload);
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_list_messages" && j.contains("thread")) {
                        std::string payload = build_bt_messages(j["thread"]).dump() + "\n";
                        write_plain_packet(client_fd, payload);
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_mark_read" &&
                               (j.contains("handle") || j.contains("handles"))) {
                        std::vector<std::string> handles;
                        if (j.contains("handles") && j["handles"].is_array()) {
                            for (const auto& h : j["handles"])
                                if (h.is_string())
                                    handles.push_back(h.get<std::string>());
                        } else if (j["handle"].is_string()) {
                            handles.push_back(j["handle"].get<std::string>());
                        }
                        bluetooth::mark_messages_read_async(std::move(handles), j.value("read", true));
                    } else if (j.contains("command") && j["command"] == "bt_send_message" && j.contains("thread") &&
                               j.contains("body")) {
                        std::string thread = j["thread"];
                        std::string body = j["body"];
                        const std::string operation_id = j.value("operation_id", std::string{});
                        // PushMessage is a blocking OBEX transfer, so it cannot
                        // run on the loop that has to keep serving the UI.
                        std::thread([thread, body, operation_id]() {
                            std::string err;
                            bluetooth::Message sent;
                            nlohmann::json event;
                            event["command"] = "bt_send_result";
                            event["thread"] = thread;
                            set_operation_id(event, operation_id);
                            event["success"] = bluetooth::send_message(thread, body, sent, err);
                            if (event["success"]) {
                                nlohmann::json message = bluetooth::to_json(sent);
                                message["command"] = "bt_message";
                                broadcast_local_event(message.dump());
                            } else {
                                event["message"] = err;
                            }
                            broadcast_local_event(event.dump());
                        }).detach();
                    } else if (j.contains("command") && j["command"] == "bt_solicit") {
                        std::thread([]() {
                            nlohmann::json event;
                            event["command"] = "bt_solicit_result";

                            // The advert has two jobs: soliciting ANCS, and making iOS
                            // reveal the Messages and Contacts toggles at all. A live
                            // ANCS session says nothing about whether MAP's permission
                            // has been granted, so refuse only when every profile the
                            // advert could still surface is already open.
                            const nlohmann::json live = build_bt_connection_status();
                            if (live.value("ancs_ready", false) && live.value("map_open", false) &&
                                live.value("pbap_open", false)) {
                                event["success"] = true;
                                event["message"] = _("Notification mirroring is already active; nothing to do.");
                                broadcast_local_event(event.dump());
                                return;
                            }

                            std::string err;
                            event["success"] = bluetooth::g_bluez && bluetooth::solicit_ancs(*bluetooth::g_bluez, err);
                            event["message"] =
                                event["success"]
                                    // TRANSLATORS: "Show Message Notifications" and the Settings path are
                                    // the iPhone's own wording; use Apple's for the target language.
                                    ? _("Asked the iPhone to re-offer notification access. Watch the connection "
                                        "status. If it does not go active within a minute or two, toggle Show "
                                        "Message Notifications off and on in Settings > Bluetooth > (i) -- it can "
                                        "need re-granting even when it already looks enabled.")
                                    : (err.empty() ? _("Bluetooth is unavailable.") : err);
                            broadcast_local_event(event.dump());
                        }).detach();
                    } else if (j.contains("command") && j["command"] == "bt_set_ancs") {
                        auto config = bluetooth::load_config();
                        config.ancs_enabled = j.value("enabled", true);
                        bluetooth::save_config(config);

                        if (bluetooth::g_bt_connections)
                            bluetooth::g_bt_connections->set_ancs_enabled(config.ancs_enabled);

                        bluetooth::set_group_replies_enabled(config.group_messages_enabled &&
                                                             config.ancs_content_enabled && config.ancs_enabled);
                        broadcast_local_event(build_bt_status().dump());
                    } else if (j.contains("command") && j["command"] == "bt_set_adapter") {
                        auto config = bluetooth::load_config();
                        config.adapter = j.value("adapter", "");
                        bluetooth::save_config(config);
                        if (bluetooth::g_bluez)
                            bluetooth::g_bluez->set_preferred_adapter(config.adapter);
                        std::thread(restart_supervision, config).detach();
                    } else if (j.contains("command") && j["command"] == "bt_set_enabled") {
                        auto config = bluetooth::load_config();
                        config.enabled = j.value("enabled", true);
                        bluetooth::save_config(config);
                        std::thread(restart_supervision, config).detach();
                    } else if (j.contains("command") && j["command"] == "bt_set_ancs_content") {
                        auto config = bluetooth::load_config();
                        config.ancs_content_enabled = j.value("enabled", true);
                        bluetooth::save_config(config);
                        if (bluetooth::g_bt_connections)
                            bluetooth::g_bt_connections->set_ancs_content_enabled(config.ancs_content_enabled);
                        // Group correlation reads notification bodies, so it
                        // follows this toggle.
                        bluetooth::set_group_replies_enabled(config.group_messages_enabled &&
                                                             config.ancs_content_enabled && config.ancs_enabled);
                        broadcast_local_event(build_bt_status().dump());
                    } else if (j.contains("command") && j["command"] == "bt_set_calls") {
                        auto config = bluetooth::load_config();
                        config.calls_enabled = j.value("enabled", false);
                        bluetooth::save_config(config);
                        if (bluetooth::g_bt_connections)
                            bluetooth::g_bt_connections->set_calls_enabled(config.calls_enabled);
                        broadcast_local_event(build_bt_status().dump());
                    } else if (j.contains("command") && j["command"] == "bt_set_lock_on_away") {
                        auto config = bluetooth::load_config();
                        config.lock_on_away = j.value("enabled", false);
                        config.lock_away_seconds =
                            std::max(1, j.value("grace_seconds", bluetooth::AWAY_LOCK_GRACE_SECONDS));
                        bluetooth::save_config(config);
                        if (bluetooth::g_bt_connections)
                            bluetooth::g_bt_connections->set_lock_on_away(config.lock_on_away,
                                                                          config.lock_away_seconds);
                        broadcast_local_event(build_bt_status().dump());
                    } else if (j.contains("command") && j["command"] == "set_desktop_popups") {
                        auto config = bluetooth::load_config();
                        config.desktop_popups_enabled = j.value("enabled", true);
                        bluetooth::save_config(config);
                        set_desktop_popups_enabled(config.desktop_popups_enabled);
                        broadcast_local_event(build_bt_status().dump());
                    } else if (j.contains("command") && j["command"] == "bt_list_calls") {
                        nlohmann::json payload;
                        payload["command"] = "bt_calls";
                        payload["calls"] = bluetooth::g_bt_connections ? bluetooth::g_bt_connections->calls()
                                                                       : nlohmann::json::array();
                        std::string out = payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
                        write_plain_packet(client_fd, out);
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_call_dial" && j.contains("number")) {
                        std::string err = _("Bluetooth is unavailable.");
                        nlohmann::json payload;
                        payload["command"] = "bt_call_result";
                        payload["action"] = "dial";
                        payload["success"] = bluetooth::g_bt_connections &&
                                             bluetooth::g_bt_connections->dial(j.value("number", std::string{}), err);
                        if (!payload["success"])
                            payload["message"] = err;
                        broadcast_local_event(payload.dump());
                        write_plain_packet(client_fd, payload.dump() + "\n");
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_call_action" && j.contains("action")) {
                        const std::string action = j.value("action", std::string{});
                        std::string err = _("Bluetooth is unavailable.");
                        nlohmann::json payload;
                        payload["command"] = "bt_call_result";
                        payload["action"] = action;
                        payload["success"] =
                            bluetooth::g_bt_connections &&
                            bluetooth::g_bt_connections->call_action(j.value("path", std::string{}), action, err);
                        if (!payload["success"])
                            payload["message"] = err;
                        broadcast_local_event(payload.dump());
                        write_plain_packet(client_fd, payload.dump() + "\n");
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_call_tones" && j.contains("tones")) {
                        std::string err = _("Bluetooth is unavailable.");
                        nlohmann::json payload;
                        payload["command"] = "bt_call_result";
                        payload["action"] = "tones";
                        payload["success"] = bluetooth::g_bt_connections && bluetooth::g_bt_connections->call_tones(
                                                                                j.value("tones", std::string{}), err);
                        if (!payload["success"])
                            payload["message"] = err;
                        broadcast_local_event(payload.dump());
                        write_plain_packet(client_fd, payload.dump() + "\n");
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_set_retention" && j.contains("retention")) {
                        auto config = bluetooth::load_config();
                        const Retention previous = config.retention;
                        config.retention = retention_from_string(j.value("retention", "encrypted"));
                        if (config.retention != previous) {
                            bluetooth::save_config(config);
                            secret::set_retention(config.retention);
                            // Move what is already stored
                            if (config.retention == Retention::None) {
                                bluetooth::discard_retained_messages();
                                std::error_code ec;
                                for (auto mode : {Retention::Encrypted, Retention::Plaintext}) {
                                    std::filesystem::remove(bluetooth::journal_path(mode), ec);
                                    std::filesystem::remove(bluetooth::contacts_path(mode), ec);
                                }
                            } else {
                                bluetooth::move_retained_store(previous, config.retention);
                            }
                        }
                        broadcast_local_event(build_bt_status().dump());
                    } else if (j.contains("command") && j["command"] == "bt_list_notifications") {
                        nlohmann::json payload;
                        payload["command"] = "bt_notifications";
                        payload["notifications"] = bluetooth::g_bt_connections
                                                       ? bluetooth::g_bt_connections->notifications()
                                                       : nlohmann::json::array();
                        std::string out = payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
                        write_plain_packet(client_fd, out);
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_notification_action" && j.contains("uid")) {
                        // ANCS offers positive and negative only; there is no
                        // free-text reply to expose here.
                        const bool positive = j.value("action", "positive") == "positive";
                        nlohmann::json payload;
                        payload["command"] = "bt_notification_action_result";
                        payload["uid"] = j["uid"];
                        payload["success"] =
                            bluetooth::g_bt_connections &&
                            bluetooth::g_bt_connections->perform_notification_action(
                                j["uid"].get<uint32_t>(),
                                positive ? bluetooth::ancs::ActionId::Positive : bluetooth::ancs::ActionId::Negative);
                        broadcast_local_event(payload.dump());
                    } else if (j.contains("command") && j["command"] == "bt_connection") {
                        std::string out = build_bt_connection_status().dump() + "\n";
                        write_plain_packet(client_fd, out);
                        continue;
                    } else if (j.contains("command") && j["command"] == "bt_diagnostics") {
                        nlohmann::json connection = build_bt_connection_status();
                        std::string out = bluetooth::build_diagnostics(build_bt_status(), connection).dump() + "\n";
                        write_plain_packet(client_fd, out);
                        continue;
                    } else if (j.contains("command") && j["command"] == "clipboard_set" && j.contains("content")) {
                        std::string content = j["content"];
                        if (g_wayland)
                            g_wayland->copy_to_clipboard(content);
                    } else if (j.contains("command") && j["command"] == "clipboard_get") {
                        if (g_wayland) {
                            nlohmann::json resp;
                            resp["command"] = "clipboard_content";
                            resp["content"] = g_wayland->get_clipboard();
                            std::string payload = resp.dump() + "\n";
                            write_plain_packet(client_fd, payload);
                            continue;
                        }
                    } else if (j.contains("command") && j["command"] == "clipboard_send") {
                        const std::string image = g_wayland ? g_wayland->get_clipboard_image() : std::string{};
                        if (!image.empty())
                            broadcast_clipboard_image(image);
                        nlohmann::json resp;
                        resp["command"] = "clipboard_content";
                        resp["content"] = g_wayland ? g_wayland->get_clipboard() : std::string{};
                        // with an image selected the cached text is stale don't push it
                        if (image.empty() && !resp["content"].get_ref<const std::string&>().empty()) {
                            nlohmann::json bc;
                            bc["command"] = "clipboard_updated";
                            bc["content"] = resp["content"];
                            broadcast_message(bc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
                        }
                        write_plain_packet(client_fd,
                                           resp.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
                        continue;
                    } else if (j.contains("command") && j["command"] == "new_otp" && j.contains("otp")) {
                        otp_publish(otp_from_json(j["otp"]), j.value("sender_domain", std::string{}), client_fd);
                        std::string payload = "{\"status\":\"ok\"}\n";
                        write_plain_packet(client_fd, payload);
                        continue;
                    } else if (j.contains("command") && j["command"] == "request_otp") {
                        // Claimed by the first site that polls for it, so the next site
                        // the user visits doesn't get handed the previous site's code.
                        Otp otp = otp_take_for_host(j.value("url", std::string{}));
                        std::string payload = make_otp_event(otp).dump() + "\n";
                        write_plain_packet(client_fd, payload);
                        continue;
                    } else if (j.contains("command") && j["command"] == "consume_otp") {
                        // A page filled the code; retire it so it isn't re-served. Scoped
                        // to the id so a slow consume from the previous page can't wipe
                        // the code that just arrived for the current one.
                        otp_consume(j.value("otp_id", static_cast<uint64_t>(0)));
                        std::string payload = "{\"status\":\"ok\"}\n";
                        write_plain_packet(client_fd, payload);
                        continue;
                    } else if (j.contains("command") && j["command"] == "file_start") {
                        if (broadcast_tcp_message(msg) == 0) {
                            nlohmann::json resp;
                            resp["command"] = "error";
                            resp["message"] = "no_connected_mobile_client";
                            std::string payload = resp.dump() + "\n";
                            write_plain_packet(client_fd, payload);
                            continue;
                        }
                    } else if (j.contains("command") && j["command"] == "file_chunk") {
                        if (broadcast_tcp_message(msg) == 0) {
                            nlohmann::json resp;
                            resp["command"] = "error";
                            resp["message"] = "no_connected_mobile_client";
                            std::string payload = resp.dump() + "\n";
                            write_plain_packet(client_fd, payload);
                            continue;
                        }
                    } else if (j.contains("command") && j["command"] == "file_end") {
                        if (broadcast_tcp_message(msg) == 0) {
                            nlohmann::json resp;
                            resp["command"] = "error";
                            resp["message"] = "no_connected_mobile_client";
                            std::string payload = resp.dump() + "\n";
                            write_plain_packet(client_fd, payload);
                            continue;
                        }

                        nlohmann::json resp;
                        resp["command"] = "file_status";
                        resp["transfer_id"] = j["transfer_id"];
                        resp["status"] = "success";
                        std::string payload = resp.dump() + "\n";
                        write_plain_packet(client_fd, payload);
                        continue;
                    } else if (j.contains("command") && j["command"] == "accept_device" && j.contains("fingerprint")) {
                        const std::string print = j.value("fingerprint", "");
                        const std::string fallback_name = j.value("device_name", "Paired Device");
                        const bool connected = !print.empty() && tcp_server_.accept_device(print, fallback_name);
                        nlohmann::json response{{"command", "accept_device_result"},
                                                {"accepted", !print.empty()},
                                                {"connected", connected}};
                        write_plain_packet(client_fd, response.dump() + "\n");
                        continue;
                    } else if (j.contains("command") && j["command"] == "forget_device" && j.contains("fingerprint")) {
                        const std::string print = j.value("fingerprint", "");
                        const bool forgotten = tcp_server_.forget_device(print);
                        nlohmann::json response{
                            {"command", "forget_device_result"}, {"fingerprint", print}, {"forgotten", forgotten}};
                        write_plain_packet(client_fd, response.dump() + "\n");
                        broadcast_local_event(response.dump());
                        continue;
                    } else if (j.contains("command") && j["command"] == "pair_request" && j.contains("host")) {
                        const std::string target_host = j["host"];
                        const int target_port = j.value("port", 5134);
                        const std::string peer_name = j.value("device_name", "");
                        tcp_server_.connect_peer(target_host, target_port, peer_name);
                    } else if (j.contains("command") && j["command"] == "discover") {
                        std::thread([]() {
                            Discovery discovery;
                            auto hosts = discovery.discover(3000);
                            auto grouped = group_discovered_hosts(hosts, Crypto::instance().get_my_fingerprint());

                            nlohmann::json payload;
                            payload["command"] = "discovery_result";
                            payload["devices"] = nlohmann::json::array();
                            for (const auto& dev : grouped) {
                                nlohmann::json d;
                                d["name"] = dev.name;
                                d["fingerprint"] = dev.fingerprint;
                                d["addresses"] = nlohmann::json::array();
                                for (const auto& addr : dev.addresses) {
                                    nlohmann::json a;
                                    a["address"] = addr.address;
                                    a["port"] = addr.port;
                                    d["addresses"].push_back(a);
                                }
                                payload["devices"].push_back(d);
                            }
                            set_discovered_devices(payload["devices"]);
                            broadcast_local_event(payload.dump());
                        }).detach();
                    } else if (j.contains("command") && j["command"] == "send_file" && j.contains("path")) {
                        std::string path = j["path"];
                        const std::string operation_id = j.value("operation_id", std::string{});
                        std::thread([path, operation_id]() {
                            nlohmann::json resp;
                            resp["command"] = "file_send_complete";
                            set_operation_id(resp, operation_id);
                            Client local;
                            if (local.connect("", 0)) { // connects correctly via unix socket
                                std::string err;
                                bool ok = local.send_file(path, err);
                                resp["success"] = ok;
                                resp["message"] =
                                    ok ? tr_format(_("Sent {}"), std::filesystem::path(path).filename().string())
                                       : tr_format(_("Send failed: {}"), err.empty() ? _("unknown error") : err);
                            } else {
                                resp["success"] = false;
                                resp["message"] = tr_format(_("Send failed: {}"), _("Could not reach the Tether daemon."));
                            }
                            broadcast_local_event(resp.dump());
                        }).detach();
                        continue; // skip the "OK\n" below because we reply asynchronously
                    }
                } catch (...) {
                }

                std::string response = "OK\n";
                write_plain_packet(client_fd, response);
            }

            if (start > 0) {
                buffer.erase(0, start); // compact once per read, not once per message
            }
        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            // Disconnected
            debug::log(INFO, "UnixServer: Client disconnected (fd: {})", client_fd);
            client_buffers_.erase(client_fd);
            unregister_client_fd(client_fd);
            loop_.removeFd(client_fd);
            close(client_fd);
        }
    }

    // --- TcpServer ---

    TcpServer::TcpServer(EpollEventLoop& loop, int bind_port) : loop_(loop), bind_port_(bind_port) {}

    TcpServer::~TcpServer() { stop(); }

    // Binds and listens on the wildcard address of `family`. AF_INET6 socket dual-stack.
    static int bind_listen_socket(int family, int port) {
        // CLOEXEC: the pairing dialog is fork+exec'd from this process and must
        // not inherit the listening socket, or it keeps the port bound and
        // SO_REUSEPORT hands it connections it will never accept.
        int fd = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return -1;

        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
            debug::log(ERR, "TcpServer setsockopt reuse failed");
        }

        // Set non-blocking
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        sockaddr_storage ss{};
        socklen_t len = 0;
        if (family == AF_INET6) {
            int off = 0;
            if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) < 0) {
                int err = errno;
                close(fd);
                errno = err;
                return -1;
            }
            auto* a6 = reinterpret_cast<sockaddr_in6*>(&ss);
            a6->sin6_family = AF_INET6;
            a6->sin6_addr = in6addr_any;
            a6->sin6_port = htons(port);
            len = sizeof(*a6);
        } else {
            auto* a4 = reinterpret_cast<sockaddr_in*>(&ss);
            a4->sin_family = AF_INET;
            a4->sin_addr.s_addr = INADDR_ANY;
            a4->sin_port = htons(port);
            len = sizeof(*a4);
        }

        if (bind(fd, reinterpret_cast<sockaddr*>(&ss), len) < 0 || listen(fd, SOMAXCONN) < 0) {
            int err = errno;
            close(fd);
            errno = err;
            return -1;
        }

        return fd;
    }

    bool TcpServer::start() {
        bool dual_stack = true;
        server_fd_ = bind_listen_socket(AF_INET6, bind_port_);
        if (server_fd_ < 0) {
            dual_stack = false;
            server_fd_ = bind_listen_socket(AF_INET, bind_port_);
        }

        if (server_fd_ < 0) {
            debug::log(ERR, "Failed to listen on tcp port {}: {}", bind_port_, std::strerror(errno));
            return false;
        }

        loop_.addFd(server_fd_, [this](int fd) { handle_accept(fd); });
        debug::log(INFO, "TcpServer listening on {}:{}", dual_stack ? "[::]" : "0.0.0.0", bind_port_);
        return true;
    }

    void TcpServer::stop() {
        if (server_fd_ >= 0) {
            loop_.removeFd(server_fd_);
            close(server_fd_);
            server_fd_ = -1;
        }

        // Clean up all active clients and SSL sessions
        std::vector<int> client_fds;
        for (auto const& [fd, _] : active_ssl_) {
            client_fds.push_back(fd);
        }
        for (int fd : client_fds) {
            drop_client(fd);
        }
    }

    void TcpServer::drop_client(int fd) {
        auto ssl_it = active_ssl_.find(fd);
        if (ssl_it != active_ssl_.end()) {
            if (ssl_it->second)
                SSL_free(ssl_it->second);
            active_ssl_.erase(ssl_it);
        }

        client_buffers_.erase(fd);
        clipboard_images_.erase(fd);
        ssl_handshake_complete_.erase(fd);
        client_paired_.erase(fd);
        client_info_.erase(fd);
        client_initiated_.erase(fd);
        connected_remote_clients.erase(fd);

        unregister_client_fd(fd);
        loop_.removeFd(fd);
        close(fd);
    }

    // Text form of an accepted peer.
    static std::string peer_ip(const sockaddr_storage& ss, socklen_t len) {
        if (ss.ss_family == AF_INET6) {
            const auto* a6 = reinterpret_cast<const sockaddr_in6*>(&ss);
            if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
                char ip[INET_ADDRSTRLEN] = {};
                if (inet_ntop(AF_INET, &a6->sin6_addr.s6_addr[12], ip, sizeof(ip)))
                    return ip;
                return "unknown";
            }
        }

        char host[NI_MAXHOST] = {};
        if (getnameinfo(reinterpret_cast<const sockaddr*>(&ss), len, host, sizeof(host), nullptr, 0, NI_NUMERICHOST) !=
            0)
            return "unknown";
        return host;
    }

    void TcpServer::handle_accept(int fd) {
        sockaddr_storage client_addr{};
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept4(fd, reinterpret_cast<sockaddr*>(&client_addr), &addrlen, SOCK_CLOEXEC);
        if (client_fd < 0)
            return;

        // Set non-blocking
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        const std::string ip = peer_ip(client_addr, addrlen);
        debug::log(INFO, "TcpServer: New connection from {} (fd: {})", ip, client_fd);

        // SSL Wrapping
        SSL* ssl = SSL_new(tether::Crypto::instance().get_server_context());
        SSL_set_fd(ssl, client_fd);
        SSL_set_accept_state(ssl);

        active_ssl_[client_fd] = ssl;
        ssl_handshake_complete_[client_fd] = false;
        client_paired_[client_fd] = false;
        client_info_[client_fd].address = ip;
        connected_remote_clients[client_fd].address = ip;

        // We do NOT register for broadcasts until SSL handshake is complete
        loop_.addFd(client_fd, [this](int cfd) { handle_client(cfd); });
    }

    // How long a dial waits before calling the peer unreachable.
    constexpr int DIAL_TIMEOUT_SECONDS = 5;

    bool should_dial_peer(const std::string& my_fingerprint, const std::string& peer_fingerprint, bool peer_is_known) {
        if (my_fingerprint.empty() || peer_fingerprint.empty())
            return false;
        if (!peer_is_known)
            return false;
        return my_fingerprint < peer_fingerprint;
    }

    // A dial is identified by the peer's fingerprint when mDNS gave us one, and by address otherwise.
    static std::string dial_key_for(const std::string& host, const std::string& fingerprint) {
        return fingerprint.empty() ? host : fingerprint;
    }

    bool TcpServer::connect_peer(const std::string& host,
                                 int port,
                                 const std::string& peer_name,
                                 const std::string& expected_fingerprint) {
        if (host.empty())
            return false;

        const bool by_fingerprint = !expected_fingerprint.empty();
        for (auto const& [fd, remote] : connected_remote_clients) {
            (void)fd;
            const bool match = by_fingerprint ? remote.fingerprint == expected_fingerprint : remote.address == host;
            if (match) {
                debug::log(INFO, "TcpServer: Already holding a session with {}", host);
                return true;
            }
        }

        const std::string dial_key = dial_key_for(host, expected_fingerprint);
        if (!dialing_.insert(dial_key).second) {
            debug::log(INFO, "TcpServer: Dial to {} already in flight", host);
            return true;
        }

        // connect() blocks, and the loop watches EPOLLIN only
        std::thread([this, host, port, peer_name, expected_fingerprint, dial_key]() {
            // adopt_peer clears it on the success path, once the fd is in the client tables.
            auto give_up = [&] { loop_.post([this, dial_key]() { dialing_.erase(dial_key); }); };

            auto fail = [&](const char* reason) {
                give_up();
                nlohmann::json event{{"command", "pair_rejected"},
                                     {"fingerprint", ""},
                                     {"device_name", peer_name},
                                     {"address", host},
                                     {"reason", reason}};
                broadcast_local_event(event.dump());
            };

            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_NUMERICSERV;

            addrinfo* results = nullptr;
            const std::string service = std::to_string(port);
            if (int err = getaddrinfo(host.c_str(), service.c_str(), &hints, &results); err != 0 || !results) {
                debug::log(ERR, "TcpServer: Cannot resolve {}: {}", host, gai_strerror(err));
                fail("unresolved");
                return;
            }

            int sock = -1;
            int connect_errno = 0;
            for (addrinfo* ai = results; ai; ai = ai->ai_next) {
                sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                if (sock < 0) {
                    connect_errno = errno;
                    continue;
                }

                timeval timeout{DIAL_TIMEOUT_SECONDS, 0};
                setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                if (::connect(sock, ai->ai_addr, ai->ai_addrlen) == 0)
                    break;
                connect_errno = errno;
                close(sock);
                sock = -1;
            }
            freeaddrinfo(results);

            if (sock < 0) {
                debug::log(ERR, "TcpServer: Failed to dial {}:{}: {}", host, port, std::strerror(connect_errno));
                switch (connect_errno) {
                case ECONNREFUSED:
                    fail("refused");
                    break;
                case EINPROGRESS:
                case EAGAIN:
                case ETIMEDOUT:
                case EHOSTUNREACH:
                case ENETUNREACH:
                    fail("unreachable");
                    break;
                default:
                    fail("failed");
                    break;
                }
                return;
            }

            timeval none{0, 0};
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof(none));

            loop_.post([this, sock, host, peer_name, expected_fingerprint]() {
                adopt_peer(sock, host, peer_name, expected_fingerprint);
            });
        }).detach();

        return true;
    }

    void TcpServer::adopt_peer(int fd,
                               const std::string& host,
                               const std::string& peer_name,
                               const std::string& expected_fingerprint) {
        dialing_.erase(dial_key_for(host, expected_fingerprint));

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        debug::log(INFO, "TcpServer: Dialled {} (fd: {})", host, fd);

        SSL* ssl = SSL_new(tether::Crypto::instance().get_client_context());
        SSL_set_fd(ssl, fd);
        SSL_set_connect_state(ssl);

        active_ssl_[fd] = ssl;
        ssl_handshake_complete_[fd] = false;
        client_paired_[fd] = false;
        client_initiated_[fd] = true;
        client_info_[fd].address = host;
        client_info_[fd].device_name = peer_name;
        connected_remote_clients[fd].address = host;
        connected_remote_clients[fd].device_name = peer_name;
        connected_remote_clients[fd].fingerprint = expected_fingerprint;

        loop_.addFd(fd, [this](int cfd) { handle_client(cfd); });

        // Drive the ClientHello out; the peer's reply then arrives as EPOLLIN.
        handle_client(fd);
    }

    void TcpServer::handle_client(int client_fd) {
        SSL* ssl = active_ssl_[client_fd];

        if (!ssl_handshake_complete_[client_fd]) {
            // OpenSSL keeps a per-thread error queue. Clear it before each new
            // handshake attempt so the diagnostics below reflect only this socket's
            // failure, not stale errors from earlier operations.
            ERR_clear_error();
            const bool initiated = client_initiated_[client_fd];
            int ret = initiated ? SSL_connect(ssl) : SSL_accept(ssl);
            if (ret == 1) {
                ssl_handshake_complete_[client_fd] = true;
                std::string print = Crypto::get_peer_fingerprint(ssl);
                client_info_[client_fd].fingerprint = print;
                connected_remote_clients[client_fd].fingerprint = print;
                if (Crypto::instance().is_host_known(print)) {
                    std::string device_name = lookup_known_host_name(print);
                    if (device_name.empty())
                        device_name = client_info_[client_fd].device_name;
                    promote_session(client_fd, print, device_name);
                } else if (initiated) {
                    // We dialled, so we are the one asking. The peer prompts its user
                    // and answers pair_accepted; we pin nothing until it does.
                    char hostname[256] = {};
                    gethostname(hostname, sizeof(hostname) - 1);

                    nlohmann::json request{{"command", "pair_request"}, {"device_name", hostname}};
                    const std::string payload = request.dump() + "\n";
                    robust_ssl_write(ssl, payload.c_str(), payload.size());

                    debug::log(INFO, "TcpServer: Pair request sent to {} ({})", client_info_[client_fd].address, print);
                } else {
                    debug::log(INFO, "TcpServer: Untrusted client connected. Fingerprint: {}", print);
                    nlohmann::json event;
                    event["command"] = "untrusted_client_connected";
                    event["address"] = client_info_[client_fd].address;
                    event["fingerprint"] = print;
                    event["device_name"] = "Unknown Device";
                    broadcast_local_event(event.dump());
                }
            } else {
                const int ssl_err = errno;
                int err = SSL_get_error(ssl, ret);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    return; // Wait for Epoll to re-trigger
                }

                // Handshake strictly failed
                std::string reason;
                while (unsigned long e = ERR_get_error()) {
                    char ebuf[256];
                    ERR_error_string_n(e, ebuf, sizeof(ebuf));
                    if (!reason.empty())
                        reason += "; ";
                    reason += ebuf;
                }
                ERR_clear_error();
                if (reason.empty()) {
                    if (err == SSL_ERROR_ZERO_RETURN || (err == SSL_ERROR_SYSCALL && ret == 0)) {
                        reason = "peer closed connection during handshake";
                    } else if (err == SSL_ERROR_SYSCALL) {
                        reason = (ssl_err != 0) ? std::string("syscall error: ") + std::strerror(ssl_err)
                                                : "ssl syscall failed";
                    } else {
                        reason = "ssl error " + std::to_string(err);
                    }
                }
                debug::log(ERR,
                           "TcpServer: TLS handshake failed for {} (fd: {}): {}",
                           client_info_[client_fd].address,
                           client_fd,
                           reason);

                drop_client(client_fd);
                return;
            }
        }

        char buf[65536];
        ERR_clear_error();
        int n = SSL_read(ssl, buf, sizeof(buf));
        if (n > 0) {

            std::string& buffer = client_buffers_[client_fd];

            buffer.append(buf, n);
            if (buffer.size() > MAX_CLIENT_BUFFER_BYTES) {
                debug::log(ERR, "TcpServer: client {} sent an unframed stream; dropping it", client_fd);
                drop_client(client_fd);
                return;
            }

            size_t start = 0;
            size_t pos;
            while ((pos = buffer.find('\n', start)) != std::string::npos) {
                std::string msg = buffer.substr(start, pos - start);
                start = pos + 1;

                try {
                    nlohmann::json j = nlohmann::json::parse(msg);

                    if (!client_paired_[client_fd] && client_initiated_[client_fd]) {
                        // Our own pair_request is outstanding. The peer's user is the
                        // approver, so only the peer's verdict pins anything here.
                        const std::string command = j.value("command", "");
                        const std::string print = Crypto::get_peer_fingerprint(ssl);

                        if (command == "pair_accepted") {
                            std::string dev_name = client_info_[client_fd].device_name;
                            if (dev_name.empty())
                                dev_name = client_info_[client_fd].address;

                            Crypto::instance().add_known_host(dev_name, print);
                            promote_session(client_fd, print, dev_name);

                            debug::log(INFO, "[Pairing Accepted] by {} ({})", dev_name, print);

                            nlohmann::json event{{"command", "pair_accepted"},
                                                 {"fingerprint", print},
                                                 {"device_name", dev_name},
                                                 {"address", client_info_[client_fd].address},
                                                 {"connected", true}};
                            broadcast_local_event(event.dump());
                        } else if (command == "pair_pending") {
                            const std::string peer = j.value("device_name", client_info_[client_fd].address);
                            client_info_[client_fd].device_name = peer;
                            connected_remote_clients[client_fd].device_name = peer;

                            nlohmann::json event{{"command", "pair_outbound_pending"},
                                                 {"fingerprint", print},
                                                 {"device_name", peer},
                                                 {"address", client_info_[client_fd].address}};
                            broadcast_local_event(event.dump());
                        } else {
                            debug::log(INFO,
                                       "TcpServer: {} refused the pair request ({})",
                                       client_info_[client_fd].address,
                                       command.empty() ? "no command" : command);
                            nlohmann::json event{{"command", "pair_rejected"},
                                                 {"fingerprint", print},
                                                 {"device_name", client_info_[client_fd].device_name},
                                                 {"address", client_info_[client_fd].address}};
                            broadcast_local_event(event.dump());
                            drop_client(client_fd);
                            return;
                        }
                        continue;
                    }

                    if (!client_paired_[client_fd]) {
                        if (j.contains("command") && j["command"] == "pair_request") {
                            std::string print = Crypto::get_peer_fingerprint(ssl);
                            std::string dev_name = j.value("device_name", "Unknown Device");
                            client_info_[client_fd].device_name = dev_name;
                            connected_remote_clients[client_fd].device_name = dev_name;
                            debug::log(INFO,
                                       "[Pairing Request Pending] from {}. Accept by running: tether --accept {}",
                                       dev_name,
                                       print);

                            // Persist the pending request so accept_device can retrieve the name
                            {
                                auto pending = load_pending_pairs();
                                pending[print] = {{"name", dev_name}, {"ts", static_cast<int64_t>(std::time(nullptr))}};
                                save_pending_pairs(pending);
                            }

                            char hostname[256] = {};
                            gethostname(hostname, sizeof(hostname) - 1);

                            nlohmann::json resp;
                            resp["command"] = "pair_pending";
                            resp["device_name"] = hostname;
                            std::string payload = resp.dump() + "\n";
                            robust_ssl_write(ssl, payload.c_str(), payload.size());

                            nlohmann::json event;
                            event["command"] = "pair_request_received";
                            event["fingerprint"] = print;
                            event["device_name"] = dev_name;
                            event["address"] = client_info_[client_fd].address;
                            broadcast_local_event(event.dump());

                            // Launch the layer-shell dialog for interactive approval
                            spawn_pair_dialog(client_fd, ssl, print, dev_name);
                        } else {
                            std::string rej = "{\"command\":\"error\",\"message\":\"unauthorized\"}\n";
                            robust_ssl_write(ssl, rej.c_str(), rej.size());
                        }
                        continue;
                    }

                    if (j.contains("command") && j["command"] == "clipboard_set" && j.contains("content")) {
                        std::string content = j["content"];
                        if (g_wayland)
                            g_wayland->copy_to_clipboard(content);
                        // Broadcast to everyone (including sender) to ensure robust transport
                        nlohmann::json bc;
                        bc["command"] = "clipboard_updated";
                        bc["content"] = content;
                        broadcast_message(bc.dump(), client_fd);
                    } else if (j.contains("command") && j["command"] == "hello") {
                        // feature negotiation. old daemons answer "OK"
                        const auto features = j.value("features", nlohmann::json::array());
                        if (features.is_array() &&
                            std::find(features.begin(), features.end(), "clipboard_image") != features.end())
                            set_session_clipboard_images(client_fd);
                        nlohmann::json resp{{"command", "hello"}};
                        resp["features"] = nlohmann::json::array({"clipboard_image"});
                        std::string payload = resp.dump() + "\n";
                        robust_ssl_write(ssl, payload.c_str(), payload.size());
                        continue;
                    } else if (j.contains("command") && j["command"] == "open_url" && j.contains("content") &&
                               j["content"].is_string()) {
                        open_web_url(j["content"].get<std::string>());
                    } else if (j.contains("command") && j["command"] == "clipboard_get") {
                        if (g_wayland) {
                            std::vector<std::string> image;
                            if (session_accepts_clipboard_images(client_fd))
                                image = clipboard_image_messages(g_wayland->get_clipboard_image(), "content");
                            if (!image.empty()) {
                                for (auto& m : image) {
                                    m += "\n";
                                    robust_ssl_write(ssl, m.c_str(), m.size());
                                }
                                continue;
                            }
                            nlohmann::json resp;
                            resp["command"] = "clipboard_content";
                            resp["content"] = g_wayland->get_clipboard();
                            std::string payload = resp.dump() + "\n";
                            robust_ssl_write(ssl, payload.c_str(), payload.size());
                            continue;
                        }
                    } else if (j.contains("command") && j["command"] == "new_otp" && j.contains("otp")) {
                        // OTP sent from a mobile client (iPhone Share Extension) over mTLS.
                        otp_publish(otp_from_json(j["otp"]), j.value("sender_domain", std::string{}));
                        std::string payload = "{\"status\":\"ok\"}\n";
                        robust_ssl_write(ssl, payload.c_str(), payload.size());
                        continue;
                    } else if (j.contains("command") && j["command"] == "file_start") {
                        // A clipboard image goes to the clipboard, not Downloads.
                        if (j.contains("clipboard"))
                            clipboard_images_[client_fd].start(j["transfer_id"], j["size"]);
                        else if (g_file_manager)
                            g_file_manager->handle_start(j["transfer_id"], j["filename"], j["size"]);
                    } else if (j.contains("command") && j["command"] == "file_chunk") {
                        if (!clipboard_images_[client_fd].chunk(j["transfer_id"], j["data"]) && g_file_manager)
                            g_file_manager->handle_chunk(j["transfer_id"], j["chunk_index"], j["data"]);
                    } else if (j.contains("command") && j["command"] == "file_end") {
                        if (std::string png = clipboard_images_[client_fd].finish(j["transfer_id"]); !png.empty()) {
                            if (g_wayland)
                                g_wayland->copy_image_to_clipboard(png);
                            broadcast_clipboard_image(png, client_fd);
                            nlohmann::json resp{{"command", "file_status"}, {"status", "success"}};
                            resp["transfer_id"] = j["transfer_id"];
                            std::string payload = resp.dump() + "\n";
                            robust_ssl_write(ssl, payload.c_str(), payload.size());
                            continue;
                        }
                        if (g_file_manager && g_file_manager->handle_end(j["transfer_id"])) {
                            nlohmann::json resp;
                            resp["command"] = "file_status";
                            resp["transfer_id"] = j["transfer_id"];
                            resp["status"] = "success";
                            std::string payload = resp.dump() + "\n";
                            robust_ssl_write(ssl, payload.c_str(), payload.size());
                            continue;
                        }
                    }
                } catch (...) {
                }

                std::string response = "OK\n";
                robust_ssl_write(ssl, response.c_str(), response.size());
            }

            if (start > 0) {
                buffer.erase(0, start);
            }
        } else {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                return;
            }

            debug::log(INFO, "TcpServer: Client disconnected (fd: {})", client_fd);
            auto it = connected_remote_clients.find(client_fd);
            if (it != connected_remote_clients.end()) {
                nlohmann::json event;
                event["command"] = "client_disconnected";
                event["address"] = it->second.address;
                event["fingerprint"] = it->second.fingerprint;
                event["device_name"] = it->second.device_name;
                event["paired"] = it->second.paired;
                broadcast_local_event(event.dump());
            }
            drop_client(client_fd);
        }
    }

    void TcpServer::promote_session(int client_fd, const std::string& fingerprint, const std::string& device_name) {
        auto ssl_it = active_ssl_.find(client_fd);
        auto paired_it = client_paired_.find(client_fd);
        auto info_it = client_info_.find(client_fd);
        auto remote_it = connected_remote_clients.find(client_fd);
        if (ssl_it == active_ssl_.end() || paired_it == client_paired_.end() || info_it == client_info_.end() ||
            remote_it == connected_remote_clients.end())
            return;

        paired_it->second = true;
        info_it->second.paired = true;
        info_it->second.device_name = device_name;
        remote_it->second.paired = true;
        remote_it->second.device_name = device_name;
        remote_it->second.fingerprint = fingerprint;

        // Only a trusted session joins the broadcast registry.
        register_client_ssl(client_fd, ssl_it->second);

        nlohmann::json connected_event{{"command", "client_connected"},
                                       {"fingerprint", fingerprint},
                                       {"device_name", device_name},
                                       {"address", remote_it->second.address},
                                       {"paired", true}};
        broadcast_local_event(connected_event.dump());
    }

    void TcpServer::set_peers_changed_callback(std::function<void()> callback) { peers_changed_ = std::move(callback); }

    bool TcpServer::forget_device(const std::string& fingerprint) {
        if (fingerprint.empty())
            return false;

        const bool removed = Crypto::instance().remove_known_host(fingerprint);

        std::vector<int> matching_fds;
        for (auto const& [client_fd, remote] : connected_remote_clients) {
            if (remote.fingerprint == fingerprint)
                matching_fds.push_back(client_fd);
        }

        for (int client_fd : matching_fds) {
            auto it = connected_remote_clients.find(client_fd);
            nlohmann::json event{{"command", "client_disconnected"},
                                 {"address", it->second.address},
                                 {"fingerprint", it->second.fingerprint},
                                 {"device_name", it->second.device_name},
                                 {"paired", it->second.paired}};
            broadcast_local_event(event.dump());
            drop_client(client_fd);
        }

        return removed;
    }

    bool TcpServer::accept_device(const std::string& fingerprint, const std::string& fallback_name) {
        if (fingerprint.empty())
            return false;

        auto pending = load_pending_pairs();
        std::string device_name = lookup_known_host_name(fingerprint);
        if (device_name.empty())
            device_name = fallback_name.empty() ? "Paired Device" : fallback_name;
        if (const std::string pending_name = pending_pair_name(pending, fingerprint); !pending_name.empty()) {
            device_name = pending_name;
            pending.erase(fingerprint);
            save_pending_pairs(pending);
        }

        Crypto::instance().add_known_host(device_name, fingerprint);

        bool promoted = false;
        std::string address;
        std::vector<int> matching_fds;
        for (auto const& [client_fd, remote] : connected_remote_clients) {
            if (remote.fingerprint == fingerprint)
                matching_fds.push_back(client_fd);
        }

        for (int client_fd : matching_fds) {
            auto ssl_it = active_ssl_.find(client_fd);
            if (ssl_it == active_ssl_.end())
                continue;

            promote_session(client_fd, fingerprint, device_name);
            address = connected_remote_clients[client_fd].address;
            promoted = true;

            // We are the approver here, so the peer is told its request went through.
            nlohmann::json response{{"command", "pair_accepted"}};
            const std::string payload = response.dump() + "\n";
            robust_ssl_write(ssl_it->second, payload.c_str(), payload.size());
        }

        nlohmann::json event{{"command", "pair_accepted"},
                             {"fingerprint", fingerprint},
                             {"device_name", device_name},
                             {"address", address},
                             {"connected", promoted}};
        broadcast_local_event(event.dump());

        if (!promoted && peers_changed_)
            peers_changed_();

        // An external acceptance supersedes any dialog for the same request.
        // Its exit callback observes the promoted session and ignores the stale
        // non-zero result rather than sending pair_rejected to the phone.
        for (auto& [read_fd, dialog] : pending_dialogs_) {
            (void)read_fd;
            if (dialog.fingerprint != fingerprint)
                continue;
            dialog.superseded = true;
            kill(dialog.pid, SIGTERM);
        }

        debug::log(INFO,
                   promoted ? "[Pairing Accepted] {} ({}) and live session promoted"
                            : "[Pairing Accepted] {} ({}) for the next connection",
                   device_name,
                   fingerprint);
        return promoted;
    }

    void TcpServer::spawn_pair_dialog(int client_fd,
                                      SSL* ssl,
                                      const std::string& fingerprint,
                                      const std::string& device_name) {
        // A headless client approves through the command socket. No GUI is not a rejection.
        // WAYLAND_DISPLAY alone would also skip a working X11 session: tether-dialog
        // falls back to an ordinary window when gtk-layer-shell isn't available.
        const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
        const char* x11_display = std::getenv("DISPLAY");
        if ((!wayland_display || !*wayland_display) && (!x11_display || !*x11_display))
            return;

        // Translated before the fork: gettext takes a lock, and calling it in the
        // child of a threaded process can deadlock if another thread held it.
        std::string short_fp = fingerprint;
        if (short_fp.size() > 16) {
            short_fp = short_fp.substr(0, 16) + "...";
        }
        // TRANSLATORS: {0} is the device name, {1} is a shortened key fingerprint.
        const std::string body = tr_format(_("{0} ({1}) wants to pair with this device."), device_name, short_fp);
        const std::string title = _("Pairing Request");
        const std::string accept = _("Accept");
        const std::string reject = _("Reject");

        // Create a pipe so the parent can detect when the child exits via epoll
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            debug::log(ERR, "spawn_pair_dialog: pipe() failed: {}", std::strerror(errno));
            return;
        }

        pid_t pid = fork();
        if (pid < 0) {
            debug::log(ERR, "spawn_pair_dialog: fork() failed: {}", std::strerror(errno));
            close(pipefd[0]);
            close(pipefd[1]);
            return;
        }

        if (pid == 0) {
            // --- Child process ---
            close(pipefd[0]); // close read end
            // The write end stays open; it will close when this process exits,
            // signaling EOF to the parent's read end.

            // Try alongside the daemon binary first, then fall back to PATH
            std::filesystem::path self_path;
            try {
                self_path = std::filesystem::read_symlink("/proc/self/exe");
            } catch (...) {
            }
            std::string sibling = (self_path.parent_path() / "tether-dialog").string();

            execl(sibling.c_str(),
                  "tether-dialog",
                  "--title",
                  title.c_str(),
                  "--body",
                  body.c_str(),
                  "--accept",
                  accept.c_str(),
                  "--reject",
                  reject.c_str(),
                  "--timeout",
                  "60",
                  nullptr);
            // If sibling path failed, try PATH
            execlp("tether-dialog",
                   "tether-dialog",
                   "--title",
                   title.c_str(),
                   "--body",
                   body.c_str(),
                   "--accept",
                   accept.c_str(),
                   "--reject",
                   reject.c_str(),
                   "--timeout",
                   "60",
                   nullptr);
            // exec failed entirely
            debug::log(ERR, "spawn_pair_dialog: exec failed: {}", std::strerror(errno));
            _exit(3);
        }

        // --- Parent process ---
        close(pipefd[1]); // close write end

        // Set read end non-blocking for epoll
        int flags = fcntl(pipefd[0], F_GETFL, 0);
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

        pending_dialogs_[pipefd[0]] = {pid, client_fd, fingerprint, device_name};

        loop_.addFd(pipefd[0], [this](int read_fd) {
            // Pipe became readable → child exited (EOF on write end)
            char dummy;
            if (read(read_fd, &dummy, 1) < 0) {
                debug::log(ERR, "net read error\n");
            } // consume EOF

            auto it = pending_dialogs_.find(read_fd);
            if (it == pending_dialogs_.end()) {
                loop_.removeFd(read_fd);
                close(read_fd);
                return;
            }

            PendingPairDialog info = it->second;
            pending_dialogs_.erase(it);

            int status = 0;
            waitpid(info.pid, &status, 0);
            int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 3;

            if (exit_code == 0) {
                accept_device(info.fingerprint, info.device_name);
            } else if (info.superseded) {
                debug::log(INFO, "Pairing dialog dismissed after {} was accepted elsewhere", info.device_name);
            } else if (!bluetooth::dialog_answered(status)) {
                debug::log(WARN,
                           "[Pairing Pending] {}: dialog unavailable (exit code {}); explicit approval still required",
                           info.device_name,
                           exit_code);
            } else {
                debug::log(INFO, "[Pairing Rejected] {} (exit code {})", info.device_name, exit_code);
                erase_pending_pair(info.fingerprint);

                auto remote_it = connected_remote_clients.find(info.client_fd);
                auto ssl_it = active_ssl_.find(info.client_fd);

                nlohmann::json event;
                event["command"] = "pair_rejected";
                event["fingerprint"] = info.fingerprint;
                event["device_name"] = info.device_name;
                if (remote_it != connected_remote_clients.end()) {
                    event["address"] = remote_it->second.address;
                }
                broadcast_local_event(event.dump());

                if (ssl_it != active_ssl_.end()) {
                    nlohmann::json resp;
                    resp["command"] = "pair_rejected";
                    std::string payload = resp.dump() + "\n";
                    robust_ssl_write(ssl_it->second, payload.c_str(), payload.size());
                }
            }

            // Clean up the pipe and remove from epoll at the very end
            loop_.removeFd(read_fd);
            close(read_fd);
        });

        debug::log(INFO, "spawn_pair_dialog: Launched dialog (pid {}) for {}", pid, device_name);
    }

} // namespace tether
