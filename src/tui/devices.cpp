#include "app.hpp"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <glib.h>
#include <tether/crypto.hpp>
#include <tether/discovery.hpp>
#include <tether/i18n.hpp>
#include <tether/version.hpp>
#include <ui_text.hpp>

namespace tether::tui {

    namespace {

        using ui::json_string;

        constexpr int BT_PROGRESS_TIMEOUT_SECONDS = 60;

        // Wire names as the daemon uses them.
        struct AirPodsMode {
            const char* wire;
            const char* label;
        };
        const AirPodsMode AIRPODS_MODES[4] = {
            {"off", N_("Off")},
            {"transparency", N_("Transparency")},
            {"adaptive", N_("Adaptive")},
            {"anc", N_("Noise Cancellation")},
        };

        struct PauseMode {
            const char* wire;
            const char* label;
        };
        const PauseMode AIRPODS_PAUSE_MODES[3] = {
            {"never", N_("Never")},
            {"one-removed", N_("One bud is removed")},
            {"both-removed", N_("Both buds are removed")},
        };

        const std::string OVERVIEW_ID = "overview";
        const std::string WIFI_PREFIX = "wifi:";
        const std::string BT_PREFIX = "bt:";

        struct Peer {
            std::string fingerprint;
            std::string name;
            std::string ip;
            uint16_t port = 5134;
        };

        std::string with_arg(const char* format, const std::string& arg) {
            gchar* text = g_strdup_printf(format, arg.c_str());
            std::string out = text ? text : "";
            g_free(text);
            return out;
        }

        class DevicesPage;
        DevicesPage* g_page = nullptr;

        class DevicesPage : public Page {
        public:
            DevicesPage() { g_page = this; }

            std::string title() const override { return _("Devices"); }

            void set_visible(bool visible) override {
                if (visible)
                    reload_paired();
            }

            void trigger_discovery() {
                app::status(_("Scanning for nearby devices..."));
                app::send({{"command", "discover"}});
                app::send({{"command", "bt_status"}});
                app::send({{"command", "bt_scan"}});
            }

            void handle_disconnect() override {
                set_bt_progress("");
                airpods_connecting_ = false;
                send_queue_.clear();
                send_batch_total_ = 0;
                send_failed_ = 0;
                send_skipped_ = 0;
                send_last_error_.clear();
            }

            std::vector<Row> list() override {
                std::vector<Row> rows;
                Row overview;
                overview.text = _("Overview");
                overview.id = OVERVIEW_ID;
                overview.selectable = true;
                rows.push_back(overview);

                const std::string my_fp = tether::Crypto::instance().get_my_fingerprint();

                std::vector<Row> connected, remembered, nearby;
                for (const auto& [fp, name] : paired_) {
                    const bool online = is_online(fp);
                    Row row = device_row(name, online ? _("Connected") : _("Offline"), WIFI_PREFIX + fp);
                    row.tone = online ? Tone::Good : Tone::Normal;
                    (online ? connected : remembered).push_back(row);
                }
                std::vector<std::string> shown;
                const auto offer = [&](const tether::DiscoveredDevice& device) {
                    if (device.fingerprint == my_fp || is_paired(device.fingerprint) ||
                        std::find(shown.begin(), shown.end(), device.fingerprint) != shown.end())
                        return;
                    shown.push_back(device.fingerprint);
                    nearby.push_back(
                        device_row(device.name, _("Nearby (Tap to Pair)"), WIFI_PREFIX + device.fingerprint));
                };
                for (const auto* source : {&discovered_, &unpaired_clients_})
                    for (const auto& device : *source)
                        offer(device);
                for (const auto& request : pending_requests_)
                    offer(request);

                if (!connected.empty() || !remembered.empty() || !nearby.empty()) {
                    rows.push_back(heading("WI-FI"));
                    for (const auto* group : {&connected, &remembered, &nearby})
                        rows.insert(rows.end(), group->begin(), group->end());
                }

                std::vector<Row> bt_rows;
                for (const auto& device : bt_devices_) {
                    const std::string address = device.value("address", "");
                    if (!device.value("iphone", false) && !device.value("airpods", false) && !is_supervised(address))
                        continue;
                    const std::string name = device.value("name", "").empty() ? address : device.value("name", "");
                    const bool paired = device.value("paired", false);
                    const bool is_connected = device.value("connected", false);
                    const bool airpods = device.value("airpods", false);
                    const bool route_ready = is_supervised(address) && (bt_connection_.value("map_open", false) ||
                                                                        bt_connection_.value("ancs_ready", false));

                    std::string subtitle = (route_ready || (airpods && is_connected)) ? _("Connected")
                                           : is_connected                             ? _("Partially connected")
                                           : paired                                   ? _("Paired")
                                                                                      : _("Nearby (Tap to Pair)");
                    if (airpods && bt_airpods_.value("address", "") == address) {
                        const std::string battery = ui::airpods_status_text(bt_airpods_);
                        subtitle += "\n  " + (battery.empty() ? std::string(_("Reading battery…")) : battery);
                    }
                    Row row = device_row(name, subtitle, BT_PREFIX + address);
                    row.tone = route_ready || (airpods && is_connected) ? Tone::Good : Tone::Normal;
                    bt_rows.push_back(row);
                }
                if (!bt_rows.empty()) {
                    rows.push_back(heading("BLUETOOTH"));
                    rows.insert(rows.end(), bt_rows.begin(), bt_rows.end());
                }
                return rows;
            }

            void cursor_moved(const std::string& id) override {
                set_bt_progress("");
                selected_ = id;
            }

