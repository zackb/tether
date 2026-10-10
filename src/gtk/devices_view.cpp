#include "devices_view.hpp"
#include "daemon_client.hpp"
#include "tray.hpp"
#include "ui_util.hpp"
#include <tether/i18n.hpp>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <tether/crypto.hpp>
#include <tether/discovery.hpp>
#include <tether/version.hpp>
#include <vector>

namespace tether::ui {

    namespace {

        struct DevicesState {
            std::vector<tether::DiscoveredDevice> discovered_devices;
            std::vector<tether::DiscoveredDevice> unpaired_clients;
            std::vector<tether::DiscoveredDevice> pending_pairing_requests;
            std::vector<std::pair<std::string, std::string>> paired_devices; // fp, name
            std::vector<std::string> connected_fps;                          // active connections

            // false when avahi-daemon is down
            bool mdns_ok = true;

            // false when the compositor withholds data-control (flatpak)
            bool clipboard_ok = true;
            bool firewall_active = false;
            // Why Wi-Fi is or is not connected, including the command that fixes it.
            std::string wifi_detail;

            // The Bluetooth route
            std::vector<nlohmann::json> bt_devices;
            // An object, not null: bt_devices can arrive before the first bt_airpods.
            nlohmann::json bt_airpods = nlohmann::json::object();
            nlohmann::json bt_status = nlohmann::json::object();
            nlohmann::json bt_connection = nlohmann::json::object();

            GtkWidget* list_devices = nullptr;
            GtkWidget* right_pane_stack = nullptr;

            std::string selected_device_fp;
            std::string selected_device_name;
            std::string selected_device_ip;
            uint16_t selected_device_port = 5134;

            GtkWidget* lbl_action_name = nullptr;
            GtkWidget* lbl_action_status = nullptr;
            GtkWidget* btn_grid = nullptr;
            GtkWidget* btn_action_bt_pair = nullptr;
            GtkWidget* btn_forget = nullptr;
            bool select_bt_after_scan = false;

            GtkWidget* lbl_unpaired_name = nullptr;
            GtkWidget* lbl_unpaired_ip = nullptr;

            std::string selected_bt_address;
            std::string selected_bt_name;

            GtkWidget* lbl_airpods_name = nullptr;
            GtkWidget* lbl_airpods_battery = nullptr;
            GtkWidget* lbl_airpods_reason = nullptr;
            GtkWidget* lbl_airpods_worn = nullptr;
            GtkWidget* chk_airpods_enabled = nullptr;
            GtkWidget* cmb_airpods_pause = nullptr;
            GtkWidget* chk_airpods_handoff = nullptr;
            GtkWidget* lbl_airpods_apple_id = nullptr;
            GtkWidget* airpods_mode_buttons[4] = {nullptr, nullptr, nullptr, nullptr};
            bool airpods_syncing = false;
            GtkWidget* btn_airpods_connect = nullptr;
            bool airpods_connecting = false;

            GtkWidget* lbl_bt_name = nullptr;
            GtkWidget* bt_setup_box = nullptr;
            GtkWidget* lbl_bt_setup_what = nullptr;
            GtkWidget* lbl_bt_setup_command = nullptr;
            std::string bt_setup_commands;
            GtkWidget* lbl_bt_mode = nullptr;
            GtkWidget* lbl_bt_link = nullptr;
            GtkWidget* lbl_bt_messages = nullptr;
            GtkWidget* lbl_bt_contacts = nullptr;
            GtkWidget* lbl_bt_notifications = nullptr;
            GtkWidget* lbl_bt_reason = nullptr;
            GtkWidget* lbl_bt_progress = nullptr;
            guint bt_progress_expiry_id = 0;
            GtkWidget* btn_bt_pair = nullptr;
            GtkWidget* btn_bt_solicit = nullptr;
            GtkWidget* chk_bt_enabled = nullptr;
            GtkWidget* btn_bt_unpair = nullptr;
            std::string bt_operation_id;

            GtkWidget* lbl_welcome_wifi = nullptr;
            GtkWidget* lbl_welcome_bt = nullptr;

            // Files waiting to go to the phone, front is in flight.
            GtkWidget* dropzone = nullptr;
            std::deque<std::filesystem::path> send_queue;
            size_t send_batch_total = 0;
            size_t send_failed = 0;
            size_t send_skipped = 0;
            std::string send_last_error;
        };

        DevicesState g_devices;

        void set_status_action(const std::string& text) { set_text(g_devices.lbl_action_status, text); }

        constexpr int BT_PROGRESS_TIMEOUT_SECONDS = 60;

        // Progress text describes a moment, not a state, so it expires.
        void set_bt_progress(const std::string& text) {
            if (g_devices.bt_progress_expiry_id != 0) {
                g_source_remove(g_devices.bt_progress_expiry_id);
                g_devices.bt_progress_expiry_id = 0;
            }
            set_text(g_devices.lbl_bt_progress, text);
            if (text.empty())
                return;
            g_devices.bt_progress_expiry_id = g_timeout_add_seconds(
                BT_PROGRESS_TIMEOUT_SECONDS,
                [](gpointer) -> gboolean {
                    g_devices.bt_progress_expiry_id = 0;
                    set_text(g_devices.lbl_bt_progress, "");
                    return G_SOURCE_REMOVE;
                },
                nullptr);
        }

        const nlohmann::json* find_bt_device(const std::string& address) {
            for (const auto& device : g_devices.bt_devices) {
                if (device.value("address", "") == address)
                    return &device;
            }
            return nullptr;
        }

        // The device row has room to say why it is empty; the tray tooltip does not.
        std::string airpods_row_text(const nlohmann::json& airpods) {
            const std::string text = airpods_status_text(airpods);
            return text.empty() ? _("Reading battery\u2026") : text;
        }

        void set_battery_label(GtkWidget* label, const nlohmann::json& airpods, const std::string& text) {
            set_text(label, text);
            const std::string spoken = text.empty() ? "" : airpods_status_text(airpods, true);
            set_accessible_name(label, spoken.empty() ? text : spoken);
        }

        // The daemon supervises one iPhone at a time, so the live profile status
        // describes whichever Bluetooth device is bonded, not the one merely
        // selected in the list.
        bool is_supervised_bt_device(const std::string& address) {
            if (address.empty())
                return false;
            const std::string supervised = g_devices.bt_status.value("device_address", "");
            return !supervised.empty() && supervised == address;
        }

        void set_capability_row(GtkWidget* label, const std::string& title, bool ok, const std::string& note) {
            std::string markup = std::string(ok ? "\u2713" : "\u2717") + "  <b>" + escape_markup(title) + "</b>";
            if (!note.empty())
                markup += "  <span size='small'>" + escape_markup(note) + "</span>";
            set_markup(label, markup);
        }

        void update_welcome_pane() {
            // The route bar keeps the reason in a hover tooltip; here it is readable
            // by keyboard and screen reader, and the fix command can be copied.
            set_text(g_devices.lbl_welcome_wifi,
                     g_devices.wifi_detail.empty() ? _("Not connected yet.") : g_devices.wifi_detail);

            const bool bt_ok =
                g_devices.bt_connection.value("map_open", false) || g_devices.bt_connection.value("ancs_ready", false);
            std::string bt_note = _("Not connected yet.");
            if (bt_ok)
                bt_note = _("Connected.");
            else if (!g_devices.bt_status.empty() && !g_devices.bt_status.value("available", false))
                bt_note = _("Bluetooth is unavailable on this machine.");
            set_text(g_devices.lbl_welcome_bt, bt_note);
        }

        void on_bt_enabled_toggled(GtkWidget* widget, gpointer);

        void update_action_bt_scan_controls() {
            if (g_devices.btn_action_bt_pair)
                gtk_widget_set_sensitive(g_devices.btn_action_bt_pair, !g_devices.select_bt_after_scan);
        }

        void on_action_bt_pair_clicked(GtkWidget*, gpointer) {
            g_devices.select_bt_after_scan = true;
            update_action_bt_scan_controls();
            set_status_action(_("Scanning for nearby devices..."));
            set_status_main(_("Scanning for nearby devices..."));
            daemon_send({{"command", "bt_status"}});
            daemon_send({{"command", "bt_scan"}});
        }

        // Wire names as the daemon uses them
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

        // Wire values for the pause-on-removal setting, in the order they appear
        // in the dropdown.
        const char* const AIRPODS_PAUSE_MODES[3] = {"never", "one-removed", "both-removed"};

        void on_airpods_pause_changed(GtkWidget* combo, gpointer) {
            if (g_devices.airpods_syncing)
                return;
            const int index = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
            if (index < 0 || index > 2)
                return;
            daemon_send({{"command", "bt_airpods_pause"}, {"mode", AIRPODS_PAUSE_MODES[index]}});
        }

