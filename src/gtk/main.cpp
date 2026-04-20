#include <gio/gio.h>
#include <gtk/gtk.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <tether/core.hpp>
#include <tether/crypto.hpp>
#include <tether/discovery.hpp>
#include <tether/net.hpp>
#include <unistd.h>
#include <vector>

namespace {

    struct AppState {
        std::vector<tether::DiscoveredDevice> discovered_devices;
        std::vector<tether::DiscoveredDevice> pending_pairing_requests;
        std::vector<std::pair<std::string, std::string>> paired_devices; // fp, name
        std::vector<std::string> connected_fps;                          // active connections

        int event_fd = -1;
        GIOChannel* event_channel = nullptr;
        guint event_watch_id = 0;
        guint event_retry_id = 0;

        GtkApplication* app = nullptr;
        GtkWidget* window = nullptr;

        GtkWidget* header_bar = nullptr;
        GtkWidget* list_devices = nullptr;
        GtkWidget* right_pane_stack = nullptr;

        std::string selected_device_fp;
        std::string selected_device_name;
        std::string selected_device_ip;
        uint16_t selected_device_port = 5134;

        GtkWidget* lbl_action_name = nullptr;
        GtkWidget* lbl_action_status = nullptr;

        GtkWidget* lbl_unpaired_name = nullptr;
        GtkWidget* lbl_unpaired_ip = nullptr;
    };

    AppState g_app;

    std::string format_fingerprint(const std::string& fingerprint) {
        std::string formatted;
        for (size_t i = 0; i < fingerprint.size(); ++i) {
            if (i > 0 && i % 2 == 0)
                formatted += ":";
            formatted += fingerprint[i];
        }
        return formatted;
    }

    std::string escape_markup(const std::string& text) {
        gchar* escaped = g_markup_escape_text(text.c_str(), -1);
        std::string result = escaped ? escaped : "";
        g_free(escaped);
        return result;
    }

    void set_markup(GtkWidget* label, const std::string& text) {
        if (label) {
            gtk_label_set_markup(GTK_LABEL(label), text.c_str());
        }
    }

    void set_status(GtkWidget* label, const std::string& text) {
        if (label) {
            gtk_label_set_text(GTK_LABEL(label), text.c_str());
        }
    }

    void set_status_main(const std::string& text) {
        if (g_app.header_bar) {
            gtk_header_bar_set_subtitle(GTK_HEADER_BAR(g_app.header_bar), text.c_str());
        }
    }

    void set_status_action(const std::string& text) { set_status(g_app.lbl_action_status, text); }

    void send_async_daemon_message(const nlohmann::json& j) {
        if (g_app.event_fd >= 0) {
            std::string payload = j.dump() + "\n";
            ::write(g_app.event_fd, payload.c_str(), payload.size());
        }
    }

    void trigger_discovery() {
        set_status_main("Scanning for nearby devices...");
        nlohmann::json j;
        j["command"] = "discover";
        send_async_daemon_message(j);
    }

    void clear_list_box(GtkWidget* list_box) {
        GList* children = gtk_container_get_children(GTK_CONTAINER(list_box));
        for (GList* iter = children; iter; iter = g_list_next(iter)) {
            gtk_widget_destroy(GTK_WIDGET(iter->data));
        }
        g_list_free(children);
    }