            std::vector<Row> detail() override {
                if (selected_.rfind(BT_PREFIX, 0) == 0) {
                    const std::string address = selected_.substr(BT_PREFIX.size());
                    const json* device = find_bt_device(address);
                    if (device && device->value("airpods", false))
                        return airpods_detail(address, *device);
                    return bt_detail(address, device);
                }
                if (selected_.rfind(WIFI_PREFIX, 0) == 0) {
                    const std::string fp = selected_.substr(WIFI_PREFIX.size());
                    if (is_paired(fp))
                        return paired_detail(fp);
                    return unpaired_detail(fp);
                }
                return overview_detail();
            }

            bool action(const std::string& action) override {
                if (action == "refresh") {
                    trigger_discovery();
                    return true;
                }
                if (action == "delete") {
                    if (selected_.rfind(WIFI_PREFIX, 0) == 0 && is_paired(selected_.substr(WIFI_PREFIX.size())))
                        forget(selected_.substr(WIFI_PREFIX.size()));
                    else if (selected_.rfind(BT_PREFIX, 0) == 0)
                        unpair(selected_.substr(BT_PREFIX.size()));
                    return true;
                }
                return false;
            }

            bool command(const std::string& name, const std::string& args) override {
                if (name == "scan" || name == "discover") {
                    trigger_discovery();
                    return true;
                }
                if (name == "send" || name == "s") {
                    send_files(args);
                    return true;
                }
                if (name == "clip" || name == "clipboard") {
                    send_clipboard();
                    return true;
                }
                if (name == "pair") {
                    if (selected_.rfind(BT_PREFIX, 0) == 0)
                        bt_pair(selected_.substr(BT_PREFIX.size()));
                    else if (selected_.rfind(WIFI_PREFIX, 0) == 0)
                        wifi_pair(selected_.substr(WIFI_PREFIX.size()));
                    else
                        app::status(_("Select a device first."));
                    return true;
                }
                if (name == "unpair") {
                    if (selected_.rfind(BT_PREFIX, 0) == 0)
                        unpair(selected_.substr(BT_PREFIX.size()));
                    return true;
                }
                if (name == "forget") {
                    if (selected_.rfind(WIFI_PREFIX, 0) == 0)
                        forget(selected_.substr(WIFI_PREFIX.size()));
                    return true;
                }
                if (name == "mode") {
                    for (const auto& mode : AIRPODS_MODES) {
                        if (args == mode.wire) {
                            app::send({{"command", "bt_airpods_mode"}, {"mode", mode.wire}});
                            return true;
                        }
                    }
                    app::status(_("Modes: off, transparency, adaptive, anc"));
                    return true;
                }
                return false;
            }

