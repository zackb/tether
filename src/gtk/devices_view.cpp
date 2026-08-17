#include "devices_view.hpp"
#include "daemon_client.hpp"
#include "ui_util.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <tether/crypto.hpp>
#include <tether/discovery.hpp>
#include <vector>

namespace tether::ui {

    namespace {

        struct DevicesState {
            std::vector<tether::DiscoveredDevice> discovered_devices;
            std::vector<tether::DiscoveredDevice> pending_pairing_requests;
            std::vector<std::pair<std::string, std::string>> paired_devices; // fp, name
            std::vector<std::string> connected_fps;                          // active connections

            GtkWidget* list_devices = nullptr;
            GtkWidget* right_pane_stack = nullptr;

            std::string selected_device_fp;
            std::string selected_device_name;
            std::string selected_device_ip;
            uint16_t selected_device_port = 5134;

            GtkWidget* lbl_action_name = nullptr;
            GtkWidget* lbl_action_status = nullptr;
            GtkWidget* btn_grid = nullptr;

            GtkWidget* lbl_unpaired_name = nullptr;
            GtkWidget* lbl_unpaired_ip = nullptr;
        };

        DevicesState g_devices;

        void set_status_action(const std::string& text) { set_text(g_devices.lbl_action_status, text); }

        void update_right_pane() {
            if (g_devices.selected_device_fp.empty()) {
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
                set_status_action(online ? "Connected and ready."
                                         : "Device is offline. Pair again or wait for network.");
                if (g_devices.btn_grid) {
                    gtk_widget_set_visible(g_devices.btn_grid, online);
                }
            } else {
                gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "pair");
                set_markup(g_devices.lbl_unpaired_name,
                           ("<b>" + escape_markup(g_devices.selected_device_name) + "</b>"));
                set_text(g_devices.lbl_unpaired_ip,
                         ("Found at " + g_devices.selected_device_ip + ":" +
                          std::to_string(g_devices.selected_device_port)));
            }
        }

        void on_device_selected(GtkListBox*, GtkListBoxRow* row, gpointer) {
            if (!row)
                return;
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
            set_text(g_devices.lbl_unpaired_ip, "Sending pair request...");
            nlohmann::json j;
            j["command"] = "pair_request";
            j["host"] = g_devices.selected_device_ip;
            j["port"] = g_devices.selected_device_port;
            daemon_send(j);
            set_status_main("Pair request sent. Approve on remote device!");
        }

        void on_accept_click(GtkWidget*, gpointer) {
            if (g_devices.selected_device_fp.empty())
                return;
            nlohmann::json j;
            j["command"] = "accept_device";
            j["fingerprint"] = g_devices.selected_device_fp;
            daemon_send(j);
        }

        void send_file_async(const std::filesystem::path& path) {
            if (!daemon_connected()) {
                set_status_action("Daemon unavailable.");
                return;
            }
            set_status_action("Sending " + path.filename().string() + "…");
            nlohmann::json j;
            j["command"] = "send_file";
            j["path"] = path.string();
            daemon_send(j);
        }