    // Refresh the device list based on Discovered (unpaired), Paired (offline), Paired (online)
    void refresh_device_list() {
        clear_list_box(g_app.list_devices);

        tether::Crypto::instance().init();
        try {
            nlohmann::json known = nlohmann::json::parse(tether::Crypto::instance().get_known_hosts_dump());
            g_app.paired_devices.clear();
            if (!known.empty()) {
                for (auto& [fingerprint, name_value] : known.items()) {
                    g_app.paired_devices.push_back(
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
            gtk_label_set_markup(GTK_LABEL(lbl), ("<b><span size='small' color='gray'>" + escape_markup(title_text) + "</span></b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        };

        std::string my_fp = tether::Crypto::instance().get_my_fingerprint();

        std::vector<GtkWidget*> connected_rows;
        std::vector<GtkWidget*> remembered_rows;
        std::vector<GtkWidget*> discovered_rows;

        for (const auto& paired : g_app.paired_devices) {
            bool online = std::find(g_app.connected_fps.begin(), g_app.connected_fps.end(), paired.first) !=
                          g_app.connected_fps.end();
            GtkWidget* r = create_row(paired.second, paired.first, "", 5134, true, online);
            if (online) {
                connected_rows.push_back(r);
            } else {
                remembered_rows.push_back(r);
            }
        }

        for (const auto& disc : g_app.discovered_devices) {
            if (disc.fingerprint == my_fp)
                continue;
            bool is_paired = false;
            for (const auto& p : g_app.paired_devices) {
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
        for (const auto& req : g_app.pending_pairing_requests) {
            bool is_paired = false;
            for (const auto& p : g_app.paired_devices) {
                if (p.first == req.fingerprint) {
                    is_paired = true;
                    break;
                }
            }
            if (is_paired)
                continue; // ignore if they successfully paired

            bool already_shown = false;
            for (const auto& disc : g_app.discovered_devices) {
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
            gtk_list_box_insert(GTK_LIST_BOX(g_app.list_devices), create_header_row("CONNECTED"), -1);
            for (auto* r : connected_rows) gtk_list_box_insert(GTK_LIST_BOX(g_app.list_devices), r, -1);
        }
        
        if (!remembered_rows.empty()) {
            gtk_list_box_insert(GTK_LIST_BOX(g_app.list_devices), create_header_row("REMEMBERED"), -1);
            for (auto* r : remembered_rows) gtk_list_box_insert(GTK_LIST_BOX(g_app.list_devices), r, -1);
        }
        
        if (!discovered_rows.empty()) {
            gtk_list_box_insert(GTK_LIST_BOX(g_app.list_devices), create_header_row("DISCOVERED"), -1);
            for (auto* r : discovered_rows) gtk_list_box_insert(GTK_LIST_BOX(g_app.list_devices), r, -1);
        }

        gtk_widget_show_all(g_app.list_devices);
    }

    void update_right_pane() {
        if (g_app.selected_device_fp.empty()) {
            gtk_stack_set_visible_child_name(GTK_STACK(g_app.right_pane_stack), "placeholder");
            return;
        }

        bool is_paired = false;
        for (const auto& p : g_app.paired_devices) {
            if (p.first == g_app.selected_device_fp)
                is_paired = true;
        }

        if (is_paired) {
            gtk_stack_set_visible_child_name(GTK_STACK(g_app.right_pane_stack), "action");
            set_markup(g_app.lbl_action_name, ("<b>" + escape_markup(g_app.selected_device_name) + "</b>"));
            bool online = std::find(g_app.connected_fps.begin(), g_app.connected_fps.end(), g_app.selected_device_fp) !=
                          g_app.connected_fps.end();
            set_status_action(online ? "Connected and ready." : "Device is offline. Pair again or wait for network.");
        } else {
            gtk_stack_set_visible_child_name(GTK_STACK(g_app.right_pane_stack), "pair");
            set_markup(g_app.lbl_unpaired_name, ("<b>" + escape_markup(g_app.selected_device_name) + "</b>"));
            set_status(g_app.lbl_unpaired_ip,
                       ("Found at " + g_app.selected_device_ip + ":" + std::to_string(g_app.selected_device_port)));
        }
    }

    void on_device_selected(GtkListBox*, GtkListBoxRow* row, gpointer) {
        if (!row)
            return;
        const char* fp = (const char*)g_object_get_data(G_OBJECT(row), "fp");
        const char* name = (const char*)g_object_get_data(G_OBJECT(row), "name");
        const char* ip = (const char*)g_object_get_data(G_OBJECT(row), "ip");
        g_app.selected_device_fp = fp ? fp : "";
        g_app.selected_device_name = name ? name : "";
        g_app.selected_device_ip = ip ? ip : "";
        g_app.selected_device_port = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "port"));
        update_right_pane();
    }

    void on_pair_click(GtkWidget*, gpointer) {
        if (g_app.selected_device_ip.empty())
            return;
        set_status(g_app.lbl_unpaired_ip, "Sending pair request...");
        nlohmann::json j;
        j["command"] = "pair_request";
        j["host"] = g_app.selected_device_ip;
        j["port"] = g_app.selected_device_port;
        send_async_daemon_message(j);
        set_status_main("Pair request sent. Approve on remote device!");
    }

    void on_accept_click(GtkWidget*, gpointer) {
        if (g_app.selected_device_fp.empty())
            return;
        nlohmann::json j;
        j["command"] = "accept_device";
        j["fingerprint"] = g_app.selected_device_fp;
        send_async_daemon_message(j);
    }

    void send_file_async(const std::filesystem::path& path) {
        if (g_app.event_fd < 0) {
            set_status_action("Daemon unavailable.");
            return;
        }
        set_status_action("Sending " + path.filename().string() + "…");
        nlohmann::json j;
        j["command"] = "send_file";
        j["path"] = path.string();
        send_async_daemon_message(j);
    }

    void on_choose_file(GtkWidget*, gpointer) {
        GtkWidget* dialog = gtk_file_chooser_dialog_new("Send File",
                                                        GTK_WINDOW(g_app.window),
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

    void on_pull_clipboard(GtkWidget*, gpointer) {
        nlohmann::json j;
        j["command"] = "clipboard_get";
        send_async_daemon_message(j);
        set_status_action("Requested desktop clipboard...");
    }

    void on_push_clipboard(GtkWidget*, gpointer) {
        // Wait, KDE Connect puts local clipboard to remote automatically, or via button.
        // We can just use the Push Clipboard button which pushes current wayland clipboard.
        // But the daemon does this transparently! We just need `clipboard_set` if the user typed text,
        // but here we just push whatever is on Wayland to the remote right now, wait...
        // Actually, if the user hits "Push", we read local Wayland, then send it via daemon? No, the Daemon already
        // pushes wayland changes! Actually, let's keep the button for explicit "Sync Now" if they disabled auto-sync.
    }

    void apply_state_snapshot(const nlohmann::json& j) {
        g_app.connected_fps.clear();
        if (j.contains("connected_clients") && j["connected_clients"].is_array()) {
            for (auto& c : j["connected_clients"]) {
                if (c.value("paired", false)) {
                    g_app.connected_fps.push_back(c.value("fingerprint", ""));
                } else {
                    tether::DiscoveredDevice dev;
                    dev.fingerprint = c.value("fingerprint", "");
                    dev.name = c.value("device_name", "Unknown Device");
                    dev.addresses.push_back({c.value("address", ""), 5134});
                    g_app.discovered_devices.push_back(dev);
                }
            }
        }
        g_app.pending_pairing_requests.clear();
        if (j.contains("pending_pairs") && j["pending_pairs"].is_array()) {
            for (auto& p : j["pending_pairs"]) {
                tether::DiscoveredDevice req;
                req.fingerprint = p.value("fingerprint", "");
                req.name = p.value("device_name", "Unknown Device");
                g_app.pending_pairing_requests.push_back(req);
            }
        }
        refresh_device_list();
    }

    void handle_daemon_event(const nlohmann::json& event) {
        const std::string command = event.value("command", "");
        if (command == "state_snapshot") {
            apply_state_snapshot(event);
            return;
        }
        if (command == "client_connected") {
            std::string fp = event.value("fingerprint", "");
            if (std::find(g_app.connected_fps.begin(), g_app.connected_fps.end(), fp) == g_app.connected_fps.end()) {
                g_app.connected_fps.push_back(fp);
            }
            refresh_device_list();
            update_right_pane();
            return;
        }
        if (command == "client_disconnected") {
            std::string fp = event.value("fingerprint", "");
            g_app.connected_fps.erase(std::remove(g_app.connected_fps.begin(), g_app.connected_fps.end(), fp),
                                      g_app.connected_fps.end());
            refresh_device_list();
            update_right_pane();
            return;
        }
        if (command == "pair_request_received" || command == "untrusted_client_connected") {
            tether::DiscoveredDevice req;
            req.fingerprint = event.value("fingerprint", "");

            std::string resolved_name = event.value("device_name", "Unknown Device");
            if (resolved_name == "Unknown Device") {
                for (const auto& d : g_app.discovered_devices) {
                    if (d.fingerprint == req.fingerprint && !d.name.empty()) {
                        resolved_name = d.name;
                        break;
                    }
                }
            }
            req.name = resolved_name;
            req.addresses.push_back({event.value("address", ""), 5134});
            bool exists = false;
            for (const auto& dev : g_app.pending_pairing_requests) {
                if (dev.fingerprint == req.fingerprint) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                g_app.pending_pairing_requests.push_back(req);
                refresh_device_list();
            }
            return;
        }
        if (command == "pair_accepted") {
            std::string fp = event.value("fingerprint", "");
            std::string name = event.value("device_name", "");
            if (!fp.empty()) {
                g_app.connected_fps.push_back(fp);

                // Ensure it's in the paired devices list so the UI transitions
                bool already_paired = false;
                for (const auto& p : g_app.paired_devices) {
                    if (p.first == fp) {
                        already_paired = true;
                        break;
                    }
                }
                if (!already_paired)
                    g_app.paired_devices.push_back({fp, name});

                g_app.pending_pairing_requests.erase(
                    std::remove_if(g_app.pending_pairing_requests.begin(),
                                   g_app.pending_pairing_requests.end(),
                                   [&](const tether::DiscoveredDevice& d) { return d.fingerprint == fp; }),
                    g_app.pending_pairing_requests.end());
                refresh_device_list();
                update_right_pane();
            }
            return;
        }
        if (command == "discovery_result") {
            g_app.discovered_devices.clear();
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
                    g_app.discovered_devices.push_back(dev);
                }
            }
            refresh_device_list();
            set_status_main("Ready");
            return;
        }
        if (command == "file_send_complete") {
            bool ok = event.value("success", false);
            set_status_action(event.value("message", ""));
            return;
        }
        if (command == "clipboard_content") {
            set_status_action("Desktop Clipboard Sync triggered.");
            return;
        }
        if (command == "pair_accepted") {
            // Remove from pending pairing requests
            std::string fp = event.value("fingerprint", "");
            g_app.pending_pairing_requests.erase(
                std::remove_if(g_app.pending_pairing_requests.begin(),
                               g_app.pending_pairing_requests.end(),
                               [&](const tether::DiscoveredDevice& d) { return d.fingerprint == fp; }),
                g_app.pending_pairing_requests.end());

            refresh_device_list();
            update_right_pane();
            return;
        }
    }

    // ----------- EVENT LOOP -----------
    void stop_event_subscription() {
        if (g_app.event_watch_id != 0) {
            g_source_remove(g_app.event_watch_id);
            g_app.event_watch_id = 0;
        }
        if (g_app.event_channel) {
            g_io_channel_shutdown(g_app.event_channel, TRUE, nullptr);
            g_io_channel_unref(g_app.event_channel);
            g_app.event_channel = nullptr;
        } else if (g_app.event_fd >= 0) {
            close(g_app.event_fd);
        }
        g_app.event_fd = -1;
    }

    gboolean start_event_subscription(gpointer);
    void schedule_event_retry() {
        if (g_app.event_retry_id == 0)
            g_app.event_retry_id = g_timeout_add_seconds(2, start_event_subscription, nullptr);
    }

    gboolean on_event_channel(GIOChannel* source, GIOCondition condition, gpointer) {
        if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
            stop_event_subscription();
            schedule_event_retry();
            set_status_main("Daemon Offline");
            return FALSE;
        }
        if (condition & G_IO_IN) {
            gchar* line = nullptr;
            gsize length = 0;
            GError* error = nullptr;
            while (true) {
                GIOStatus status = g_io_channel_read_line(source, &line, &length, nullptr, &error);
                if (status == G_IO_STATUS_AGAIN) {
                    if (line)
                        g_free(line);
                    break;
                }
                if (status == G_IO_STATUS_EOF || status == G_IO_STATUS_ERROR) {
                    if (error) {
                        std::cerr << "Stream err: " << error->message << std::endl;
                        g_error_free(error);
                    }
                    if (line)
                        g_free(line);
                    stop_event_subscription();
                    schedule_event_retry();
                    set_status_main("Daemon Offline");
                    return FALSE;
                }
                if (line && length > 0) {
                    try {
                        handle_daemon_event(nlohmann::json::parse(std::string(line, length)));
                    } catch (...) {
                    }
                }
                if (line)
                    g_free(line);
            }
        }
        return TRUE;
    }

    gboolean start_event_subscription(gpointer) {
        if (g_app.event_retry_id != 0) {
            g_source_remove(g_app.event_retry_id);
            g_app.event_retry_id = 0;
        }
        stop_event_subscription();
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            schedule_event_retry();
            return G_SOURCE_REMOVE;
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::string path = tether::get_runtime_dir() + "/tetherd.sock";
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            static bool autostart_attempted = false;
            if (!autostart_attempted) {
                autostart_attempted = true;
                pid_t pid = fork();
                if (pid == 0) {
                    std::filesystem::path self_path = std::filesystem::read_symlink("/proc/self/exe");
                    std::string daemon_path = (self_path.parent_path() / "tetherd").string();
                    if (fork() == 0) {
                        freopen("/dev/null", "w", stdout);
                        freopen("/dev/null", "w", stderr);
                        freopen("/dev/null", "r", stdin);
                        execl(daemon_path.c_str(), "tetherd", nullptr);
                        execlp("tetherd", "tetherd", nullptr);
                        exit(1);
                    }
                    exit(0);
                } else if (pid > 0) {
                    int status;
                    waitpid(pid, &status, 0);
                }
            }
            schedule_event_retry();
            set_status_main("Daemon Offline");
            return G_SOURCE_REMOVE;
        }

        g_app.event_fd = fd;
        g_app.event_channel = g_io_channel_unix_new(fd);
        g_io_channel_set_encoding(g_app.event_channel, nullptr, nullptr);
        g_io_channel_set_buffered(g_app.event_channel, TRUE);
        g_io_channel_set_flags(g_app.event_channel, G_IO_FLAG_NONBLOCK, nullptr);
        g_app.event_watch_id = g_io_add_watch(g_app.event_channel,
                                              static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
                                              on_event_channel,
                                              nullptr);
        static const char kSubscribe[] = "{\"command\":\"subscribe\"}\n";
        write(fd, kSubscribe, sizeof(kSubscribe) - 1);
        set_status_main("Daemon Online");
        return G_SOURCE_REMOVE;
    }

    void activate(GtkApplication* app, gpointer) {
        g_app.app = app;
        tether::Crypto::instance().init();

        g_app.window = gtk_application_window_new(app);
        gtk_window_set_title(GTK_WINDOW(g_app.window), "Tether");
        gtk_window_set_default_size(GTK_WINDOW(g_app.window), 600, 500);

        // HeaderBar
        g_app.header_bar = gtk_header_bar_new();
        gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(g_app.header_bar), TRUE);
        gtk_header_bar_set_title(GTK_HEADER_BAR(g_app.header_bar), "Tether");
        gtk_window_set_titlebar(GTK_WINDOW(g_app.window), g_app.header_bar);

        GtkWidget* refresh_btn = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
        g_signal_connect(
            refresh_btn, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer) { trigger_discovery(); }), nullptr);
        gtk_header_bar_pack_start(GTK_HEADER_BAR(g_app.header_bar), refresh_btn);

        // Paned Layout
        GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_container_add(GTK_CONTAINER(g_app.window), paned);

        // Left Pane (List)
        GtkWidget* left_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(left_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(left_scroll, 220, -1);

        g_app.list_devices = gtk_list_box_new();
        g_signal_connect(g_app.list_devices, "row-selected", G_CALLBACK(on_device_selected), nullptr);
        gtk_container_add(GTK_CONTAINER(left_scroll), g_app.list_devices);
        gtk_paned_pack1(GTK_PANED(paned), left_scroll, FALSE, FALSE);

        // Right Pane
        g_app.right_pane_stack = gtk_stack_new();
        gtk_stack_set_transition_type(GTK_STACK(g_app.right_pane_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

        // --- State: Placeholder ---
        GtkWidget* placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER);
        GtkWidget* p_icon = gtk_image_new_from_icon_name("computer-symbolic", GTK_ICON_SIZE_DIALOG);
        gtk_box_pack_start(GTK_BOX(placeholder), p_icon, FALSE, FALSE, 0);
        GtkWidget* p_lbl = gtk_label_new("Select a device to start");
        gtk_style_context_add_class(gtk_widget_get_style_context(p_lbl), "muted");
        gtk_box_pack_start(GTK_BOX(placeholder), p_lbl, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_app.right_pane_stack), placeholder, "placeholder");

        // --- State: Pair ---
        GtkWidget* pair_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_widget_set_valign(pair_box, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(pair_box, GTK_ALIGN_CENTER);
        g_app.lbl_unpaired_name = gtk_label_new(nullptr);
        gtk_box_pack_start(GTK_BOX(pair_box), g_app.lbl_unpaired_name, FALSE, FALSE, 0);
        g_app.lbl_unpaired_ip = gtk_label_new(nullptr);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_app.lbl_unpaired_ip), "muted");
        gtk_box_pack_start(GTK_BOX(pair_box), g_app.lbl_unpaired_ip, FALSE, FALSE, 0);

        GtkWidget* pair_btn = gtk_button_new_with_label("Pair Device");
        gtk_style_context_add_class(gtk_widget_get_style_context(pair_btn), "suggested-action");
        g_signal_connect(pair_btn, "clicked", G_CALLBACK(on_pair_click), nullptr);
        gtk_box_pack_start(GTK_BOX(pair_box), pair_btn, FALSE, FALSE, 0);

        // optional accept override button
        GtkWidget* accept_btn = gtk_button_new_with_label("Force Trust (Accept Pending)");
        g_signal_connect(accept_btn, "clicked", G_CALLBACK(on_accept_click), nullptr);
        gtk_box_pack_start(GTK_BOX(pair_box), accept_btn, FALSE, FALSE, 0);

        gtk_stack_add_named(GTK_STACK(g_app.right_pane_stack), pair_box, "pair");

        // --- State: Action ---
        GtkWidget* action_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
        gtk_container_set_border_width(GTK_CONTAINER(action_box), 32);
        gtk_widget_set_valign(action_box, GTK_ALIGN_CENTER);

        g_app.lbl_action_name = gtk_label_new(nullptr);
        gtk_widget_set_halign(g_app.lbl_action_name, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(action_box), g_app.lbl_action_name, FALSE, FALSE, 0);

        GtkWidget* btn_grid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_halign(btn_grid, GTK_ALIGN_CENTER);

        GtkWidget* btn_send_file = gtk_button_new_with_label("Send File");
        gtk_style_context_add_class(gtk_widget_get_style_context(btn_send_file), "suggested-action");
        g_signal_connect(btn_send_file, "clicked", G_CALLBACK(on_choose_file), nullptr);
        gtk_box_pack_start(GTK_BOX(btn_grid), btn_send_file, FALSE, FALSE, 0);

        GtkWidget* btn_send_clip = gtk_button_new_with_label("Send Clipboard");
        g_signal_connect(btn_send_clip, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer) {
            nlohmann::json j;
            j["command"] = "clipboard_set"; // triggers clipboard send
            send_async_daemon_message(j);
            set_status_action("Clipboard sync requested...");
        }), nullptr);
        gtk_box_pack_start(GTK_BOX(btn_grid), btn_send_clip, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(action_box), btn_grid, FALSE, FALSE, 0);

        g_app.lbl_action_status = gtk_label_new("Ready");
        gtk_widget_set_halign(g_app.lbl_action_status, GTK_ALIGN_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_app.lbl_action_status), "muted");
        gtk_box_pack_start(GTK_BOX(action_box), g_app.lbl_action_status, FALSE, FALSE, 0);

        gtk_stack_add_named(GTK_STACK(g_app.right_pane_stack), action_box, "action");

        gtk_paned_pack2(GTK_PANED(paned), g_app.right_pane_stack, TRUE, FALSE);
        gtk_stack_set_visible_child_name(GTK_STACK(g_app.right_pane_stack), "placeholder");

        start_event_subscription(nullptr);
        trigger_discovery();
        refresh_device_list();

        gtk_widget_show_all(g_app.window);
    }
} // namespace

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("com.tether.desktop", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    stop_event_subscription();
    if (g_app.event_retry_id != 0)
        g_source_remove(g_app.event_retry_id);
    g_object_unref(app);
    return status;
}