            bool handle_event(const json& event) override {
                const std::string command = event.value("command", "");
                const bool pairing_event = command == "bt_pair_progress" || command == "bt_pair_confirm_request" ||
                                           command == "bt_pair_result" || command == "bt_unpair_result";
                if (pairing_event && event.contains("operation_id") &&
                    (bt_operation_id_.empty() || event.value("operation_id", "") != bt_operation_id_))
                    return false;

                if (command == "state_snapshot") {
                    apply_state_snapshot(event);
                    return true;
                }
                if (command == "mdns_status") {
                    mdns_ok_ = event.value("available", true);
                    update_wifi_indicator();
                    return true;
                }
                if (command == "client_connected") {
                    const std::string fp = event.value("fingerprint", "");
                    if (!is_online(fp))
                        connected_fps_.push_back(fp);
                    reload_paired();
                    update_wifi_indicator();
                    return true;
                }
                if (command == "client_disconnected") {
                    const std::string fp = event.value("fingerprint", "");
                    connected_fps_.erase(std::remove(connected_fps_.begin(), connected_fps_.end(), fp),
                                         connected_fps_.end());
                    reload_paired();
                    update_wifi_indicator();
                    return true;
                }
                if (command == "pair_outbound_pending") {
                    // TRANSLATORS: {0} is the name of the device being paired with.
                    pair_note_ = tr_format(_("Waiting for approval on {0}..."), event.value("device_name", ""));
                    app::status(pair_note_);
                    return true;
                }
                if (command == "pair_rejected") {
                    const std::string reason = event.value("reason", "");
                    if (reason == "unreachable")
                        pair_note_ = _("Could not reach that device. If a firewall is running, Tether needs inbound "
                                       "TCP 5134 on the other machine.");
                    else if (reason == "refused")
                        pair_note_ = _("That device refused the connection. Is Tether running on it?");
                    else if (reason == "unresolved")
                        pair_note_ = _("That address could not be resolved.");
                    else if (reason == "failed")
                        pair_note_ = _("Could not connect to that device.");
                    else
                        pair_note_ = _("Pair request was rejected.");
                    app::status(pair_note_);
                    return true;
                }
                if (command == "pair_request_received" || command == "untrusted_client_connected") {
                    tether::DiscoveredDevice request;
                    request.fingerprint = event.value("fingerprint", "");
                    std::string name = event.value("device_name", _("Unknown Device"));
                    if (name == _("Unknown Device")) {
                        for (const auto& d : discovered_) {
                            if (d.fingerprint == request.fingerprint && !d.name.empty()) {
                                name = d.name;
                                break;
                            }
                        }
                    }
                    request.name = name;
                    request.addresses.push_back({event.value("address", ""), 5134});
                    const bool exists =
                        std::any_of(pending_requests_.begin(), pending_requests_.end(), [&](const auto& d) {
                            return d.fingerprint == request.fingerprint;
                        });
                    if (!exists)
                        pending_requests_.push_back(request);
                    return true;
                }
                if (command == "pair_accepted") {
                    const std::string fp = event.value("fingerprint", "");
                    if (!fp.empty()) {
                        if (event.value("connected", false) && !is_online(fp))
                            connected_fps_.push_back(fp);
                        // The daemon only emits this after writing known_hosts.
                        pending_requests_.erase(std::remove_if(pending_requests_.begin(),
                                                               pending_requests_.end(),
                                                               [&](const auto& d) { return d.fingerprint == fp; }),
                                                pending_requests_.end());
                        reload_paired();
                    }
                    return true;
                }
                if (command == "forget_device_result") {
                    const std::string fp = event.value("fingerprint", "");
                    connected_fps_.erase(std::remove(connected_fps_.begin(), connected_fps_.end(), fp),
                                         connected_fps_.end());
                    reload_paired();
                    update_wifi_indicator();
                    return true;
                }
                if (command == "discovery_result") {
                    discovered_ = parse_devices(event.value("devices", json::array()));
                    app::status(_("Ready"));
                    return true;
                }
                if (command == "file_send_complete") {
                    on_file_sent(event);
                    return true;
                }
                if (command == "clipboard_content") {
                    action_note_ = event.value("content", std::string{}).empty()
                                       ? _("Clipboard is empty.")
                                       : _("Desktop Clipboard Sync triggered.");
                    app::status(action_note_);
                    return true;
                }
                if (command == "bt_status") {
                    bt_status_ = event;
                    reload_paired();
                    return true;
                }
                if (command == "bt_devices") {
                    bt_devices_.clear();
                    if (event.contains("devices") && event["devices"].is_array())
                        for (const auto& device : event["devices"])
                            bt_devices_.push_back(device);
                    if (select_bt_after_scan_) {
                        const auto iphone = std::find_if(bt_devices_.begin(), bt_devices_.end(), [](const json& d) {
                            return d.value("iphone", false);
                        });
                        if (iphone != bt_devices_.end()) {
                            select_bt_after_scan_ = false;
                            app::status(tr_format(_("Found {}. Select it under BLUETOOTH to pair."),
                                                  iphone->value("name", "iPhone")));
                        }
                    }
                    return true;
                }
                if (command == "bt_airpods") {
                    bt_airpods_ = event;
                    const std::string address = event.value("address", "");
                    app::set_airpods(json_string(event, "name"), address.empty() ? "" : ui::airpods_status_text(event));
                    return true;
                }
                if (command == "bt_connection_changed") {
                    bt_connection_ = event;
                    const bool route_up = event.value("map_open", false) || event.value("ancs_ready", false);
                    std::string detail = ui::connection_reason(event);
                    if (detail.empty() && route_up)
                        detail = _("Messages and notifications are connected.");
                    app::set_route(app::Route::Bluetooth, route_up, detail);
                    // A broadcast the Messages and Notifications pages need too.
                    return false;
                }
                if (command == "bt_scan_result") {
                    const std::string message = event.value("message", "");
                    app::status(message);
                    set_bt_progress(message);
                    // A failed scan already carries the reason; only one that ran
                    // and found nothing wants the "check the phone" advice.
                    if (event.value("success", false) && (bt_devices_.empty() || select_bt_after_scan_)) {
                        scan_note_ = _("No iPhone found. Unlock the phone and open Settings > Bluetooth so it "
                                       "advertises, then scan again.");
                        if (select_bt_after_scan_)
                            action_note_ = scan_note_;
                        select_bt_after_scan_ = false;
                    } else if (!event.value("success", false)) {
                        scan_note_ = message;
                        if (select_bt_after_scan_)
                            action_note_ = message;
                        select_bt_after_scan_ = false;
                    }
                    return true;
                }
                if (command == "bt_solicit_result") {
                    set_bt_progress(event.value("message", ""));
                    return false;
                }
                if (command == "bt_pair_confirm_request") {
                    const std::string operation = bt_operation_id_;
                    const auto answer = [operation](bool accept) {
                        app::send({{"command", "bt_pair_confirm"}, {"operation_id", operation}, {"accept", accept}});
                    };
                    app::confirm(
                        with_arg(_("Does the iPhone show this code?\n\n%s"), event.value("code", "")),
                        [answer] { answer(true); },
                        [answer] { answer(false); });
                    return true;
                }
                if (command == "bt_airpods_connect_result") {
                    airpods_connecting_ = false;
                    if (!event.value("success", false))
                        app::status(event.value("message", ""));
                    return true;
                }
                if (command == "bt_pair_progress") {
                    set_bt_progress(event.value("step", "") + "  " + event.value("detail", ""));
                    return true;
                }
                if (command == "bt_pair_result" || command == "bt_unpair_result") {
                    bt_operation_id_.clear();
                    set_bt_progress(event.value("message", ""));
                    // The bond and the supervised device both just changed.
                    app::send({{"command", "bt_status"}});
                    app::send({{"command", "bt_list_devices"}});
                    return true;
                }
                return false;
            }

        private:
            static Row device_row(const std::string& name, const std::string& subtitle, const std::string& id) {
                Row row;
                row.text = name + "\n  " + subtitle;
                row.id = id;
                row.selectable = true;
                return row;
            }

            static std::vector<tether::DiscoveredDevice> parse_devices(const json& devices) {
                std::vector<tether::DiscoveredDevice> out;
                if (!devices.is_array())
                    return out;
                for (const auto& d : devices) {
                    tether::DiscoveredDevice device;
                    device.name = d.value("name", "");
                    device.fingerprint = d.value("fingerprint", "");
                    if (d.contains("addresses") && d["addresses"].is_array())
                        for (const auto& a : d["addresses"])
                            device.addresses.push_back({a.value("address", ""), a.value<uint16_t>("port", 5134)});
                    out.push_back(device);
                }
                return out;
            }