        void on_choose_file(GtkWidget*, gpointer) {
            GtkWidget* dialog = gtk_file_chooser_dialog_new("Send File",
                                                            GTK_WINDOW(main_window()),
                                                            GTK_FILE_CHOOSER_ACTION_OPEN,
                                                            "_Cancel",
                                                            GTK_RESPONSE_CANCEL,
                                                            "_Send",
                                                            GTK_RESPONSE_ACCEPT,
                                                            nullptr);
            if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
                char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
                if (filename) {
                    send_file_async(filename);
                    g_free(filename);
                }
            }
            gtk_widget_destroy(dialog);
        }

        void apply_state_snapshot(const nlohmann::json& j) {
            g_devices.connected_fps.clear();
            if (j.contains("connected_clients") && j["connected_clients"].is_array()) {
                for (auto& c : j["connected_clients"]) {
                    if (c.value("paired", false)) {
                        g_devices.connected_fps.push_back(c.value("fingerprint", ""));
                    } else {
                        tether::DiscoveredDevice dev;
                        dev.fingerprint = c.value("fingerprint", "");
                        dev.name = c.value("device_name", "Unknown Device");
                        dev.addresses.push_back({c.value("address", ""), 5134});
                        g_devices.discovered_devices.push_back(dev);
                    }
                }
            }
            g_devices.pending_pairing_requests.clear();
            if (j.contains("pending_pairs") && j["pending_pairs"].is_array()) {
                for (auto& p : j["pending_pairs"]) {
                    tether::DiscoveredDevice req;
                    req.fingerprint = p.value("fingerprint", "");
                    req.name = p.value("device_name", "Unknown Device");
                    g_devices.pending_pairing_requests.push_back(req);
                }
            }
            devices_view_refresh();
        }

    } // namespace

    void devices_view_trigger_discovery() {
        set_status_main("Scanning for nearby devices...");
        nlohmann::json j;
        j["command"] = "discover";
        daemon_send(j);
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
                        {fingerprint, name_value.is_string() ? name_value.get<std::string>() : "Unknown Device"});
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
                paired ? (online ? "network-cellular-connected-symbolic" : "network-cellular-offline-symbolic")
                       : "dialog-information-symbolic";
            GtkWidget* icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
            gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);

            GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget* title = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(title), ("<b>" + escape_markup(name) + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

            GtkWidget* subtitle = gtk_label_new(paired ? (online ? "Connected" : "Offline") : "Nearby (Tap to Pair)");
            gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
            gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
            gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);

            gtk_box_pack_start(GTK_BOX(box), labels, TRUE, TRUE, 0);
            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        };

        auto create_header_row = [](const std::string& title_text) {
            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_container_set_border_width(GTK_CONTAINER(box), 8);
            GtkWidget* lbl = gtk_label_new(nullptr);
            gtk_label_set_markup(
                GTK_LABEL(lbl),
                ("<b><span size='small' color='gray'>" + escape_markup(title_text) + "</span></b>").c_str());
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

        for (const auto& disc : g_devices.discovered_devices) {
            if (disc.fingerprint == my_fp)
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
            std::string ip = disc.addresses.empty() ? "" : disc.addresses[0].address;
            uint16_t port = disc.addresses.empty() ? 5134 : disc.addresses[0].port;
            GtkWidget* r = create_row(disc.name, disc.fingerprint, ip, port, false, false);
            discovered_rows.push_back(r);
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

            bool already_shown = false;
            for (const auto& disc : g_devices.discovered_devices) {
                if (disc.fingerprint == req.fingerprint) {
                    already_shown = true;
                    break;
                }
            }
            if (already_shown)
                continue; // ignore if mDNS already populated it

            std::string ip = req.addresses.empty() ? "" : req.addresses[0].address;
            uint16_t port = req.addresses.empty() ? 5134 : req.addresses[0].port;
            GtkWidget* r = create_row(req.name, req.fingerprint, ip, port, false, false);
            discovered_rows.push_back(r);
        }

        if (!connected_rows.empty()) {
            gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), create_header_row("CONNECTED"), -1);
            for (auto* r : connected_rows)
                gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), r, -1);
        }

        if (!remembered_rows.empty()) {
            gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), create_header_row("REMEMBERED"), -1);
            for (auto* r : remembered_rows)
                gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), r, -1);
        }

        if (!discovered_rows.empty()) {
            gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), create_header_row("DISCOVERED"), -1);
            for (auto* r : discovered_rows)
                gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), r, -1);
        }

        gtk_widget_show_all(g_devices.list_devices);
    }

    bool devices_view_handle_event(const nlohmann::json& event) {
        const std::string command = event.value("command", "");
        if (command == "state_snapshot") {
            apply_state_snapshot(event);
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
            return true;
        }
        if (command == "client_disconnected") {
            std::string fp = event.value("fingerprint", "");
            g_devices.connected_fps.erase(
                std::remove(g_devices.connected_fps.begin(), g_devices.connected_fps.end(), fp),
                g_devices.connected_fps.end());
            devices_view_refresh();
            update_right_pane();
            return true;
        }
        if (command == "pair_request_received" || command == "untrusted_client_connected") {
            tether::DiscoveredDevice req;
            req.fingerprint = event.value("fingerprint", "");

            std::string resolved_name = event.value("device_name", "Unknown Device");
            if (resolved_name == "Unknown Device") {
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
            std::string name = event.value("device_name", "");
            if (!fp.empty()) {
                g_devices.connected_fps.push_back(fp);

                // Ensure it's in the paired devices list so the UI transitions
                bool already_paired = false;
                for (const auto& p : g_devices.paired_devices) {
                    if (p.first == fp) {
                        already_paired = true;
                        break;
                    }
                }
                if (!already_paired)
                    g_devices.paired_devices.push_back({fp, name});

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
            set_status_main("Ready");
            return true;
        }
        if (command == "file_send_complete") {
            set_status_action(event.value("message", ""));
            return true;
        }
        if (command == "clipboard_content") {
            set_status_action("Desktop Clipboard Sync triggered.");
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

        g_devices.list_devices = gtk_list_box_new();
        g_signal_connect(g_devices.list_devices, "row-selected", G_CALLBACK(on_device_selected), nullptr);
        gtk_container_add(GTK_CONTAINER(left_scroll), g_devices.list_devices);
        gtk_paned_pack1(GTK_PANED(paned), left_scroll, FALSE, FALSE);

        // Right Pane
        g_devices.right_pane_stack = gtk_stack_new();
        gtk_stack_set_transition_type(GTK_STACK(g_devices.right_pane_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

        // Placeholder
        GtkWidget* placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER);
        GtkWidget* p_icon = gtk_image_new_from_icon_name("computer-symbolic", GTK_ICON_SIZE_DIALOG);
        gtk_box_pack_start(GTK_BOX(placeholder), p_icon, FALSE, FALSE, 0);
        GtkWidget* p_lbl = gtk_label_new("Select a device to start");
        gtk_style_context_add_class(gtk_widget_get_style_context(p_lbl), "muted");
        gtk_box_pack_start(GTK_BOX(placeholder), p_lbl, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_devices.right_pane_stack), placeholder, "placeholder");

        // Pair
        GtkWidget* pair_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_widget_set_valign(pair_box, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(pair_box, GTK_ALIGN_CENTER);
        g_devices.lbl_unpaired_name = gtk_label_new(nullptr);
        gtk_box_pack_start(GTK_BOX(pair_box), g_devices.lbl_unpaired_name, FALSE, FALSE, 0);
        g_devices.lbl_unpaired_ip = gtk_label_new(nullptr);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_unpaired_ip), "muted");
        gtk_box_pack_start(GTK_BOX(pair_box), g_devices.lbl_unpaired_ip, FALSE, FALSE, 0);

        GtkWidget* pair_btn = gtk_button_new_with_label("Pair Device");
        gtk_style_context_add_class(gtk_widget_get_style_context(pair_btn), "suggested-action");
        g_signal_connect(pair_btn, "clicked", G_CALLBACK(on_pair_click), nullptr);
        gtk_box_pack_start(GTK_BOX(pair_box), pair_btn, FALSE, FALSE, 0);

        // optional accept override button
        GtkWidget* accept_btn = gtk_button_new_with_label("Force Trust (Accept Pending)");
        g_signal_connect(accept_btn, "clicked", G_CALLBACK(on_accept_click), nullptr);
        gtk_box_pack_start(GTK_BOX(pair_box), accept_btn, FALSE, FALSE, 0);

        gtk_stack_add_named(GTK_STACK(g_devices.right_pane_stack), pair_box, "pair");

        // Action
        GtkWidget* action_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
        gtk_container_set_border_width(GTK_CONTAINER(action_box), 32);
        gtk_widget_set_valign(action_box, GTK_ALIGN_CENTER);

        g_devices.lbl_action_name = gtk_label_new(nullptr);
        gtk_widget_set_halign(g_devices.lbl_action_name, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(action_box), g_devices.lbl_action_name, FALSE, FALSE, 0);

        GtkWidget* btn_grid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_halign(btn_grid, GTK_ALIGN_CENTER);
        g_devices.btn_grid = btn_grid;

        GtkWidget* btn_send_file = gtk_button_new_with_label("Send File");
        gtk_style_context_add_class(gtk_widget_get_style_context(btn_send_file), "suggested-action");
        g_signal_connect(btn_send_file, "clicked", G_CALLBACK(on_choose_file), nullptr);
        gtk_box_pack_start(GTK_BOX(btn_grid), btn_send_file, FALSE, FALSE, 0);

        GtkWidget* btn_send_clip = gtk_button_new_with_label("Send Clipboard");
        g_signal_connect(btn_send_clip,
                         "clicked",
                         G_CALLBACK(+[](GtkWidget*, gpointer) {
                             nlohmann::json j;
                             j["command"] = "clipboard_set"; // triggers clipboard send
                             daemon_send(j);
                             set_status_action("Clipboard sync requested...");
                         }),
                         nullptr);
        gtk_box_pack_start(GTK_BOX(btn_grid), btn_send_clip, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(action_box), btn_grid, FALSE, FALSE, 0);

        g_devices.lbl_action_status = gtk_label_new("Ready");
        gtk_widget_set_halign(g_devices.lbl_action_status, GTK_ALIGN_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_action_status), "muted");
        gtk_box_pack_start(GTK_BOX(action_box), g_devices.lbl_action_status, FALSE, FALSE, 0);

        gtk_stack_add_named(GTK_STACK(g_devices.right_pane_stack), action_box, "action");

        gtk_paned_pack2(GTK_PANED(paned), g_devices.right_pane_stack, TRUE, FALSE);
        gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "placeholder");

        return paned;
    }

} // namespace tether::ui