        void on_airpods_enabled_toggled(GtkWidget* button, gpointer) {
            if (g_devices.airpods_syncing)
                return;
            daemon_send({{"command", "bt_airpods_enable"},
                         {"enabled", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)) == TRUE}});
        }

        void on_airpods_handoff_toggled(GtkWidget* button, gpointer) {
            if (g_devices.airpods_syncing)
                return;
            daemon_send({{"command", "bt_airpods_handoff"},
                         {"enabled", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)) == TRUE}});
        }

        void on_airpods_mode_toggled(GtkWidget* button, gpointer data) {
            if (g_devices.airpods_syncing || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)))
                return;
            daemon_send({{"command", "bt_airpods_mode"}, {"mode", AIRPODS_MODES[GPOINTER_TO_INT(data)].wire}});
        }

        void on_airpods_connect_click(GtkWidget* button, gpointer) {
            const nlohmann::json* device = find_bt_device(g_devices.selected_bt_address);
            if (!device)
                return;
            if (!daemon_send({{"command", "bt_airpods_connect"},
                              {"address", g_devices.selected_bt_address},
                              {"connect", !device->value("connected", false)}})) {
                set_status_main(_("Could not reach the Tether daemon."));
                return;
            }
            // Held until the result, so a slow Connect is not sent twice.
            g_devices.airpods_connecting = true;
            gtk_widget_set_sensitive(button, FALSE);
        }

        void update_airpods_pane() {
            const nlohmann::json& airpods = g_devices.bt_airpods;
            const bool managed = g_devices.bt_status.value("airpods_enabled", false);
            set_markup(g_devices.lbl_airpods_name,
                       "<b>" + escape_markup(json_string(airpods, "name", g_devices.selected_bt_name)) + "</b>");

            const std::string levels = airpods_status_text(airpods);
            set_battery_label(g_devices.lbl_airpods_battery,
                              airpods,
                              !managed         ? ""
                              : levels.empty() ? _("Reading battery\u2026")
                                               : levels);

            // bluez link action, so it works whether or not tether manages the buds
            const nlohmann::json* device = find_bt_device(g_devices.selected_bt_address);
            gtk_button_set_label(GTK_BUTTON(g_devices.btn_airpods_connect),
                                 device && device->value("connected", false) ? _("Disconnect") : _("Connect"));
            gtk_widget_set_sensitive(g_devices.btn_airpods_connect, device && !g_devices.airpods_connecting);

            const std::string current = json_string(airpods, "anc");
            g_devices.airpods_syncing = true;
            for (int i = 0; i < 4; ++i) {
                GtkWidget* button = g_devices.airpods_mode_buttons[i];
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), current == AIRPODS_MODES[i].wire);
                gtk_widget_set_sensitive(button, managed && !current.empty());
            }
            g_devices.airpods_syncing = false;

            // Reuses the daemon's own sentence rather than inventing a second one.
            const char* reason = !managed ? _("Tether is not managing the AirPods, so another program can use them.")
                                 : !current.empty() ? ""
                                 : json_string(airpods, "address").empty()
                                     ? _("No AirPods are connected.")
                                     : _("These AirPods do not report a listening mode.");
            set_text(g_devices.lbl_airpods_reason, reason);

            // Which bud is "primary" moves between them, so this counts rather
            // than naming a side.
            const int in_ear = airpods.value("in_ear", 0);
            const nlohmann::json ear = airpods.value("ear", nlohmann::json::object());
            const bool ear_known = json_string(ear, "primary", "unknown") != "unknown" ||
                                   json_string(ear, "secondary", "unknown") != "unknown";
            set_text(g_devices.lbl_airpods_worn,
                     !ear_known    ? _("These AirPods do not report whether they are being worn.")
                     : in_ear == 2 ? _("Both buds are in.")
                     : in_ear == 1 ? _("One bud is in.")
                                   : _("Neither bud is in."));

            const std::string pause = g_devices.bt_status.value("airpods_pause", "never");
            g_devices.airpods_syncing = true;
            for (int i = 0; i < 3; ++i) {
                if (pause == AIRPODS_PAUSE_MODES[i])
                    gtk_combo_box_set_active(GTK_COMBO_BOX(g_devices.cmb_airpods_pause), i);
            }
            gtk_widget_set_sensitive(g_devices.cmb_airpods_pause, managed && ear_known);

            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_devices.chk_airpods_enabled), managed);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_devices.chk_airpods_handoff),
                                         g_devices.bt_status.value("airpods_handoff", false));
            // Ownership handoff runs off the buds' own state; only the disconnect
            // fallback needs the iPhone's call state.
            const bool apple_id = g_devices.bt_status.value("apple_device_id", false);
            const bool calls_on = g_devices.bt_status.value("calls_enabled", false);
            gtk_widget_set_sensitive(g_devices.chk_airpods_handoff, managed && (apple_id || calls_on));

            // Apple's own handoff needs the controller to look like a Mac; without it
            // the buds are handed over by disconnecting them.
            set_text(g_devices.lbl_airpods_apple_id,
                     apple_id ? _("The buds are handed over with Apple's own handoff.")
                              : _("The buds are handed over by disconnecting them. For Apple's own handoff, put "
                                  "DeviceID = bluetooth:004C:0000:0000 under [General] in /etc/bluetooth/main.conf "
                                  "and restart bluetooth."));
            g_devices.airpods_syncing = false;
        }

        void update_bt_pane() {
            const std::string address = g_devices.selected_bt_address;
            const nlohmann::json* device = find_bt_device(address);
            const bool paired = device && device->value("paired", false);
            const bool supervised = is_supervised_bt_device(address);
            const bool available = g_devices.bt_status.value("available", false);
            const bool bt_on = g_devices.bt_status.value("enabled", true);
            const bool ancs_on = g_devices.bt_status.value("ancs_enabled", true);

            set_markup(g_devices.lbl_bt_name, "<b>" + escape_markup(g_devices.selected_bt_name) + "</b>");

            const nlohmann::json capability = g_devices.bt_status.value("capability", nlohmann::json());

            std::string what;
            std::string commands;
            if (capability.is_object() && capability.contains("setup")) {
                int n = 0;
                for (const auto& step : capability["setup"]) {
                    if (!what.empty())
                        what += "\n";
                    what += std::to_string(++n) + ". " + step.value("what", "");
                    if (!commands.empty())
                        commands += "\n";
                    commands += step.value("command", "");
                }
            }
            g_devices.bt_setup_commands = commands;
            set_text(g_devices.lbl_bt_setup_what, what);
            set_text(g_devices.lbl_bt_setup_command, commands);

            std::string mode;
            if (!g_devices.bt_status.empty()) {
                if (!available || capability.is_null()) {
                    mode = _("Bluetooth is unavailable on this machine.");
                } else if (capability.value("mode", "") == "full") {
                    mode = _("Full mode \u2014 messages, contacts, and notifications.");
                } else if (capability.value("mode", "") == "compatibility") {
                    mode = _("Compatibility mode \u2014 messages and contacts, no notification mirroring.");
                } else {
                    mode = _("This machine cannot carry the Bluetooth features.");
                }
                // The reasons name what is missing (adapter class, LE roles, BlueZ
                // version) and are the only place that detail surfaces outside the CLI.
                if (!capability.is_null() && capability.contains("reasons")) {
                    for (const auto& reason : capability["reasons"]) {
                        if (reason.is_string())
                            mode += "\n" + reason.get<std::string>();
                    }
                }
            }

            // A package upgrade replaces the binaries but leaves the old tetherd running.
            if (!g_devices.bt_status.empty()) {
                const std::string running = g_devices.bt_status.value("version", "");
                if (running != TETHER_VERSION) {
                    if (!mode.empty())
                        mode += "\n";
                    // No version field at all means a daemon older than the field itself.
                    mode += tr_format(_("The running tetherd is {}, but this is tether {}. Stop it and it\n"
                                        "restarts on demand:\n"
                                        "    pkill tetherd\n"),
                                      running.empty() ? _("an older build") : running.c_str(),
                                      TETHER_VERSION);
                }
            }
            set_text(g_devices.lbl_bt_mode, mode);

            // Only the supervised bond has live profile state; anything else can
            // report what BlueZ knows and no more.
            const nlohmann::json& connection = g_devices.bt_connection;
            const bool classic = supervised ? connection.value("classic_connected", false)
                                            : (device && device->value("classic_connected", false));
            const bool le =
                supervised ? connection.value("le_connected", false) : (device && device->value("le_connected", false));
            const bool map_open = supervised && connection.value("map_open", false);
            const bool pbap_open = supervised && connection.value("pbap_open", false);
            const bool ancs_ready = supervised && connection.value("ancs_ready", false);

            const bool all_live = map_open && pbap_open && (ancs_ready || !ancs_on);
            gtk_widget_set_visible(g_devices.bt_setup_box, !commands.empty() && !all_live);

            set_capability_row(g_devices.lbl_bt_link,
                               _("Link"),
                               classic || le,
                               std::string("BR/EDR ") + (classic ? "on" : "off") + ", LE " + (le ? "on" : "off"));

            const std::string map_error = connection.value("map_error", "none");
            const std::string pbap_error = connection.value("pbap_error", "none");
            set_capability_row(
                g_devices.lbl_bt_messages, _("Messages"), map_open, map_open || map_error == "none" ? "" : map_error);
            set_capability_row(g_devices.lbl_bt_contacts,
                               _("Contacts"),
                               pbap_open,
                               pbap_open || pbap_error == "none" ? "" : pbap_error);
            set_capability_row(g_devices.lbl_bt_notifications,
                               _("Notifications"),
                               ancs_ready,
                               ancs_ready ? "" : connection.value("ancs_reason", ""));

            // The daemon's reasons name the actual next step, so they are shown
            // verbatim rather than replaced with something vaguer.
            std::string reason;
            if (!paired) {
                reason = _("Not paired over Bluetooth yet.");
            } else if (!supervised) {
                reason = _("Tether is not using this device. Pair it to select it.");
            } else if (!bt_on) {
                // Supervision is idle while switched off, so its status would
                // otherwise report no device selected.
                reason = _("Bluetooth is switched off for this iPhone.");
            } else {
                reason = connection_reason(connection);
            }
            set_text(g_devices.lbl_bt_reason, reason);

            gtk_widget_set_visible(g_devices.btn_bt_pair, available && (!paired || !supervised));
            gtk_widget_set_visible(g_devices.btn_bt_unpair, available && paired);
            // Re-soliciting only helps when the phone is withholding a profile,
            // not when the link itself is down or nothing is bonded.
            gtk_widget_set_visible(g_devices.btn_bt_solicit,
                                   available && supervised &&
                                       (map_error == "forbidden" || map_error == "no_record" || !ancs_ready));

            gtk_widget_set_visible(g_devices.chk_bt_enabled, available && supervised);

            g_signal_handlers_block_by_func(
                g_devices.chk_bt_enabled, reinterpret_cast<gpointer>(on_bt_enabled_toggled), nullptr);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_devices.chk_bt_enabled), bt_on);
            g_signal_handlers_unblock_by_func(
                g_devices.chk_bt_enabled, reinterpret_cast<gpointer>(on_bt_enabled_toggled), nullptr);
        }

        void update_right_pane() {
            if (!g_devices.selected_bt_address.empty()) {
                const nlohmann::json* device = find_bt_device(g_devices.selected_bt_address);
                if (device && device->value("airpods", false)) {
                    gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "airpods");
                    update_airpods_pane();
                    return;
                }
                gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "bluetooth");
                update_bt_pane();
                return;
            }

            if (g_devices.selected_device_fp.empty()) {
                update_welcome_pane();
                gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "placeholder");
                return;
            }

            bool is_paired = false;
            for (const auto& p : g_devices.paired_devices) {
                if (p.first == g_devices.selected_device_fp)
                    is_paired = true;
            }

            if (is_paired) {
                gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "action");
                set_markup(g_devices.lbl_action_name, ("<b>" + escape_markup(g_devices.selected_device_name) + "</b>"));
                bool online = std::find(g_devices.connected_fps.begin(),
                                        g_devices.connected_fps.end(),
                                        g_devices.selected_device_fp) != g_devices.connected_fps.end();
                if (g_devices.select_bt_after_scan)
                    set_status_action(_("Scanning for nearby devices..."));
                else
                    set_status_action(
                        online ? _("Connected and ready.")
                               : _("Device is offline. Make sure it is switched on and on the same network."));
                if (g_devices.btn_grid) {
                    gtk_widget_set_visible(g_devices.btn_grid, online);
                }
                if (g_devices.btn_action_bt_pair) {
                    const std::string supervised = g_devices.bt_status.value("device_address", "");
                    gtk_widget_set_visible(g_devices.btn_action_bt_pair, supervised.empty());
                }
                update_action_bt_scan_controls();
            } else {
                gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "pair");
                set_markup(g_devices.lbl_unpaired_name,
                           ("<b>" + escape_markup(g_devices.selected_device_name) + "</b>"));
                // TRANSLATORS: {0} is an IP address, {1} a port number.
                set_text(g_devices.lbl_unpaired_ip,
                         tether::tr_format(
                             _("Found at {0}:{1}"), g_devices.selected_device_ip, g_devices.selected_device_port));
            }
        }

        void on_device_selected(GtkListBox*, GtkListBoxRow* row, gpointer) {
            if (!row)
                return;

            set_bt_progress("");

            const char* bt_address = (const char*)g_object_get_data(G_OBJECT(row), "bt_address");
            g_devices.selected_bt_address = bt_address ? bt_address : "";
            if (!g_devices.selected_bt_address.empty()) {
                const char* bt_name = (const char*)g_object_get_data(G_OBJECT(row), "name");
                g_devices.selected_bt_name = bt_name ? bt_name : "";
                g_devices.selected_device_fp.clear();
                update_right_pane();
                return;
            }

            const char* fp = (const char*)g_object_get_data(G_OBJECT(row), "fp");
            const char* name = (const char*)g_object_get_data(G_OBJECT(row), "name");
            const char* ip = (const char*)g_object_get_data(G_OBJECT(row), "ip");
            g_devices.selected_device_fp = fp ? fp : "";
            g_devices.selected_device_name = name ? name : "";
            g_devices.selected_device_ip = ip ? ip : "";
            g_devices.selected_device_port = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "port"));
            update_right_pane();
        }

        void on_pair_click(GtkWidget*, gpointer) {
            if (g_devices.selected_device_ip.empty())
                return;
            set_text(g_devices.lbl_unpaired_ip, _("Sending pair request..."));
            nlohmann::json j;
            j["command"] = "pair_request";
            j["host"] = g_devices.selected_device_ip;
            j["port"] = g_devices.selected_device_port;
            j["device_name"] = g_devices.selected_device_name;
            daemon_send(j);
            set_status_main(_("Pair request sent. Approve on remote device!"));
        }

        void on_accept_click(GtkWidget*, gpointer) {
            if (g_devices.selected_device_fp.empty())
                return;
            nlohmann::json j;
            j["command"] = "accept_device";
            j["fingerprint"] = g_devices.selected_device_fp;
            j["device_name"] = g_devices.selected_device_name;
            daemon_send(j);
        }

        std::string new_bt_operation_id() {
            gchar* uuid = g_uuid_string_random();
            std::string operation_id = uuid;
            g_free(uuid);
            return operation_id;
        }

        void on_bt_pair_click(GtkWidget*, gpointer) {
            if (g_devices.selected_bt_address.empty())
                return;
            g_devices.bt_operation_id = new_bt_operation_id();
            if (!daemon_send({{"command", "bt_pair"},
                              {"address", g_devices.selected_bt_address},
                              {"operation_id", g_devices.bt_operation_id}})) {
                g_devices.bt_operation_id.clear();
                set_bt_progress(_("Could not reach the Tether daemon."));
                return;
            }
            gtk_widget_set_sensitive(g_devices.btn_bt_pair, FALSE);
            set_bt_progress(_("Pairing\u2026 confirm the prompt on the iPhone."));
        }

        void on_bt_unpair_click(GtkWidget*, gpointer) {
            if (g_devices.selected_bt_address.empty())
                return;

            GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(main_window()),
                                                       GTK_DIALOG_MODAL,
                                                       GTK_MESSAGE_QUESTION,
                                                       GTK_BUTTONS_OK_CANCEL,
                                                       _("Remove the Bluetooth pairing with %s?"),
                                                       g_devices.selected_bt_name.c_str());
            gtk_message_dialog_format_secondary_text(
                GTK_MESSAGE_DIALOG(dialog),
                _("Also delete this computer from the iPhone's Bluetooth settings before pairing again, "
                  "or the phone will keep the stale bond."));
            const bool confirmed = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK;
            gtk_widget_destroy(dialog);
            if (!confirmed)
                return;

            g_devices.bt_operation_id = new_bt_operation_id();
            daemon_send({{"command", "bt_unpair"},
                         {"address", g_devices.selected_bt_address},
                         {"operation_id", g_devices.bt_operation_id}});
            set_bt_progress(_("Removing the pairing\u2026"));
        }

        void on_forget_click(GtkWidget*, gpointer) {
            if (g_devices.selected_device_fp.empty())
                return;

            GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(main_window()),
                                                       GTK_DIALOG_MODAL,
                                                       GTK_MESSAGE_QUESTION,
                                                       GTK_BUTTONS_OK_CANCEL,
                                                       _("Forget %s?"),
                                                       g_devices.selected_device_name.c_str());
            gtk_message_dialog_format_secondary_text(
                GTK_MESSAGE_DIALOG(dialog),
                _("Tether will stop connecting to it. Forget this computer on the other device too, "
                  "then pair again to reconnect."));
            const bool confirmed = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK;
            gtk_widget_destroy(dialog);
            if (!confirmed)
                return;

            daemon_send({{"command", "forget_device"}, {"fingerprint", g_devices.selected_device_fp}});
        }

        void on_bt_enabled_toggled(GtkWidget* widget, gpointer) {
            daemon_send({{"command", "bt_set_enabled"},
                         {"enabled", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) == TRUE}});
        }

        void on_bt_setup_copy_click(GtkWidget*, gpointer) {
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), g_devices.bt_setup_commands.c_str(), -1);
            set_status_main(_("Setup commands copied to the clipboard."));
        }

        void on_bt_solicit_click(GtkWidget*, gpointer) {
            if (!daemon_send({{"command", "bt_solicit"}})) {
                set_bt_progress(_("Could not reach the Tether daemon."));
                return;
            }
            set_bt_progress(_("Asking the iPhone to show its Bluetooth permissions\u2026"));
        }

        // The daemon opens a fresh connection per send_file, batch one file at a time
        void start_next_send() {
            if (g_devices.send_queue.empty())
                return;
            const std::filesystem::path& path = g_devices.send_queue.front();
            // TRANSLATORS: {} is a file name.
            std::string status = tether::tr_format(_("Sending {}…"), path.filename().string());
            if (g_devices.send_batch_total > 1) {
                const size_t index = g_devices.send_batch_total - g_devices.send_queue.size() + 1;
                // TRANSLATORS: {0} is the current file's position, {1} the batch size.
                status += " " + tether::tr_format(_("({0} of {1})"), index, g_devices.send_batch_total);
            }
            set_status_action(status);
            nlohmann::json j;
            j["command"] = "send_file";
            j["path"] = path.string();
            daemon_send(j);
        }

        // skipped counts items that came in with the batch but are not sendable
        // files, so the final status can admit they were dropped.
        void queue_files(std::vector<std::filesystem::path> paths, size_t skipped = 0) {
            if (paths.empty())
                return;
            if (!daemon_connected()) {
                set_status_action(_("Daemon unavailable."));
                return;
            }
            const bool idle = g_devices.send_queue.empty();
            if (idle) {
                g_devices.send_batch_total = paths.size();
                g_devices.send_failed = 0;
                g_devices.send_skipped = skipped;
                g_devices.send_last_error.clear();
            } else {
                g_devices.send_batch_total += paths.size();
                g_devices.send_skipped += skipped;
            }
            for (auto& path : paths)
                g_devices.send_queue.push_back(std::move(path));
            if (idle)
                start_next_send();
        }

        void on_choose_file(GtkWidget*, gpointer) {
            GtkWidget* dialog = gtk_file_chooser_dialog_new(_("Send File"),
                                                            GTK_WINDOW(main_window()),
                                                            GTK_FILE_CHOOSER_ACTION_OPEN,
                                                            _("_Cancel"),
                                                            GTK_RESPONSE_CANCEL,
                                                            _("_Send"),
                                                            GTK_RESPONSE_ACCEPT,
                                                            nullptr);
            gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);
            std::vector<std::filesystem::path> files;
            if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
                GSList* names = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
                for (GSList* node = names; node; node = node->next) {
                    files.emplace_back(static_cast<char*>(node->data));
                    g_free(node->data);
                }
                g_slist_free(names);
            }
            gtk_widget_destroy(dialog);
            queue_files(std::move(files));
        }

        void set_dropzone_active(bool active) {
            if (!g_devices.dropzone)
                return;
            GtkStyleContext* ctx = gtk_widget_get_style_context(g_devices.dropzone);
            if (active)
                gtk_style_context_add_class(ctx, "tether-dropzone-active");
            else
                gtk_style_context_remove_class(ctx, "tether-dropzone-active");
        }

        gboolean on_drop_motion(GtkWidget*, GdkDragContext*, gint, gint, guint, gpointer) {
            set_dropzone_active(true);
            return FALSE; // let GTK_DEST_DEFAULT_MOTION answer the drag
        }

        void on_drop_leave(GtkWidget*, GdkDragContext*, guint, gpointer) { set_dropzone_active(false); }

        void on_drop_received(
            GtkWidget*, GdkDragContext* context, gint, gint, GtkSelectionData* data, guint, guint time, gpointer) {
            // drag-leave is not guaranteed once the drop lands.
            set_dropzone_active(false);

            gchar** uris = gtk_selection_data_get_uris(data);
            if (!uris) {
                gtk_drag_finish(context, FALSE, FALSE, time);
                return;
            }

            std::vector<std::filesystem::path> files;
            size_t skipped = 0;
            for (gchar** uri = uris; *uri; ++uri) {
                // Non-file:// URIs have no local path and cannot be streamed.
                gchar* local = g_filename_from_uri(*uri, nullptr, nullptr);
                if (!local) {
                    ++skipped;
                    continue;
                }
                std::filesystem::path path(local);
                g_free(local);
                std::error_code ec;
                if (std::filesystem::is_directory(path, ec))
                    ++skipped;
                else
                    files.push_back(std::move(path));
            }
            g_strfreev(uris);

            gtk_drag_finish(context, !files.empty(), FALSE, time);
            if (files.empty()) {
                set_status_action(_("Folders can't be sent — drop files instead."));
                return;
            }
            queue_files(std::move(files), skipped);
        }

        void update_wifi_indicator() {
            const bool connected = !g_devices.connected_fps.empty();
            if (connected)
                g_devices.wifi_detail = g_devices.clipboard_ok
                                            ? _("Clipboard, files, and OTP are connected.")
                                            : _("Files and OTP are connected. This compositor does not give Tether "
                                                "clipboard access, so clipboard sync is off.");
            else if (!g_devices.mdns_ok)
                g_devices.wifi_detail = _("avahi-daemon isn't running, so other devices can't find this PC. Start it "
                                          "with: sudo systemctl enable --now avahi-daemon");
            else if (g_devices.firewall_active && !g_devices.discovered_devices.empty())
                g_devices.wifi_detail = _("A device is on the network but cannot reach this PC. A firewall is "
                                          "running; Tether needs inbound TCP 5134. Allow it with: sudo ufw allow "
                                          "5134/tcp");
            else
                g_devices.wifi_detail = _("No paired device is connected. Switch it on and join the same network.");
            set_route_status(Route::WiFi, connected, g_devices.wifi_detail);
            update_welcome_pane();
        }

        void apply_state_snapshot(const nlohmann::json& j) {

            g_devices.mdns_ok = j.value("mdns_available", true);
            g_devices.clipboard_ok = j.value("clipboard_available", true);
            g_devices.firewall_active = j.value("firewall_active", false);

            if (j.contains("discovered_devices") && j["discovered_devices"].is_array()) {
                g_devices.discovered_devices.clear();
                for (const auto& d : j["discovered_devices"]) {
                    tether::DiscoveredDevice dev;
                    dev.name = d.value("name", "");
                    dev.fingerprint = d.value("fingerprint", "");
                    if (d.contains("addresses") && d["addresses"].is_array()) {
                        for (const auto& a : d["addresses"])
                            dev.addresses.push_back({a.value("address", ""), a.value<uint16_t>("port", 5134)});
                    }
                    g_devices.discovered_devices.push_back(dev);
                }
            }

            g_devices.connected_fps.clear();
            g_devices.unpaired_clients.clear();
            if (j.contains("connected_clients") && j["connected_clients"].is_array()) {
                for (auto& c : j["connected_clients"]) {
                    if (c.value("paired", false)) {
                        g_devices.connected_fps.push_back(c.value("fingerprint", ""));
                    } else {
                        tether::DiscoveredDevice dev;
                        dev.fingerprint = c.value("fingerprint", "");
                        dev.name = c.value("device_name", _("Unknown Device"));
                        dev.addresses.push_back({c.value("address", ""), 5134});
                        g_devices.unpaired_clients.push_back(dev);
                    }
                }
            }
            g_devices.pending_pairing_requests.clear();
            if (j.contains("pending_pairs") && j["pending_pairs"].is_array()) {
                for (auto& p : j["pending_pairs"]) {
                    tether::DiscoveredDevice req;
                    req.fingerprint = p.value("fingerprint", "");
                    req.name = p.value("device_name", _("Unknown Device"));
                    g_devices.pending_pairing_requests.push_back(req);
                }
            }
            devices_view_refresh();
            update_wifi_indicator();
        }

    } // namespace

    void devices_view_handle_disconnect() {
        set_bt_progress("");
        g_devices.airpods_connecting = false;
        g_devices.send_queue.clear();
        g_devices.send_batch_total = 0;
        g_devices.send_failed = 0;
        g_devices.send_skipped = 0;
        g_devices.send_last_error.clear();
    }

    void devices_view_trigger_discovery() {
        set_status_main(_("Scanning for nearby devices..."));
        // Refresh means both routes: mDNS for Wi-Fi peers, and a real BlueZ discovery for Bluetooth.
        daemon_send({{"command", "discover"}});
        daemon_send({{"command", "bt_status"}});
        daemon_send({{"command", "bt_scan"}});
    }

    // Refresh the device list based on Discovered (unpaired), Paired (offline), Paired (online)
    void devices_view_refresh() {
        clear_list_box(g_devices.list_devices);

        tether::Crypto::instance().init();
        try {
            nlohmann::json known = nlohmann::json::parse(tether::Crypto::instance().get_known_hosts_dump());
            g_devices.paired_devices.clear();
            if (!known.empty()) {
                for (auto& [fingerprint, name_value] : known.items()) {
                    g_devices.paired_devices.push_back(
                        {fingerprint, name_value.is_string() ? name_value.get<std::string>() : _("Unknown Device")});
                }
            }
        } catch (...) {
        }

        // We build a unified list of devices.
        // 1. All Paired Devices
        // 2. Any Discovered Devices not in the Paired list

        auto create_row = [](const std::string& name,
                             const std::string& fp,
                             const std::string& ip,
                             uint16_t port,
                             bool paired,
                             bool online) {
            GtkWidget* row = gtk_list_box_row_new();
            g_object_set_data_full(G_OBJECT(row), "fp", g_strdup(fp.c_str()), g_free);
            g_object_set_data_full(G_OBJECT(row), "name", g_strdup(name.c_str()), g_free);
            g_object_set_data_full(G_OBJECT(row), "ip", g_strdup(ip.c_str()), g_free);
            g_object_set_data(G_OBJECT(row), "port", GINT_TO_POINTER(port));
            g_object_set_data(G_OBJECT(row), "paired", GINT_TO_POINTER(paired ? 1 : 0));

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
            gtk_container_set_border_width(GTK_CONTAINER(box), 12);

            const char* icon_name =
                paired ? (online ? "network-wireless-signal-excellent-symbolic" : "network-wireless-offline-symbolic")
                       : "network-wireless-acquiring-symbolic";
            GtkWidget* icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
            gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);

            GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget* title = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(title), ("<b>" + escape_markup(name) + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(title), 18);
            gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

            GtkWidget* subtitle =
                gtk_label_new(paired ? (online ? _("Connected") : _("Offline")) : _("Nearby (Tap to Pair)"));
            gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
            gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
            gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);

            gtk_box_pack_start(GTK_BOX(box), labels, TRUE, TRUE, 0);
            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        };

        auto create_bt_row = [](const nlohmann::json& device) {
            const std::string address = device.value("address", "");
            const std::string name = device.value("name", "").empty() ? address : device.value("name", "");
            const bool paired = device.value("paired", false);
            const bool connected = device.value("connected", false);
            const bool airpods = device.value("airpods", false);
            const bool route_ready =
                is_supervised_bt_device(address) && (g_devices.bt_connection.value("map_open", false) ||
                                                     g_devices.bt_connection.value("ancs_ready", false));

            GtkWidget* row = gtk_list_box_row_new();
            g_object_set_data_full(G_OBJECT(row), "bt_address", g_strdup(address.c_str()), g_free);
            g_object_set_data_full(G_OBJECT(row), "name", g_strdup(name.c_str()), g_free);

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
            gtk_container_set_border_width(GTK_CONTAINER(box), 12);

            const char* icon_name = airpods     ? "audio-headphones-symbolic"
                                    : connected ? "bluetooth-active-symbolic"
                                                : "bluetooth-symbolic";
            GtkWidget* icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
            gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);

            GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget* title = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(title), ("<b>" + escape_markup(name) + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(title), 18);
            gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

            // "Partially connected" is about the iPhone's message and notification
            // routes, which say nothing about a pair of earbuds.
            const char* subtitle_text = (route_ready || (airpods && connected)) ? _("Connected")
                                        : connected                             ? _("Partially connected")
                                        : paired                                ? _("Paired")
                                                                                : _("Nearby (Tap to Pair)");
            GtkWidget* subtitle = gtk_label_new(subtitle_text);
            gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
            gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
            gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);

            if (airpods) {
                GtkWidget* battery = gtk_label_new(nullptr);
                gtk_label_set_xalign(GTK_LABEL(battery), 0.0);
                gtk_style_context_add_class(gtk_widget_get_style_context(battery), "muted");
                if (g_devices.bt_airpods.value("address", "") == address)
                    set_battery_label(battery, g_devices.bt_airpods, airpods_row_text(g_devices.bt_airpods));
                g_object_set_data(G_OBJECT(row), "bt_battery", battery);
                gtk_box_pack_start(GTK_BOX(labels), battery, FALSE, FALSE, 0);
            }

            gtk_box_pack_start(GTK_BOX(box), labels, TRUE, TRUE, 0);
            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        };

        auto create_header_row = [](const std::string& title_text) {
            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
            // A heading, not a device: keyboard focus skips it.
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
            gtk_widget_set_can_focus(row, FALSE);
            atk_object_set_role(gtk_widget_get_accessible(row), ATK_ROLE_HEADING);
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_container_set_border_width(GTK_CONTAINER(box), 8);
            GtkWidget* lbl = gtk_label_new(nullptr);
            gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "muted");
            gtk_label_set_markup(GTK_LABEL(lbl),
                                 ("<b><span size='small'>" + escape_markup(title_text) + "</span></b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        };

        std::string my_fp = tether::Crypto::instance().get_my_fingerprint();

        std::vector<GtkWidget*> connected_rows;
        std::vector<GtkWidget*> remembered_rows;
        std::vector<GtkWidget*> discovered_rows;

        for (const auto& paired : g_devices.paired_devices) {
            bool online = std::find(g_devices.connected_fps.begin(), g_devices.connected_fps.end(), paired.first) !=
                          g_devices.connected_fps.end();
            GtkWidget* r = create_row(paired.second, paired.first, "", 5134, true, online);
            if (online) {
                connected_rows.push_back(r);
            } else {
                remembered_rows.push_back(r);
            }
        }

        std::vector<std::string> shown_fps;
        for (const auto* source : {&g_devices.discovered_devices, &g_devices.unpaired_clients}) {
            for (const auto& disc : *source) {
                if (disc.fingerprint == my_fp)
                    continue;
                if (std::find(shown_fps.begin(), shown_fps.end(), disc.fingerprint) != shown_fps.end())
                    continue;
                bool is_paired = false;
                for (const auto& p : g_devices.paired_devices) {
                    if (p.first == disc.fingerprint) {
                        is_paired = true;
                        break;
                    }
                }
                if (is_paired)
                    continue; // already added above
                shown_fps.push_back(disc.fingerprint);
                std::string ip = disc.addresses.empty() ? "" : disc.addresses[0].address;
                uint16_t port = disc.addresses.empty() ? 5134 : disc.addresses[0].port;
                GtkWidget* r = create_row(disc.name, disc.fingerprint, ip, port, false, false);
                discovered_rows.push_back(r);
            }
        }

        // Render Pending Pair Requests
        for (const auto& req : g_devices.pending_pairing_requests) {
            bool is_paired = false;
            for (const auto& p : g_devices.paired_devices) {
                if (p.first == req.fingerprint) {
                    is_paired = true;
                    break;
                }
            }
            if (is_paired)
                continue; // ignore if they successfully paired

            if (std::find(shown_fps.begin(), shown_fps.end(), req.fingerprint) != shown_fps.end())
                continue; // ignore if mDNS or a live connection already populated it

            std::string ip = req.addresses.empty() ? "" : req.addresses[0].address;
            uint16_t port = req.addresses.empty() ? 5134 : req.addresses[0].port;
            GtkWidget* r = create_row(req.name, req.fingerprint, ip, port, false, false);
            discovered_rows.push_back(r);
        }

        if (!connected_rows.empty() || !remembered_rows.empty() || !discovered_rows.empty()) {
            gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), create_header_row("WI-FI"), -1);
            for (const auto* rows : {&connected_rows, &remembered_rows, &discovered_rows})
                for (auto* r : *rows)
                    gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), r, -1);
        }

        std::vector<GtkWidget*> bt_rows;
        for (const auto& device : g_devices.bt_devices) {
            if (!device.value("iphone", false) && !device.value("airpods", false) &&
                !is_supervised_bt_device(device.value("address", "")))
                continue;
            bt_rows.push_back(create_bt_row(device));
        }

        if (!bt_rows.empty()) {
            gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), create_header_row("BLUETOOTH"), -1);
            for (auto* r : bt_rows)
                gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), r, -1);
        }

        gtk_widget_show_all(g_devices.list_devices);
    }

    bool devices_view_handle_event(const nlohmann::json& event) {
        const std::string command = event.value("command", "");
        const bool pairing_event = command == "bt_pair_progress" || command == "bt_pair_confirm_request" ||
                                   command == "bt_pair_result" || command == "bt_unpair_result";
        if (pairing_event && event.contains("operation_id") &&
            (g_devices.bt_operation_id.empty() || event.value("operation_id", "") != g_devices.bt_operation_id))
            return false;
        if (command == "state_snapshot") {
            apply_state_snapshot(event);
            return true;
        }
        if (command == "mdns_status") {
            g_devices.mdns_ok = event.value("available", true);
            update_wifi_indicator();
            return true;
        }
        if (command == "client_connected") {
            std::string fp = event.value("fingerprint", "");
            if (std::find(g_devices.connected_fps.begin(), g_devices.connected_fps.end(), fp) ==
                g_devices.connected_fps.end()) {
                g_devices.connected_fps.push_back(fp);
            }
            devices_view_refresh();
            update_right_pane();
            update_wifi_indicator();
            return true;
        }
        if (command == "client_disconnected") {
            std::string fp = event.value("fingerprint", "");
            g_devices.connected_fps.erase(
                std::remove(g_devices.connected_fps.begin(), g_devices.connected_fps.end(), fp),
                g_devices.connected_fps.end());
            devices_view_refresh();
            update_right_pane();
            update_wifi_indicator();
            return true;
        }
        if (command == "pair_outbound_pending") {
            // TRANSLATORS: {0} is the name of the device being paired with.
            const std::string waiting =
                tether::tr_format(_("Waiting for approval on {0}..."), event.value("device_name", ""));
            set_text(g_devices.lbl_unpaired_ip, waiting);
            set_status_main(waiting);
            return true;
        }
        if (command == "pair_rejected") {

            const std::string reason = event.value("reason", "");
            std::string message;
            if (reason == "unreachable")
                message = _("Could not reach that device. If a firewall is running, Tether needs inbound TCP "
                            "5134 on the other machine.");
            else if (reason == "refused")
                message = _("That device refused the connection. Is Tether running on it?");
            else if (reason == "unresolved")
                message = _("That address could not be resolved.");
            else if (reason == "failed")
                message = _("Could not connect to that device.");
            else
                message = _("Pair request was rejected.");
            set_text(g_devices.lbl_unpaired_ip, message);
            set_status_main(message);
            return true;
        }
        if (command == "pair_request_received" || command == "untrusted_client_connected") {
            tether::DiscoveredDevice req;
            req.fingerprint = event.value("fingerprint", "");

            std::string resolved_name = event.value("device_name", _("Unknown Device"));
            if (resolved_name == _("Unknown Device")) {
                for (const auto& d : g_devices.discovered_devices) {
                    if (d.fingerprint == req.fingerprint && !d.name.empty()) {
                        resolved_name = d.name;
                        break;
                    }
                }
            }
            req.name = resolved_name;
            req.addresses.push_back({event.value("address", ""), 5134});
            bool exists = false;
            for (const auto& dev : g_devices.pending_pairing_requests) {
                if (dev.fingerprint == req.fingerprint) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                g_devices.pending_pairing_requests.push_back(req);
                devices_view_refresh();
            }
            return true;
        }
        if (command == "pair_accepted") {
            std::string fp = event.value("fingerprint", "");
            if (!fp.empty()) {
                if (event.value("connected", false) &&
                    std::find(g_devices.connected_fps.begin(), g_devices.connected_fps.end(), fp) ==
                        g_devices.connected_fps.end())
                    g_devices.connected_fps.push_back(fp);

                // paired_devices is rebuilt from known_hosts.json on every refresh, and
                // the daemon only emits this after writing that file.
                g_devices.pending_pairing_requests.erase(
                    std::remove_if(g_devices.pending_pairing_requests.begin(),
                                   g_devices.pending_pairing_requests.end(),
                                   [&](const tether::DiscoveredDevice& d) { return d.fingerprint == fp; }),
                    g_devices.pending_pairing_requests.end());
                devices_view_refresh();
                update_right_pane();
            }
            return true;
        }
        if (command == "forget_device_result") {
            const std::string fp = event.value("fingerprint", "");
            g_devices.connected_fps.erase(
                std::remove(g_devices.connected_fps.begin(), g_devices.connected_fps.end(), fp),
                g_devices.connected_fps.end());
            if (g_devices.selected_device_fp == fp) {
                g_devices.selected_device_fp.clear();
                g_devices.selected_device_name.clear();
                g_devices.selected_device_ip.clear();
            }
            // paired_devices is rebuilt from known_hosts.json the daemon has already written.
            devices_view_refresh();
            update_right_pane();
            update_wifi_indicator();
            return true;
        }
        if (command == "discovery_result") {
            g_devices.discovered_devices.clear();
            if (event.contains("devices") && event["devices"].is_array()) {
                for (const auto& d : event["devices"]) {
                    tether::DiscoveredDevice dev;
                    dev.name = d.value("name", "");
                    dev.fingerprint = d.value("fingerprint", "");
                    if (d.contains("addresses") && d["addresses"].is_array()) {
                        for (const auto& a : d["addresses"]) {
                            dev.addresses.push_back({a.value("address", ""), a.value<uint16_t>("port", 5134)});
                        }
                    }
                    g_devices.discovered_devices.push_back(dev);
                }
            }
            devices_view_refresh();
            set_status_main(_("Ready"));
            return true;
        }
        if (command == "file_send_complete") {
            if (!event.value("success", true)) {
                ++g_devices.send_failed;
                g_devices.send_last_error = event.value("message", "");
            }
            if (!g_devices.send_queue.empty())
                g_devices.send_queue.pop_front();
            if (!g_devices.send_queue.empty()) {
                start_next_send();
                return true;
            }

            // The daemon's own message is exact for a single file; a batch needs
            // a tally, otherwise a failure halfway through is never seen.
            std::string message = event.value("message", "");
            if (g_devices.send_batch_total > 1) {
                const size_t sent = g_devices.send_batch_total - g_devices.send_failed;
                // TRANSLATORS: {0} is how many were sent, {1} the batch size.
                message =
                    tether::tr_format(P_("Sent {0} of {1} file.", "Sent {0} of {1} files.", g_devices.send_batch_total),
                                      sent,
                                      g_devices.send_batch_total);
                if (g_devices.send_failed > 0 && !g_devices.send_last_error.empty())
                    message += " " + g_devices.send_last_error;
            }
            if (g_devices.send_skipped > 0)
                message +=
                    " " + tether::tr_format(
                              P_("Skipped {} non-file item.", "Skipped {} non-file items.", g_devices.send_skipped),
                              g_devices.send_skipped);
            set_status_action(message);

            g_devices.send_batch_total = 0;
            g_devices.send_failed = 0;
            g_devices.send_skipped = 0;
            g_devices.send_last_error.clear();
            return true;
        }
        if (command == "clipboard_content") {
            set_status_action(event.value("content", std::string{}).empty() ? _("Clipboard is empty.")
                                                                            : _("Desktop Clipboard Sync triggered."));
            return true;
        }
        if (command == "bt_status") {
            g_devices.bt_status = event;
            devices_view_refresh();
            update_right_pane();
            return true;
        }
        if (command == "bt_devices") {
            g_devices.bt_devices.clear();
            if (event.contains("devices") && event["devices"].is_array()) {
                for (const auto& device : event["devices"])
                    g_devices.bt_devices.push_back(device);
            }

            if (g_devices.select_bt_after_scan) {
                const auto iphone =
                    std::find_if(g_devices.bt_devices.begin(),
                                 g_devices.bt_devices.end(),
                                 [](const nlohmann::json& device) { return device.value("iphone", false); });
                if (iphone != g_devices.bt_devices.end()) {
                    g_devices.selected_bt_address = iphone->value("address", "");
                    g_devices.selected_bt_name = iphone->value("name", "iPhone");
                    g_devices.selected_device_fp.clear();
                    g_devices.select_bt_after_scan = false;
                }
            }
            devices_view_refresh();
            update_right_pane();
            return true;
        }
        if (command == "bt_airpods") {
            g_devices.bt_airpods = event;
            // Battery arrives whenever the buds feel like sending it
            const std::string address = event.value("address", "");
            const std::string text = address.empty() ? "" : airpods_row_text(event);
            tray_set_airpods(json_string(event, "name"), address.empty() ? "" : airpods_status_text(event));
            GList* rows = gtk_container_get_children(GTK_CONTAINER(g_devices.list_devices));
            for (GList* item = rows; item; item = item->next) {
                auto* row = GTK_WIDGET(item->data);
                const char* row_address = (const char*)g_object_get_data(G_OBJECT(row), "bt_address");
                auto* battery = GTK_WIDGET(g_object_get_data(G_OBJECT(row), "bt_battery"));
                if (!battery || !row_address || (!address.empty() && address != row_address))
                    continue;
                set_battery_label(battery, event, text);
            }
            g_list_free(rows);
            // The pane repaints from the same event, but only when it is showing.
            if (gtk_stack_get_visible_child_name(GTK_STACK(g_devices.right_pane_stack)) == std::string("airpods"))
                update_airpods_pane();
            return true;
        }
        if (command == "bt_connection_changed") {
            g_devices.bt_connection = event;
            const bool connected = event.value("map_open", false) || event.value("ancs_ready", false);
            std::string detail = connection_reason(event);
            if (detail.empty() && connected)
                detail = _("Messages and notifications are connected.");
            set_route_status(Route::Bluetooth, connected, detail);
            update_right_pane();
            // A status broadcast, not a command this view owns: the Messages and
            // Notifications views need the same event.
            return false;
        }
        if (command == "bt_scan_result") {
            const std::string message = event.value("message", "");
            set_status_main(message);
            set_bt_progress(message);
            // A failed scan already carries the reason; only a scan that really
            // ran and found nothing wants the "check the phone" advice.
            if (event.value("success", false) && (g_devices.bt_devices.empty() || g_devices.select_bt_after_scan)) {
                const char* advice = _("No iPhone found. Unlock the phone and open Settings > Bluetooth so it "
                                       "advertises, then scan again.");
                set_text(g_devices.lbl_welcome_bt, advice);
                if (g_devices.select_bt_after_scan)
                    set_status_action(advice);
                g_devices.select_bt_after_scan = false;
                update_action_bt_scan_controls();
            } else if (!event.value("success", false)) {
                set_text(g_devices.lbl_welcome_bt, message);
                if (g_devices.select_bt_after_scan)
                    set_status_action(message);
                g_devices.select_bt_after_scan = false;
                update_action_bt_scan_controls();
            }
            return true;
        }
        if (command == "bt_solicit_result") {
            set_bt_progress(event.value("message", ""));
            return false;
        }
        if (command == "bt_pair_confirm_request") {
            g_idle_add(
                [](gpointer data) -> gboolean {
                    std::unique_ptr<std::string> code(static_cast<std::string*>(data));
                    GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(main_window()),
                                                               GTK_DIALOG_MODAL,
                                                               GTK_MESSAGE_QUESTION,
                                                               GTK_BUTTONS_OK_CANCEL,
                                                               _("Does the iPhone show this code?\n\n%s"),
                                                               code->c_str());
                    const bool confirmed = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK;
                    gtk_widget_destroy(dialog);
                    daemon_send({{"command", "bt_pair_confirm"},
                                 {"operation_id", g_devices.bt_operation_id},
                                 {"accept", confirmed}});
                    return G_SOURCE_REMOVE;
                },
                new std::string(event.value("code", "")));
            return true;
        }
        if (command == "bt_airpods_connect_result") {
            g_devices.airpods_connecting = false;
            if (!event.value("success", false))
                set_status_main(event.value("message", ""));
            update_right_pane();
            return true;
        }
        if (command == "bt_pair_progress") {
            set_bt_progress(event.value("step", "") + "  " + event.value("detail", ""));
            return true;
        }
        if (command == "bt_pair_result" || command == "bt_unpair_result") {
            gtk_widget_set_sensitive(g_devices.btn_bt_pair, TRUE);
            g_devices.bt_operation_id.clear();
            set_bt_progress(event.value("message", ""));
            // The bond and the supervised device both just changed.
            daemon_send({{"command", "bt_status"}});
            daemon_send({{"command", "bt_list_devices"}});
            return true;
        }
        return false;
    }

    GtkWidget* devices_view_new() {
        GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

        // Left Pane (List)
        GtkWidget* left_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(left_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(left_scroll, 220, -1);
        style(left_scroll, "tether-thread-pane");

        g_devices.list_devices = gtk_list_box_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.list_devices), "tether-list");
        g_signal_connect(g_devices.list_devices, "row-selected", G_CALLBACK(on_device_selected), nullptr);
        gtk_container_add(GTK_CONTAINER(left_scroll), g_devices.list_devices);
        gtk_paned_pack1(GTK_PANED(paned), left_scroll, FALSE, FALSE);

        // Right Pane
        g_devices.right_pane_stack = gtk_stack_new();
        // A hidden device page must not impose its minimum width on this one.
        gtk_stack_set_hhomogeneous(GTK_STACK(g_devices.right_pane_stack), FALSE);
        gtk_stack_set_vhomogeneous(GTK_STACK(g_devices.right_pane_stack), FALSE);
        gtk_stack_set_transition_type(GTK_STACK(g_devices.right_pane_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

        // Placeholder, which on a machine with nothing paired is the first thing
        // anyone sees: it explains both routes rather than saying "select a device".
        GtkWidget* placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
        gtk_container_set_border_width(GTK_CONTAINER(placeholder), 24);
        gtk_widget_set_valign(placeholder, GTK_ALIGN_START);
        GtkWidget* hero = navigation_icon("devices", 48);
        style(hero, "tether-hero-icon");
        gtk_widget_set_halign(hero, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(placeholder), hero, FALSE, FALSE, 0);

        GtkWidget* welcome_title = gtk_label_new(nullptr);
        style(welcome_title, "tether-hero-title");
        gtk_label_set_line_wrap(GTK_LABEL(welcome_title), TRUE);
        gtk_label_set_justify(GTK_LABEL(welcome_title), GTK_JUSTIFY_CENTER);
        set_markup(welcome_title, "<big><b>" + escape_markup(_("Connect your iPhone")) + "</b></big>");
        gtk_box_pack_start(GTK_BOX(placeholder), welcome_title, FALSE, FALSE, 0);

        // A plain box cannot reflow, and the two blocks do not both fit in a narrow
        // window. A flow box drops to a single column instead of clipping.
        GtkWidget* routes = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(routes), GTK_SELECTION_NONE);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(routes), 1);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(routes), 2);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(routes), 24);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(routes), 16);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(routes), TRUE);
        gtk_widget_set_halign(routes, GTK_ALIGN_CENTER);

        auto build_route_block =
            [](const char* icon_name, const char* title, const char* steps, GtkWidget** status_out) {
                GtkWidget* block = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
                gtk_widget_set_valign(block, GTK_ALIGN_START);
                style(block, "tether-card");

                GtkWidget* heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                gtk_box_pack_start(
                    GTK_BOX(heading), gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON), FALSE, FALSE, 0);
                GtkWidget* heading_label = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(heading_label), title);
                gtk_label_set_xalign(GTK_LABEL(heading_label), 0.0);
                gtk_box_pack_start(GTK_BOX(heading), heading_label, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(block), heading, FALSE, FALSE, 0);

                GtkWidget* steps_label = gtk_label_new(steps);
                gtk_label_set_xalign(GTK_LABEL(steps_label), 0.0);
                gtk_label_set_line_wrap(GTK_LABEL(steps_label), TRUE);
                gtk_label_set_max_width_chars(GTK_LABEL(steps_label), 28);
                gtk_box_pack_start(GTK_BOX(block), steps_label, FALSE, FALSE, 0);

                *status_out = gtk_label_new("");
                gtk_label_set_xalign(GTK_LABEL(*status_out), 0.0);
                gtk_label_set_line_wrap(GTK_LABEL(*status_out), TRUE);
                gtk_label_set_max_width_chars(GTK_LABEL(*status_out), 30);
                gtk_style_context_add_class(gtk_widget_get_style_context(*status_out), "muted");
                gtk_box_pack_start(GTK_BOX(block), *status_out, FALSE, FALSE, 0);
                return block;
            };

        gtk_container_add(GTK_CONTAINER(routes),
                          build_route_block("network-wireless-signal-excellent-symbolic",
                                            ("<b>Wi-Fi</b>\n" + escape_markup(_("clipboard, files, OTP"))).c_str(),
                                            _("1.  Open the Tether app on the iPhone\n"
                                              "2.  It finds this computer by itself\n"
                                              "3.  Approve on both ends"),
                                            &g_devices.lbl_welcome_wifi));
        gtk_label_set_selectable(GTK_LABEL(g_devices.lbl_welcome_wifi), TRUE);

        gtk_container_add(
            GTK_CONTAINER(routes),
            build_route_block("bluetooth-active-symbolic",
                              ("<b>Bluetooth</b>\n" + escape_markup(_("messages, notifications"))).c_str(),
                              _("1.  Pick the iPhone under BLUETOOTH\n"
                                "2.  Press Pair over Bluetooth\n"
                                "3.  Grant the two toggles on the phone"),
                              &g_devices.lbl_welcome_bt));

        gtk_box_pack_start(GTK_BOX(placeholder), routes, FALSE, FALSE, 0);

        // One button for both routes: scanning is what refreshes either list.
        GtkWidget* welcome_scan = gtk_button_new_with_label(_("Scan for devices"));
        gtk_widget_set_halign(welcome_scan, GTK_ALIGN_CENTER);
        style(welcome_scan, "suggested-action");
        g_signal_connect(welcome_scan,
                         "clicked",
                         G_CALLBACK(+[](GtkWidget*, gpointer) { devices_view_trigger_discovery(); }),
                         nullptr);
        gtk_box_pack_start(GTK_BOX(placeholder), welcome_scan, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_devices.right_pane_stack), placeholder, "placeholder");

        // AirPods: battery and the listening mode, which is all the buds expose.
        GtkWidget* airpods_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_container_set_border_width(GTK_CONTAINER(airpods_box), 8);
        gtk_widget_set_valign(airpods_box, GTK_ALIGN_START);
        style(airpods_box, "tether-detail");

        g_devices.chk_airpods_enabled = gtk_check_button_new_with_label(_("Manage AirPods from Tether"));
        GtkWidget* chk_airpods_enabled_label = gtk_bin_get_child(GTK_BIN(g_devices.chk_airpods_enabled));
        gtk_label_set_line_wrap(GTK_LABEL(chk_airpods_enabled_label), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(chk_airpods_enabled_label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(chk_airpods_enabled_label), 28);
        gtk_widget_set_tooltip_text(g_devices.chk_airpods_enabled,
                                    _("The AirPods channel takes one program per computer. Turning this off "
                                      "releases it, so another AirPods program can use it."));
        g_signal_connect(g_devices.chk_airpods_enabled, "toggled", G_CALLBACK(on_airpods_enabled_toggled), nullptr);
        gtk_box_pack_start(GTK_BOX(airpods_box), g_devices.chk_airpods_enabled, FALSE, FALSE, 0);

        g_devices.lbl_airpods_name = gtk_label_new(nullptr);
        style(g_devices.lbl_airpods_name, "tether-hero-title");
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_airpods_name), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(g_devices.lbl_airpods_name), 24);
        gtk_label_set_line_wrap_mode(GTK_LABEL(g_devices.lbl_airpods_name), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_airpods_name), 0.0);
        gtk_box_pack_start(GTK_BOX(airpods_box), g_devices.lbl_airpods_name, FALSE, FALSE, 0);

        g_devices.lbl_airpods_battery = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_airpods_battery), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_airpods_battery), "muted");
        gtk_box_pack_start(GTK_BOX(airpods_box), g_devices.lbl_airpods_battery, FALSE, FALSE, 0);

        g_devices.btn_airpods_connect = gtk_button_new_with_label(_("Connect"));
        gtk_widget_set_halign(g_devices.btn_airpods_connect, GTK_ALIGN_START);
        g_signal_connect(g_devices.btn_airpods_connect, "clicked", G_CALLBACK(on_airpods_connect_click), nullptr);
        gtk_box_pack_start(GTK_BOX(airpods_box), g_devices.btn_airpods_connect, FALSE, FALSE, 0);

        GtkWidget* lbl_mode_title = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(lbl_mode_title), 0.0);
        set_markup(lbl_mode_title, "<b>" + escape_markup(_("Listening mode")) + "</b>");
        gtk_box_pack_start(GTK_BOX(airpods_box), lbl_mode_title, FALSE, FALSE, 0);

        GtkWidget* modes = gtk_grid_new();
        gtk_grid_set_column_homogeneous(GTK_GRID(modes), TRUE);
        gtk_grid_set_row_spacing(GTK_GRID(modes), 6);
        gtk_grid_set_column_spacing(GTK_GRID(modes), 6);
        style(modes, "tether-modes");
        AtkObject* modes_a11y = gtk_widget_get_accessible(modes);
        atk_object_set_role(modes_a11y, ATK_ROLE_PANEL);
        atk_object_add_relationship(modes_a11y, ATK_RELATION_LABELLED_BY, gtk_widget_get_accessible(lbl_mode_title));
        GtkWidget* group = nullptr;
        for (int i = 0; i < 4; ++i) {
            GtkWidget* button =
                gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(group), _(AIRPODS_MODES[i].label));
            gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(button), FALSE);
            if (!group)
                group = button;
            g_signal_connect(button, "toggled", G_CALLBACK(on_airpods_mode_toggled), GINT_TO_POINTER(i));
            g_devices.airpods_mode_buttons[i] = button;
            gtk_grid_attach(GTK_GRID(modes), button, i % 2, i / 2, 1, 1);
        }
        GtkWidget* listening_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        style(listening_card, "tether-card");
        gtk_box_pack_start(GTK_BOX(listening_card), modes, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(airpods_box), listening_card, FALSE, FALSE, 0);

        g_devices.lbl_airpods_reason = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_airpods_reason), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_airpods_reason), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(g_devices.lbl_airpods_reason), 30);
        gtk_label_set_line_wrap_mode(GTK_LABEL(g_devices.lbl_airpods_reason), PANGO_WRAP_WORD_CHAR);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_airpods_reason), "muted");
        gtk_box_pack_start(GTK_BOX(airpods_box), g_devices.lbl_airpods_reason, FALSE, FALSE, 0);

        GtkWidget* lbl_wear_title = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(lbl_wear_title), 0.0);
        set_markup(lbl_wear_title, "<b>" + escape_markup(_("In-ear detection")) + "</b>");
        gtk_box_pack_start(GTK_BOX(airpods_box), lbl_wear_title, FALSE, FALSE, 0);

        g_devices.lbl_airpods_worn = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_airpods_worn), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_airpods_worn), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_airpods_worn), "muted");
        gtk_box_pack_start(GTK_BOX(airpods_box), g_devices.lbl_airpods_worn, FALSE, FALSE, 0);

        GtkWidget* pause_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        GtkWidget* pause_label = gtk_label_new(_("Pause playback when:"));
        gtk_box_pack_start(GTK_BOX(pause_row), pause_label, FALSE, FALSE, 0);
        g_devices.cmb_airpods_pause = gtk_combo_box_text_new();
        gtk_label_set_mnemonic_widget(GTK_LABEL(pause_label), g_devices.cmb_airpods_pause);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_devices.cmb_airpods_pause), _("Never"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_devices.cmb_airpods_pause), _("One bud is removed"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_devices.cmb_airpods_pause), _("Both buds are removed"));
        gtk_widget_set_tooltip_text(g_devices.cmb_airpods_pause,
                                    _("Pauses whatever is playing on this computer, and starts it again when the "
                                      "buds go back in. Only playback Tether paused is resumed."));
        g_signal_connect(g_devices.cmb_airpods_pause, "changed", G_CALLBACK(on_airpods_pause_changed), nullptr);
        gtk_box_pack_start(GTK_BOX(pause_row), g_devices.cmb_airpods_pause, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(airpods_box), pause_row, FALSE, FALSE, 0);

        GtkWidget* lbl_handoff_title = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(lbl_handoff_title), 0.0);
        set_markup(lbl_handoff_title, "<b>" + escape_markup(_("Calls")) + "</b>");
        gtk_box_pack_start(GTK_BOX(airpods_box), lbl_handoff_title, FALSE, FALSE, 0);

        g_devices.chk_airpods_handoff =
            gtk_check_button_new_with_label(_("Hand the AirPods to the iPhone during a call"));
        GtkWidget* chk_airpods_handoff_label = gtk_bin_get_child(GTK_BIN(g_devices.chk_airpods_handoff));
        gtk_label_set_line_wrap(GTK_LABEL(chk_airpods_handoff_label), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(chk_airpods_handoff_label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(chk_airpods_handoff_label), 28);
        gtk_widget_set_tooltip_text(g_devices.chk_airpods_handoff,
                                    _("Pauses playback and disconnects the AirPods so the iPhone can take them, "
                                      "then reconnects them when the call ends. Needs call control, and only "
                                      "applies while the buds are connected to this computer."));
        g_signal_connect(g_devices.chk_airpods_handoff, "toggled", G_CALLBACK(on_airpods_handoff_toggled), nullptr);
        gtk_box_pack_start(GTK_BOX(airpods_box), g_devices.chk_airpods_handoff, FALSE, FALSE, 0);

        g_devices.lbl_airpods_apple_id = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_airpods_apple_id), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_airpods_apple_id), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(g_devices.lbl_airpods_apple_id), 30);
        gtk_label_set_line_wrap_mode(GTK_LABEL(g_devices.lbl_airpods_apple_id), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_selectable(GTK_LABEL(g_devices.lbl_airpods_apple_id), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_airpods_apple_id), "muted");
        gtk_box_pack_start(GTK_BOX(airpods_box), g_devices.lbl_airpods_apple_id, FALSE, FALSE, 0);

        gtk_stack_add_named(GTK_STACK(g_devices.right_pane_stack), airpods_box, "airpods");

        // Bluetooth
        GtkWidget* bt_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_container_set_border_width(GTK_CONTAINER(bt_box), 8);
        gtk_widget_set_valign(bt_box, GTK_ALIGN_START);
        style(bt_box, "tether-detail");

        g_devices.lbl_bt_name = gtk_label_new(nullptr);
        style(g_devices.lbl_bt_name, "tether-hero-title");
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_bt_name), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(g_devices.lbl_bt_name), 24);
        gtk_label_set_line_wrap_mode(GTK_LABEL(g_devices.lbl_bt_name), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_bt_name), 0.0);
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.lbl_bt_name, FALSE, FALSE, 0);

        // One-time system setup, shown only while something is actually missing.
        g_devices.bt_setup_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.bt_setup_box), "tether-setup");
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.bt_setup_box, FALSE, FALSE, 0);

        GtkWidget* lbl_setup_title = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(lbl_setup_title), 0.0);
        set_markup(lbl_setup_title, "<b>" + escape_markup(_("Bluetooth needs one-time system setup")) + "</b>");
        gtk_box_pack_start(GTK_BOX(g_devices.bt_setup_box), lbl_setup_title, FALSE, FALSE, 0);

        g_devices.lbl_bt_setup_what = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_bt_setup_what), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_bt_setup_what), TRUE);
        gtk_box_pack_start(GTK_BOX(g_devices.bt_setup_box), g_devices.lbl_bt_setup_what, FALSE, FALSE, 0);

        // Selectable so the command can be copied by hand as well as by the button.
        g_devices.lbl_bt_setup_command = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_bt_setup_command), 0.0);
        gtk_label_set_selectable(GTK_LABEL(g_devices.lbl_bt_setup_command), TRUE);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_bt_setup_command), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(g_devices.lbl_bt_setup_command), PANGO_WRAP_WORD_CHAR);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_bt_setup_command),
                                    "tether-setup-command");
        gtk_box_pack_start(GTK_BOX(g_devices.bt_setup_box), g_devices.lbl_bt_setup_command, FALSE, FALSE, 0);

        GtkWidget* setup_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* btn_setup_copy = gtk_button_new_with_label(_("Copy commands"));
        gtk_widget_set_tooltip_text(btn_setup_copy,
                                    _("Run these in a terminal. Tether does not change your Bluetooth "
                                      "adapter or bluetoothd on its own."));
        g_signal_connect(btn_setup_copy, "clicked", G_CALLBACK(on_bt_setup_copy_click), nullptr);
        gtk_box_pack_start(GTK_BOX(setup_actions), btn_setup_copy, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(g_devices.bt_setup_box), setup_actions, FALSE, FALSE, 0);

        g_devices.lbl_bt_mode = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_bt_mode), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_bt_mode), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(g_devices.lbl_bt_mode), 30);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_bt_mode), "muted");
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.lbl_bt_mode, FALSE, FALSE, 0);

        GtkWidget* capabilities = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
        style(capabilities, "tether-card");
        const char* capability_icons[] = {"devices", "messages", "contacts", "notifications"};
        size_t capability_index = 0;
        for (GtkWidget** label : {&g_devices.lbl_bt_link,
                                  &g_devices.lbl_bt_messages,
                                  &g_devices.lbl_bt_contacts,
                                  &g_devices.lbl_bt_notifications}) {
            *label = gtk_label_new(nullptr);
            gtk_label_set_xalign(GTK_LABEL(*label), 0.0);
            gtk_label_set_line_wrap(GTK_LABEL(*label), TRUE);
            GtkWidget* capability_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
            gtk_box_pack_start(
                GTK_BOX(capability_row), navigation_icon(capability_icons[capability_index++]), FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(capability_row), *label, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(capabilities), capability_row, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(bt_box), capabilities, FALSE, FALSE, 0);

        g_devices.chk_bt_enabled = gtk_check_button_new_with_label(_("Connect to this iPhone over Bluetooth"));
        GtkWidget* chk_bt_enabled_label = gtk_bin_get_child(GTK_BIN(g_devices.chk_bt_enabled));
        gtk_label_set_line_wrap(GTK_LABEL(chk_bt_enabled_label), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(chk_bt_enabled_label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(chk_bt_enabled_label), 28);
        gtk_widget_set_tooltip_text(g_devices.chk_bt_enabled,
                                    _("Keep the Bluetooth link to the iPhone up, reconnecting whenever it drops. "
                                      "Turning this off stops Tether reconnecting; a link that is already up stays "
                                      "up until you disconnect it."));
        g_signal_connect(g_devices.chk_bt_enabled, "toggled", G_CALLBACK(on_bt_enabled_toggled), nullptr);
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.chk_bt_enabled, FALSE, FALSE, 0);

        g_devices.lbl_bt_reason = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_bt_reason), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_bt_reason), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(g_devices.lbl_bt_reason), 30);
        gtk_label_set_line_wrap_mode(GTK_LABEL(g_devices.lbl_bt_reason), PANGO_WRAP_WORD_CHAR);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_bt_reason), "muted");
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.lbl_bt_reason, FALSE, FALSE, 0);

        GtkWidget* bt_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        g_devices.btn_bt_pair = gtk_button_new_with_label(_("Pair over Bluetooth"));
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.btn_bt_pair), "suggested-action");
        g_signal_connect(g_devices.btn_bt_pair, "clicked", G_CALLBACK(on_bt_pair_click), nullptr);
        gtk_box_pack_start(GTK_BOX(bt_buttons), g_devices.btn_bt_pair, FALSE, FALSE, 0);

        g_devices.btn_bt_solicit = gtk_button_new_with_label(_("Show iPhone Permissions"));
        gtk_widget_set_tooltip_text(g_devices.btn_bt_solicit,
                                    _("Re-advertise so the iPhone shows its Show Message Notifications and "
                                      "Sync Contacts toggles under Settings > Bluetooth > (i)."));
        g_signal_connect(g_devices.btn_bt_solicit, "clicked", G_CALLBACK(on_bt_solicit_click), nullptr);
        gtk_box_pack_start(GTK_BOX(bt_buttons), g_devices.btn_bt_solicit, FALSE, FALSE, 0);

        g_devices.btn_bt_unpair = gtk_button_new_with_label(_("Unpair"));
        g_signal_connect(g_devices.btn_bt_unpair, "clicked", G_CALLBACK(on_bt_unpair_click), nullptr);
        gtk_box_pack_start(GTK_BOX(bt_buttons), g_devices.btn_bt_unpair, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bt_box), bt_buttons, FALSE, FALSE, 0);

        g_devices.lbl_bt_progress = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_bt_progress), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_bt_progress), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(g_devices.lbl_bt_progress), PANGO_WRAP_WORD_CHAR);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_bt_progress), "muted");
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.lbl_bt_progress, FALSE, FALSE, 0);

        gtk_stack_add_named(GTK_STACK(g_devices.right_pane_stack), bt_box, "bluetooth");

        // Pair
        GtkWidget* pair_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_widget_set_valign(pair_box, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(pair_box, GTK_ALIGN_CENTER);
        g_devices.lbl_unpaired_name = gtk_label_new(nullptr);
        style(g_devices.lbl_unpaired_name, "tether-hero-title");
        gtk_box_pack_start(GTK_BOX(pair_box), g_devices.lbl_unpaired_name, FALSE, FALSE, 0);
        g_devices.lbl_unpaired_ip = gtk_label_new(nullptr);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_unpaired_ip), "muted");
        gtk_box_pack_start(GTK_BOX(pair_box), g_devices.lbl_unpaired_ip, FALSE, FALSE, 0);

        GtkWidget* pair_btn = gtk_button_new_with_label(_("Pair Device"));
        gtk_style_context_add_class(gtk_widget_get_style_context(pair_btn), "suggested-action");
        g_signal_connect(pair_btn, "clicked", G_CALLBACK(on_pair_click), nullptr);
        gtk_box_pack_start(GTK_BOX(pair_box), pair_btn, FALSE, FALSE, 0);

        // optional accept override button
        GtkWidget* accept_btn = gtk_button_new_with_label(_("Force Trust (Accept Pending)"));
        g_signal_connect(accept_btn, "clicked", G_CALLBACK(on_accept_click), nullptr);
        gtk_box_pack_start(GTK_BOX(pair_box), accept_btn, FALSE, FALSE, 0);

        gtk_stack_add_named(GTK_STACK(g_devices.right_pane_stack), pair_box, "pair");

        // Action
        GtkWidget* action_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
        gtk_container_set_border_width(GTK_CONTAINER(action_box), 8);
        gtk_widget_set_valign(action_box, GTK_ALIGN_START);
        style(action_box, "tether-detail");

        g_devices.lbl_action_name = gtk_label_new(nullptr);
        style(g_devices.lbl_action_name, "tether-hero-title");
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_action_name), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(g_devices.lbl_action_name), 24);
        gtk_label_set_line_wrap_mode(GTK_LABEL(g_devices.lbl_action_name), PANGO_WRAP_WORD_CHAR);
        gtk_widget_set_halign(g_devices.lbl_action_name, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(action_box), g_devices.lbl_action_name, FALSE, FALSE, 0);

        GtkWidget* btn_grid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
        gtk_widget_set_halign(btn_grid, GTK_ALIGN_CENTER);
        g_devices.btn_grid = btn_grid;

        // drop zone
        GtkWidget* dropzone = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_style_context_add_class(gtk_widget_get_style_context(dropzone), "tether-dropzone");
        g_devices.dropzone = dropzone;
        gtk_box_pack_start(GTK_BOX(btn_grid), dropzone, FALSE, FALSE, 0);

        GtkWidget* transfer_icon = gtk_image_new_from_icon_name("folder-download-symbolic", GTK_ICON_SIZE_DIALOG);
        gtk_box_pack_start(GTK_BOX(dropzone), transfer_icon, FALSE, FALSE, 0);
        GtkWidget* lbl_drop = gtk_label_new(_("Drop files here to send"));
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl_drop), "muted");
        gtk_box_pack_start(GTK_BOX(dropzone), lbl_drop, FALSE, FALSE, 0);

        GtkWidget* btn_send_file = gtk_button_new_with_label(_("Send File"));
        gtk_style_context_add_class(gtk_widget_get_style_context(btn_send_file), "suggested-action");
        g_signal_connect(btn_send_file, "clicked", G_CALLBACK(on_choose_file), nullptr);
        gtk_box_pack_start(GTK_BOX(dropzone), btn_send_file, FALSE, FALSE, 0);

        GtkWidget* btn_send_clip = gtk_button_new_with_label(_("Send Clipboard"));
        g_signal_connect(btn_send_clip,
                         "clicked",
                         G_CALLBACK(+[](GtkWidget*, gpointer) {
                             nlohmann::json j;
                             j["command"] = "clipboard_send";
                             daemon_send(j);
                             set_status_action(_("Clipboard sync requested..."));
                         }),
                         nullptr);
        GtkWidget* clipboard_card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
        style(clipboard_card, "tether-card");
        gtk_box_pack_start(GTK_BOX(clipboard_card),
                           gtk_image_new_from_icon_name("edit-paste-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR),
                           FALSE,
                           FALSE,
                           0);
        gtk_box_pack_start(GTK_BOX(clipboard_card), btn_send_clip, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(btn_grid), clipboard_card, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(action_box), btn_grid, FALSE, FALSE, 0);

        g_devices.btn_action_bt_pair = gtk_button_new_with_label(_("Pair over Bluetooth"));
        gtk_button_set_image(GTK_BUTTON(g_devices.btn_action_bt_pair),
                             gtk_image_new_from_icon_name("bluetooth-symbolic", GTK_ICON_SIZE_BUTTON));
        gtk_button_set_always_show_image(GTK_BUTTON(g_devices.btn_action_bt_pair), TRUE);
        gtk_widget_set_halign(g_devices.btn_action_bt_pair, GTK_ALIGN_CENTER);
        g_signal_connect(g_devices.btn_action_bt_pair, "clicked", G_CALLBACK(on_action_bt_pair_clicked), nullptr);
        gtk_box_pack_start(GTK_BOX(action_box), g_devices.btn_action_bt_pair, FALSE, FALSE, 0);

        g_devices.btn_forget = gtk_button_new_with_label(_("Forget"));
        gtk_widget_set_halign(g_devices.btn_forget, GTK_ALIGN_CENTER);
        g_signal_connect(g_devices.btn_forget, "clicked", G_CALLBACK(on_forget_click), nullptr);
        gtk_box_pack_start(GTK_BOX(action_box), g_devices.btn_forget, FALSE, FALSE, 0);

        GtkSizeGroup* action_button_sizes = gtk_size_group_new(GTK_SIZE_GROUP_BOTH);
        gtk_size_group_add_widget(action_button_sizes, btn_send_clip);
        gtk_size_group_add_widget(action_button_sizes, g_devices.btn_action_bt_pair);
        gtk_size_group_add_widget(action_button_sizes, g_devices.btn_forget);
        g_object_unref(action_button_sizes);

        g_devices.lbl_action_status = gtk_label_new(_("Ready"));
        gtk_widget_set_halign(g_devices.lbl_action_status, GTK_ALIGN_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_action_status), "muted");
        gtk_box_pack_start(GTK_BOX(action_box), g_devices.lbl_action_status, FALSE, FALSE, 0);

        GtkWidget* action_page = gtk_event_box_new();
        gtk_container_add(GTK_CONTAINER(action_page), action_box);

        static const GtkTargetEntry kUriTargets[] = {{const_cast<gchar*>("text/uri-list"), 0, 0}};

        gtk_drag_dest_set(action_page,
                          static_cast<GtkDestDefaults>(GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_DROP),
                          kUriTargets,
                          G_N_ELEMENTS(kUriTargets),
                          GDK_ACTION_COPY);
        g_signal_connect(action_page, "drag-motion", G_CALLBACK(on_drop_motion), nullptr);
        g_signal_connect(action_page, "drag-leave", G_CALLBACK(on_drop_leave), nullptr);
        g_signal_connect(action_page, "drag-data-received", G_CALLBACK(on_drop_received), nullptr);

        gtk_stack_add_named(GTK_STACK(g_devices.right_pane_stack), action_page, "action");

        GtkWidget* right_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(right_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(right_scroll), FALSE);
        gtk_container_add(GTK_CONTAINER(right_scroll), g_devices.right_pane_stack);
        gtk_paned_pack2(GTK_PANED(paned), right_scroll, TRUE, FALSE);
        gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "placeholder");

        return paned;
    }

} // namespace tether::ui