            // known_hosts.json is the source of truth for what is paired.
            void reload_paired() {
                tether::Crypto::instance().init();
                try {
                    const json known = json::parse(tether::Crypto::instance().get_known_hosts_dump());
                    paired_.clear();
                    for (auto& [fingerprint, name] : known.items())
                        paired_.push_back(
                            {fingerprint, name.is_string() ? name.get<std::string>() : _("Unknown Device")});
                } catch (...) {
                }
            }

            bool is_paired(const std::string& fp) const {
                return std::any_of(paired_.begin(), paired_.end(), [&](const auto& p) { return p.first == fp; });
            }

            bool is_online(const std::string& fp) const {
                return std::find(connected_fps_.begin(), connected_fps_.end(), fp) != connected_fps_.end();
            }

            // The daemon supervises one iPhone at a time, so live profile status
            // describes whichever device is bonded, not the one selected.
            bool is_supervised(const std::string& address) const {
                if (address.empty())
                    return false;
                const std::string supervised = bt_status_.value("device_address", "");
                return !supervised.empty() && supervised == address;
            }

            const json* find_bt_device(const std::string& address) const {
                for (const auto& device : bt_devices_)
                    if (device.value("address", "") == address)
                        return &device;
                return nullptr;
            }

            Peer find_peer(const std::string& fp) const {
                Peer peer;
                peer.fingerprint = fp;
                for (const auto& [paired_fp, name] : paired_)
                    if (paired_fp == fp)
                        peer.name = name;
                for (const auto* source : {&discovered_, &unpaired_clients_, &pending_requests_}) {
                    for (const auto& device : *source) {
                        if (device.fingerprint != fp)
                            continue;
                        if (peer.name.empty())
                            peer.name = device.name;
                        if (peer.ip.empty() && !device.addresses.empty()) {
                            peer.ip = device.addresses[0].address;
                            peer.port = device.addresses[0].port;
                        }
                    }
                }
                return peer;
            }

            // Progress text describes a moment, not a state, so it expires.
            void set_bt_progress(const std::string& text) {
                app::cancel(bt_progress_timer_);
                bt_progress_ = text;
                if (!text.empty())
                    bt_progress_timer_ = app::after(BT_PROGRESS_TIMEOUT_SECONDS, [this] {
                        bt_progress_timer_ = 0;
                        bt_progress_.clear();
                    });
            }

            void update_wifi_indicator() {
                const bool connected = !connected_fps_.empty();
                if (connected)
                    wifi_detail_ = clipboard_ok_ ? _("Clipboard, files, and OTP are connected.")
                                                 : _("Files and OTP are connected. This compositor does not give "
                                                     "Tether clipboard access, so clipboard sync is off.");
                else if (!mdns_ok_)
                    wifi_detail_ = _("avahi-daemon isn't running, so other devices can't find this PC. Start it "
                                     "with: sudo systemctl enable --now avahi-daemon");
                else if (firewall_active_ && !discovered_.empty())
                    wifi_detail_ = _("A device is on the network but cannot reach this PC. A firewall is "
                                     "running; Tether needs inbound TCP 5134. Allow it with: sudo ufw allow "
                                     "5134/tcp");
                else
                    wifi_detail_ = _("No paired device is connected. Switch it on and join the same network.");
                app::set_route(app::Route::WiFi, connected, wifi_detail_);
            }

            void apply_state_snapshot(const json& j) {
                mdns_ok_ = j.value("mdns_available", true);
                clipboard_ok_ = j.value("clipboard_available", true);
                firewall_active_ = j.value("firewall_active", false);
                if (j.contains("discovered_devices") && j["discovered_devices"].is_array())
                    discovered_ = parse_devices(j["discovered_devices"]);

                connected_fps_.clear();
                unpaired_clients_.clear();
                if (j.contains("connected_clients") && j["connected_clients"].is_array()) {
                    for (const auto& c : j["connected_clients"]) {
                        if (c.value("paired", false)) {
                            connected_fps_.push_back(c.value("fingerprint", ""));
                        } else {
                            tether::DiscoveredDevice device;
                            device.fingerprint = c.value("fingerprint", "");
                            device.name = c.value("device_name", _("Unknown Device"));
                            device.addresses.push_back({c.value("address", ""), 5134});
                            unpaired_clients_.push_back(device);
                        }
                    }
                }
                pending_requests_.clear();
                if (j.contains("pending_pairs") && j["pending_pairs"].is_array()) {
                    for (const auto& p : j["pending_pairs"]) {
                        tether::DiscoveredDevice request;
                        request.fingerprint = p.value("fingerprint", "");
                        request.name = p.value("device_name", _("Unknown Device"));
                        pending_requests_.push_back(request);
                    }
                }
                reload_paired();
                update_wifi_indicator();
            }

            void wifi_pair(const std::string& fp) {
                const Peer peer = find_peer(fp);
                if (peer.ip.empty())
                    return;
                pair_note_ = _("Sending pair request...");
                app::send(
                    {{"command", "pair_request"}, {"host", peer.ip}, {"port", peer.port}, {"device_name", peer.name}});
                app::status(_("Pair request sent. Approve on remote device!"));
            }

            void accept(const std::string& fp) {
                const Peer peer = find_peer(fp);
                app::send({{"command", "accept_device"}, {"fingerprint", fp}, {"device_name", peer.name}});
            }

            void forget(const std::string& fp) {
                const Peer peer = find_peer(fp);
                app::confirm(with_arg(_("Forget %s?"), peer.name) + "\n" +
                                 _("Tether will stop connecting to it. Forget this computer on the other device too, "
                                   "then pair again to reconnect."),
                             [fp] { app::send({{"command", "forget_device"}, {"fingerprint", fp}}); });
            }

            // The daemon opens a fresh connection per send_file, so a batch goes
            // one file at a time.
            void start_next_send() {
                if (send_queue_.empty())
                    return;
                const std::filesystem::path& path = send_queue_.front();
                // TRANSLATORS: {} is a file name.
                std::string status = tr_format(_("Sending {}…"), path.filename().string());
                if (send_batch_total_ > 1) {
                    const size_t index = send_batch_total_ - send_queue_.size() + 1;
                    // TRANSLATORS: {0} is the current file's position, {1} the batch size.
                    status += " " + tr_format(_("({0} of {1})"), index, send_batch_total_);
                }
                action_note_ = status;
                app::status(status);
                app::send({{"command", "send_file"}, {"path", path.string()}});
            }

            // Space-separated paths; a backslash escapes a space in a name.
            static std::vector<std::string> split_args(const std::string& args) {
                std::vector<std::string> out;
                std::string current;
                for (size_t i = 0; i < args.size(); ++i) {
                    const char c = args[i];
                    if (c == '\\' && i + 1 < args.size()) {
                        current += args[++i];
                    } else if (c == ' ') {
                        if (!current.empty())
                            out.push_back(std::move(current));
                        current.clear();
                    } else {
                        current += c;
                    }
                }
                if (!current.empty())
                    out.push_back(std::move(current));
                return out;
            }

            void send_files(const std::string& args) {
                std::vector<std::filesystem::path> files;
                size_t skipped = 0;
                for (std::string arg : split_args(args)) {
                    if (arg.rfind("~/", 0) == 0)
                        if (const char* home = g_get_home_dir())
                            arg = std::string(home) + arg.substr(1);
                    std::error_code ec;
                    const std::filesystem::path path = std::filesystem::absolute(arg, ec);
                    if (ec || !std::filesystem::is_regular_file(path, ec))
                        ++skipped;
                    else
                        files.push_back(path);
                }
                if (files.empty()) {
                    app::status(skipped ? _("Folders can't be sent — drop files instead.") : _("Usage: :send FILE…"));
                    return;
                }
                if (!app::connected()) {
                    app::status(_("Daemon unavailable."));
                    return;
                }
                const bool idle = send_queue_.empty();
                if (idle) {
                    send_batch_total_ = files.size();
                    send_failed_ = 0;
                    send_skipped_ = skipped;
                    send_last_error_.clear();
                } else {
                    send_batch_total_ += files.size();
                    send_skipped_ += skipped;
                }
                for (auto& path : files)
                    send_queue_.push_back(std::move(path));
                if (idle)
                    start_next_send();
            }

            void on_file_sent(const json& event) {
                if (!event.value("success", true)) {
                    ++send_failed_;
                    send_last_error_ = event.value("message", "");
                }
                if (!send_queue_.empty())
                    send_queue_.pop_front();
                if (!send_queue_.empty()) {
                    start_next_send();
                    return;
                }

                std::string message = event.value("message", "");
                if (send_batch_total_ > 1) {
                    const size_t sent = send_batch_total_ - send_failed_;
                    // TRANSLATORS: {0} is how many were sent, {1} the batch size.
                    message = tr_format(P_("Sent {0} of {1} file.", "Sent {0} of {1} files.", send_batch_total_),
                                        sent,
                                        send_batch_total_);
                    if (send_failed_ > 0 && !send_last_error_.empty())
                        message += " " + send_last_error_;
                }
                if (send_skipped_ > 0)
                    message +=
                        " " + tr_format(P_("Skipped {} non-file item.", "Skipped {} non-file items.", send_skipped_),
                                        send_skipped_);
                action_note_ = message;
                app::status(message);
                send_batch_total_ = 0;
                send_failed_ = 0;
                send_skipped_ = 0;
                send_last_error_.clear();
            }

            void send_clipboard() {
                app::send({{"command", "clipboard_send"}});
                action_note_ = _("Clipboard sync requested...");
                app::status(action_note_);
            }

            void pair_over_bluetooth() {
                select_bt_after_scan_ = true;
                action_note_ = _("Scanning for nearby devices...");
                app::status(action_note_);
                app::send({{"command", "bt_status"}});
                app::send({{"command", "bt_scan"}});
            }

            static std::string new_operation_id() {
                gchar* uuid = g_uuid_string_random();
                std::string id = uuid;
                g_free(uuid);
                return id;
            }

            void bt_pair(const std::string& address) {
                bt_operation_id_ = new_operation_id();
                if (!app::send({{"command", "bt_pair"}, {"address", address}, {"operation_id", bt_operation_id_}})) {
                    bt_operation_id_.clear();
                    set_bt_progress(_("Could not reach the Tether daemon."));
                    return;
                }
                set_bt_progress(_("Pairing… confirm the prompt on the iPhone."));
            }

            void unpair(const std::string& address) {
                const json* device = find_bt_device(address);
                if (!device || !device->value("paired", false))
                    return;
                const std::string name = device->value("name", address);
                app::confirm(
                    with_arg(_("Remove the Bluetooth pairing with %s?"), name) + "\n" +
                        _("Also delete this computer from the iPhone's Bluetooth settings before pairing "
                          "again, or the phone will keep the stale bond."),
                    [this, address] {
                        bt_operation_id_ = new_operation_id();
                        app::send({{"command", "bt_unpair"}, {"address", address}, {"operation_id", bt_operation_id_}});
                        set_bt_progress(_("Removing the pairing…"));
                    });
            }

            void solicit() {
                if (!app::send({{"command", "bt_solicit"}})) {
                    set_bt_progress(_("Could not reach the Tether daemon."));
                    return;
                }
                set_bt_progress(_("Asking the iPhone to show its Bluetooth permissions…"));
            }

            static Row check_row(const std::string& title, bool ok, const std::string& note) {
                Row row = text_row(std::string(ok ? "✓  " : "✗  ") + title + (note.empty() ? "" : "  " + note),
                                   ok ? Tone::Good : Tone::Bad);
                return row;
            }

            static Row toggle_row(const std::string& label, bool on, bool enabled, std::function<void()> flip) {
                Row row;
                row.text = std::string(on ? "[x] " : "[ ] ") + label;
                row.id = label;
                row.selectable = true;
                row.tone = enabled ? Tone::Normal : Tone::Muted;
                if (enabled)
                    row.activate = std::move(flip);
                return row;
            }

            std::vector<Row> overview_detail() {
                std::vector<Row> rows;
                rows.push_back(heading(_("Devices")));
                rows.push_back(blank_row());
                rows.push_back(heading("Wi-Fi"));
                rows.push_back(text_row(wifi_detail_.empty() ? _("Not connected yet.") : wifi_detail_));
                rows.push_back(blank_row());

                const bool bt_ok = bt_connection_.value("map_open", false) || bt_connection_.value("ancs_ready", false);
                std::string bt_note = _("Not connected yet.");
                if (bt_ok)
                    bt_note = _("Connected.");
                else if (!bt_status_.empty() && !bt_status_.value("available", false))
                    bt_note = _("Bluetooth is unavailable on this machine.");
                if (!scan_note_.empty() && !bt_ok)
                    bt_note = scan_note_;
                rows.push_back(heading("Bluetooth"));
                rows.push_back(text_row(bt_note));
                if (!bt_progress_.empty())
                    rows.push_back(text_row(bt_progress_, Tone::Accent));
                rows.push_back(blank_row());
                rows.push_back(action_row(_("Scan for devices"), [this] { trigger_discovery(); }));
                return rows;
            }

            std::vector<Row> unpaired_detail(const std::string& fp) {
                const Peer peer = find_peer(fp);
                std::vector<Row> rows;
                rows.push_back(heading(peer.name));
                // TRANSLATORS: {0} is an IP address, {1} a port number.
                rows.push_back(
                    text_row(pair_note_.empty() ? tr_format(_("Found at {0}:{1}"), peer.ip, peer.port) : pair_note_,
                             Tone::Muted));
                rows.push_back(blank_row());
                rows.push_back(action_row(_("Pair Device"), [this, fp] { wifi_pair(fp); }));
                rows.push_back(action_row(_("Force Trust (Accept Pending)"), [this, fp] { accept(fp); }));
                return rows;
            }

            std::vector<Row> paired_detail(const std::string& fp) {
                const Peer peer = find_peer(fp);
                const bool online = is_online(fp);
                std::vector<Row> rows;
                rows.push_back(heading(peer.name));
                if (select_bt_after_scan_)
                    rows.push_back(text_row(_("Scanning for nearby devices..."), Tone::Muted));
                else
                    rows.push_back(text_row(online ? _("Connected and ready.")
                                                   : _("Device is offline. Make sure it is switched on and on the "
                                                       "same network."),
                                            online ? Tone::Good : Tone::Muted));
                if (!action_note_.empty())
                    rows.push_back(text_row(action_note_, Tone::Accent));
                rows.push_back(blank_row());
                if (online) {
                    rows.push_back(action_row(_("Send File"), [] {
                        app::prompt(
                            std::string(_("Send File")) + ": ",
                            [](const std::string& path) {
                                if (!path.empty())
                                    g_page->send_files(path);
                            },
                            [](const std::string& text) {
                                // Completes the last path on the line.
                                const size_t space = text.rfind(' ');
                                const std::string head = space == std::string::npos ? "" : text.substr(0, space + 1);
                                std::vector<Candidate> out;
                                for (auto& c : complete_files(text.substr(head.size())))
                                    out.push_back({c.label, head + c.value});
                                return out;
                            });
                    }));
                    rows.push_back(action_row(_("Send Clipboard"), [this] { send_clipboard(); }));
                }
                if (bt_status_.value("device_address", "").empty() && !select_bt_after_scan_)
                    rows.push_back(action_row(_("Pair over Bluetooth"), [this] { pair_over_bluetooth(); }));
                rows.push_back(action_row(_("Forget"), [this, fp] { forget(fp); }));
                return rows;
            }

            static std::vector<Candidate> complete_files(const std::string& partial) {
                namespace fs = std::filesystem;
                std::vector<Candidate> out;
                std::string expanded = partial;
                if (expanded.rfind("~/", 0) == 0)
                    expanded = std::string(g_get_home_dir()) + expanded.substr(1);
                const size_t slash = expanded.rfind('/');
                const std::string dir =
                    slash == std::string::npos ? "." : (slash == 0 ? "/" : expanded.substr(0, slash));
                const std::string stem = slash == std::string::npos ? expanded : expanded.substr(slash + 1);
                const std::string prefix =
                    partial.rfind('/') == std::string::npos ? "" : partial.substr(0, partial.rfind('/') + 1);
                std::error_code ec;
                for (const auto& entry : fs::directory_iterator(dir, ec)) {
                    const std::string name = entry.path().filename().string();
                    if (name.rfind(stem, 0) != 0 || (name[0] == '.' && stem.empty()))
                        continue;
                    const std::string value = prefix + name + (entry.is_directory(ec) ? "/" : "");
                    out.push_back({value, value});
                    if (out.size() >= 200)
                        break;
                }
                std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.value < b.value; });
                return out;
            }

            std::vector<Row> bt_detail(const std::string& address, const json* device) {
                const bool paired = device && device->value("paired", false);
                const bool supervised = is_supervised(address);
                const bool available = bt_status_.value("available", false);
                const bool bt_on = bt_status_.value("enabled", true);
                const bool ancs_on = bt_status_.value("ancs_enabled", true);
                const std::string name = device ? device->value("name", address) : address;

                std::vector<Row> rows;
                rows.push_back(heading(name.empty() ? address : name));

                const json capability = bt_status_.value("capability", json());
                std::string mode;
                if (!bt_status_.empty()) {
                    if (!available || capability.is_null())
                        mode = _("Bluetooth is unavailable on this machine.");
                    else if (capability.value("mode", "") == "full")
                        mode = _("Full mode — messages, contacts, and notifications.");
                    else if (capability.value("mode", "") == "compatibility")
                        mode = _("Compatibility mode — messages and contacts, no notification mirroring.");
                    else
                        mode = _("This machine cannot carry the Bluetooth features.");
                    if (!capability.is_null() && capability.contains("reasons"))
                        for (const auto& reason : capability["reasons"])
                            if (reason.is_string())
                                mode += "\n" + reason.get<std::string>();
                    // A package upgrade replaces the binaries but leaves the old tetherd running.
                    const std::string running = bt_status_.value("version", "");
                    if (running != TETHER_VERSION)
                        mode += "\n" + tr_format(_("The running tetherd is {}, but this is tether {}. Stop it and it\n"
                                                   "restarts on demand:\n"
                                                   "    pkill tetherd\n"),
                                                 running.empty() ? _("an older build") : running.c_str(),
                                                 TETHER_VERSION);
                }
                if (!mode.empty())
                    rows.push_back(text_row(mode, Tone::Muted));

                // Only the supervised bond has live profile state.
                const json& c = bt_connection_;
                const bool classic = supervised ? c.value("classic_connected", false)
                                                : (device && device->value("classic_connected", false));
                const bool le =
                    supervised ? c.value("le_connected", false) : (device && device->value("le_connected", false));
                const bool map_open = supervised && c.value("map_open", false);
                const bool pbap_open = supervised && c.value("pbap_open", false);
                const bool ancs_ready = supervised && c.value("ancs_ready", false);
                const std::string map_error = c.value("map_error", "none");
                const std::string pbap_error = c.value("pbap_error", "none");

                rows.push_back(blank_row());
                rows.push_back(
                    check_row(_("Link"),
                              classic || le,
                              std::string("BR/EDR ") + (classic ? "on" : "off") + ", LE " + (le ? "on" : "off")));
                rows.push_back(check_row(_("Messages"), map_open, map_open || map_error == "none" ? "" : map_error));
                rows.push_back(
                    check_row(_("Contacts"), pbap_open, pbap_open || pbap_error == "none" ? "" : pbap_error));
                rows.push_back(check_row(_("Notifications"), ancs_ready, ancs_ready ? "" : c.value("ancs_reason", "")));

                std::string what, commands;
                if (capability.is_object() && capability.contains("setup")) {
                    int n = 0;
                    for (const auto& step : capability["setup"]) {
                        what += (what.empty() ? "" : "\n") + std::to_string(++n) + ". " + step.value("what", "");
                        commands += (commands.empty() ? "" : "\n") + step.value("command", "");
                    }
                }
                const bool all_live = map_open && pbap_open && (ancs_ready || !ancs_on);
                if (!commands.empty() && !all_live) {
                    rows.push_back(blank_row());
                    rows.push_back(text_row(what));
                    Row command_row = text_row(commands, Tone::Accent);
                    command_row.selectable = true;
                    command_row.id = "setup";
                    command_row.yank = commands;
                    command_row.activate = [commands] { app::yank(commands); };
                    rows.push_back(command_row);
                }

                // The daemon's reasons name the actual next step, so they are
                // shown verbatim.
                std::string reason;
                if (!paired)
                    reason = _("Not paired over Bluetooth yet.");
                else if (!supervised)
                    reason = _("Tether is not using this device. Pair it to select it.");
                else if (!bt_on)
                    reason = _("Bluetooth is switched off for this iPhone.");
                else
                    reason = ui::connection_reason(c);
                rows.push_back(blank_row());
                if (!reason.empty())
                    rows.push_back(text_row(reason));
                if (!bt_progress_.empty())
                    rows.push_back(text_row(bt_progress_, Tone::Accent));
                rows.push_back(blank_row());

                if (available && supervised)
                    rows.push_back(toggle_row(_("Connect to this iPhone over Bluetooth"), bt_on, true, [bt_on] {
                        app::send({{"command", "bt_set_enabled"}, {"enabled", !bt_on}});
                    }));
                if (available && (!paired || !supervised) && bt_operation_id_.empty())
                    rows.push_back(action_row(_("Pair over Bluetooth"), [this, address] { bt_pair(address); }));
                if (available && paired)
                    rows.push_back(action_row(_("Unpair"), [this, address] { unpair(address); }));
                // Re-soliciting only helps when the phone is withholding a profile.
                if (available && supervised && (map_error == "forbidden" || map_error == "no_record" || !ancs_ready))
                    rows.push_back(action_row(_("Show iPhone Permissions"), [this] { solicit(); }));
                return rows;
            }

            std::vector<Row> airpods_detail(const std::string& address, const json& device) {
                const json& airpods = bt_airpods_;
                const bool managed = bt_status_.value("airpods_enabled", false);
                std::vector<Row> rows;
                rows.push_back(heading(json_string(airpods, "name", device.value("name", address))));

                const std::string levels = ui::airpods_status_text(airpods);
                if (managed)
                    rows.push_back(text_row(levels.empty() ? _("Reading battery…") : levels));

                const std::string current = json_string(airpods, "anc");
                const char* reason =
                    !managed           ? _("Tether is not managing the AirPods, so another program can use them.")
                    : !current.empty() ? ""
                    : json_string(airpods, "address").empty() ? _("No AirPods are connected.")
                                                              : _("These AirPods do not report a listening mode.");
                if (*reason)
                    rows.push_back(text_row(reason, Tone::Muted));

                // Which bud is "primary" moves between them, so this counts.
                const int in_ear = airpods.value("in_ear", 0);
                const json ear = airpods.value("ear", json::object());
                const bool ear_known = json_string(ear, "primary", "unknown") != "unknown" ||
                                       json_string(ear, "secondary", "unknown") != "unknown";
                rows.push_back(text_row(!ear_known    ? _("These AirPods do not report whether they are being worn.")
                                        : in_ear == 2 ? _("Both buds are in.")
                                        : in_ear == 1 ? _("One bud is in.")
                                                      : _("Neither bud is in."),
                                        Tone::Muted));
                rows.push_back(blank_row());

                // A bluez link action, so it works whether or not tether manages the buds.
                const bool is_connected = device.value("connected", false);
                if (!airpods_connecting_)
                    rows.push_back(action_row(
                        is_connected ? _("Disconnect") : _("Connect"),
                        [this, address, is_connected] {
                            if (!app::send({{"command", "bt_airpods_connect"},
                                            {"address", address},
                                            {"connect", !is_connected}})) {
                                app::status(_("Could not reach the Tether daemon."));
                                return;
                            }
                            // Held until the result, so a slow Connect is not sent twice.
                            airpods_connecting_ = true;
                        },
                        "connect"));
                rows.push_back(toggle_row(_("Manage AirPods from Tether"), managed, true, [managed] {
                    app::send({{"command", "bt_airpods_enable"}, {"enabled", !managed}});
                }));

                rows.push_back(blank_row());
                rows.push_back(heading(_("Listening mode")));
                for (const auto& mode : AIRPODS_MODES) {
                    Row row = toggle_row(
                        _(mode.label), current == mode.wire, managed && !current.empty(), [wire = mode.wire] {
                            app::send({{"command", "bt_airpods_mode"}, {"mode", wire}});
                        });
                    row.text = std::string(current == mode.wire ? "(•) " : "( ) ") + _(mode.label);
                    row.id = std::string("mode:") + mode.wire;
                    rows.push_back(row);
                }

                rows.push_back(blank_row());
                rows.push_back(heading(_("Pause playback when:")));
                const std::string pause = bt_status_.value("airpods_pause", "never");
                for (const auto& mode : AIRPODS_PAUSE_MODES) {
                    Row row = toggle_row(_(mode.label), pause == mode.wire, managed && ear_known, [wire = mode.wire] {
                        app::send({{"command", "bt_airpods_pause"}, {"mode", wire}});
                    });
                    row.text = std::string(pause == mode.wire ? "(•) " : "( ) ") + _(mode.label);
                    row.id = std::string("pause:") + mode.wire;
                    rows.push_back(row);
                }

                // Ownership handoff runs off the buds' own state; only the
                // disconnect fallback needs the iPhone's call state.
                const bool apple_id = bt_status_.value("apple_device_id", false);
                const bool calls_on = bt_status_.value("calls_enabled", false);
                const bool handoff = bt_status_.value("airpods_handoff", false);
                rows.push_back(blank_row());
                rows.push_back(
                    toggle_row(_("Hand the AirPods to the iPhone during a call"),
                               handoff,
                               managed && (apple_id || calls_on),
                               [handoff] { app::send({{"command", "bt_airpods_handoff"}, {"enabled", !handoff}}); }));
                rows.push_back(text_row(apple_id ? _("The buds are handed over with Apple's own handoff.")
                                                 : _("The buds are handed over by disconnecting them. For Apple's own "
                                                     "handoff, put DeviceID = bluetooth:004C:0000:0000 under [General] "
                                                     "in /etc/bluetooth/main.conf and restart bluetooth."),
                                        Tone::Muted));
                return rows;
            }

            std::vector<tether::DiscoveredDevice> discovered_;
            std::vector<tether::DiscoveredDevice> unpaired_clients_;
            std::vector<tether::DiscoveredDevice> pending_requests_;
            std::vector<std::pair<std::string, std::string>> paired_; // fingerprint, name
            std::vector<std::string> connected_fps_;

            bool mdns_ok_ = true;
            bool clipboard_ok_ = true;
            bool firewall_active_ = false;
            std::string wifi_detail_;

            std::vector<json> bt_devices_;
            // An object, not null: bt_devices can arrive before the first bt_airpods.
            json bt_airpods_ = json::object();
            json bt_status_ = json::object();
            json bt_connection_ = json::object();

            std::string selected_;
            std::string pair_note_;
            std::string action_note_;
            std::string scan_note_;
            bool select_bt_after_scan_ = false;
            bool airpods_connecting_ = false;

            std::string bt_progress_;
            int bt_progress_timer_ = 0;
            std::string bt_operation_id_;

            // Files waiting to go to the phone, front is in flight.
            std::deque<std::filesystem::path> send_queue_;
            size_t send_batch_total_ = 0;
            size_t send_failed_ = 0;
            size_t send_skipped_ = 0;
            std::string send_last_error_;
        };

    } // namespace

    std::unique_ptr<Page> make_devices_page() { return std::make_unique<DevicesPage>(); }

    void devices_trigger_discovery() {
        if (g_page)
            g_page->trigger_discovery();
    }

} // namespace tether::tui
