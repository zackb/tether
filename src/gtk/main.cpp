#include <gtk/gtk.h>
#include <gio/gio.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <tether/client.hpp>
#include <tether/core.hpp>
#include <tether/crypto.hpp>
#include <tether/discovery.hpp>
#include <tether/net.hpp>

namespace {

struct ClipboardHistoryEntry {
    std::string text;
    std::string source;
    std::time_t timestamp = 0;
};

struct DiscoveredDevicesPayload {
    std::vector<tether::DiscoveredDevice> devices;
};

struct FileSendResult {
    bool success = false;
    std::string filename;
    std::string message;
};

struct PairTarget {
    std::string host;
    uint16_t port = 5134;
};

struct ConnectedClientEntry {
    std::string address;
    std::string fingerprint;
    std::string device_name;
    bool paired = false;
};

struct ReceivedFileEntry {
    std::string path;
    std::string filename;
    size_t bytes_written = 0;
};

struct AppState {
    tether::Client* local = nullptr;
    std::vector<tether::DiscoveredDevice> discovered_devices;
    std::vector<ClipboardHistoryEntry> clipboard_history;
    std::vector<ConnectedClientEntry> connected_clients;
    std::vector<ReceivedFileEntry> recent_received_files;
    std::filesystem::path downloads_dir;
    int event_fd = -1;
    GIOChannel* event_channel = nullptr;
    guint event_watch_id = 0;
    guint event_retry_id = 0;

    GtkApplication* app = nullptr;
    GtkWidget* window = nullptr;
    GtkWidget* stack = nullptr;

    GtkWidget* pill_status = nullptr;
    GtkWidget* hero_title = nullptr;
    GtkWidget* hero_subtitle = nullptr;
    GtkWidget* hero_fingerprint = nullptr;
    GtkWidget* hero_discovery = nullptr;

    GtkWidget* overview_device_list = nullptr;
    GtkWidget* connected_clients_list = nullptr;
    GtkWidget* devices_discovered_list = nullptr;
    GtkWidget* devices_paired_list = nullptr;
    GtkWidget* receive_list = nullptr;
    GtkWidget* clipboard_history_list = nullptr;

    GtkWidget* entry_fingerprint = nullptr;
    GtkWidget* entry_pair_host = nullptr;
    GtkWidget* entry_pair_port = nullptr;
    GtkWidget* clipboard_text = nullptr;

    GtkWidget* lbl_overview_status = nullptr;
    GtkWidget* lbl_devices_status = nullptr;
    GtkWidget* lbl_clipboard_status = nullptr;
    GtkWidget* lbl_file_status = nullptr;
};

AppState g_app;

std::string format_fingerprint(const std::string& fingerprint) {
    std::string formatted;
    for (size_t i = 0; i < fingerprint.size(); ++i) {
        if (i > 0 && i % 2 == 0) {
            formatted += ":";
        }
        formatted += fingerprint[i];
    }
    return formatted;
}

std::string summarize_text(const std::string& text, size_t limit = 140) {
    if (text.size() <= limit) {
        return text;
    }
    return text.substr(0, limit - 1) + "…";
}

std::string escape_markup(const std::string& text) {
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string result = escaped ? escaped : "";
    g_free(escaped);
    return result;
}

std::string format_relative_time(std::time_t timestamp) {
    std::time_t now = std::time(nullptr);
    long delta = static_cast<long>(now - timestamp);

    if (delta < 60) {
        return "just now";
    }
    if (delta < 3600) {
        return std::to_string(delta / 60) + " min ago";
    }
    if (delta < 86400) {
        return std::to_string(delta / 3600) + " hr ago";
    }
    return std::to_string(delta / 86400) + " day" + (delta / 86400 == 1 ? "" : "s") + " ago";
}

std::string format_file_size(uintmax_t size) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(size);
    size_t unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }

    char buf[64];
    if (unit == 0) {
        std::snprintf(buf, sizeof(buf), "%.0f %s", value, units[unit]);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    }
    return buf;
}

std::filesystem::path get_downloads_dir() {
    if (const char* downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD)) {
        return downloads;
    }
    if (const char* home = g_get_home_dir()) {
        return std::filesystem::path(home) / "Downloads";
    }
    return std::filesystem::current_path();
}

void set_status(GtkWidget* label, const std::string& text) {
    if (label) {
        gtk_label_set_text(GTK_LABEL(label), text.c_str());
    }
}

bool ensure_local_client() {
    if (!g_app.local) {
        g_app.local = new tether::Client();
    }

    if (g_app.local->is_connected()) {
        return true;
    }

    if (!g_app.local->connect()) {
        return false;
    }

    return true;
}

void refresh_connected_clients();
void refresh_received_files();
void refresh_connection_summary();
void refresh_paired_devices();

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

void open_uri(const std::string& uri) {
    GError* error = nullptr;
    gtk_show_uri_on_window(GTK_WINDOW(g_app.window), uri.c_str(), GDK_CURRENT_TIME, &error);
    if (error) {
        std::cerr << "Failed to open URI '" << uri << "': " << error->message << std::endl;
        g_error_free(error);
    }
}

void open_path(const std::filesystem::path& path) {
    auto absolute = std::filesystem::absolute(path).string();
    gchar* uri = g_filename_to_uri(absolute.c_str(), nullptr, nullptr);
    if (!uri) {
        return;
    }
    open_uri(uri);
    g_free(uri);
}

void clear_list_box(GtkWidget* list_box) {
    GList* children = gtk_container_get_children(GTK_CONTAINER(list_box));
    for (GList* iter = children; iter; iter = g_list_next(iter)) {
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    }
    g_list_free(children);
}

GtkWidget* make_section_title(const char* title, const char* subtitle = nullptr) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget* heading = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(heading), title);
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 0);

    if (subtitle) {
        GtkWidget* detail = gtk_label_new(subtitle);
        gtk_label_set_xalign(GTK_LABEL(detail), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(detail), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(detail), "section-detail");
        gtk_box_pack_start(GTK_BOX(box), detail, FALSE, FALSE, 0);
    }

    return box;
}

GtkWidget* create_card() {
    GtkWidget* frame = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "card");
    return frame;
}

GtkWidget* wrap_in_card(GtkWidget* child) {
    GtkWidget* frame = create_card();
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

void add_clipboard_history(const std::string& text, const std::string& source) {
    if (text.empty()) {
        return;
    }

    g_app.clipboard_history.insert(g_app.clipboard_history.begin(), ClipboardHistoryEntry{text, source, std::time(nullptr)});
    if (g_app.clipboard_history.size() > 12) {
        g_app.clipboard_history.resize(12);
    }
}

void refresh_clipboard_history() {
    clear_list_box(g_app.clipboard_history_list);

    if (g_app.clipboard_history.empty()) {
        GtkWidget* empty = gtk_label_new("Clipboard activity will appear here.");
        gtk_label_set_xalign(GTK_LABEL(empty), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(empty), "muted");
        gtk_container_add(GTK_CONTAINER(g_app.clipboard_history_list), empty);
        gtk_widget_show_all(g_app.clipboard_history_list);
        return;
    }

    for (const auto& entry : g_app.clipboard_history) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_container_set_border_width(GTK_CONTAINER(row), 8);

        GtkWidget* icon = gtk_image_new_from_icon_name(
            entry.source == "Desktop" ? "computer-symbolic" : "edit-paste-symbolic",
            GTK_ICON_SIZE_LARGE_TOOLBAR);
        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

        GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        std::string title_text = "<b>" + escape_markup(summarize_text(entry.text, 110)) + "</b>";
        GtkWidget* title = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(title), title_text.c_str());
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(title), TRUE);
        gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

        std::string meta = entry.source + " · " + format_relative_time(entry.timestamp);
        GtkWidget* subtitle = gtk_label_new(meta.c_str());
        gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
        gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), labels, TRUE, TRUE, 0);

        std::string* text_copy = new std::string(entry.text);
        GtkWidget* button = gtk_button_new_with_label("Copy");
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer data) {
            auto* text = static_cast<std::string*>(data);
            GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
            gtk_clipboard_set_text(clipboard, text->c_str(), -1);
        }), text_copy);
        g_signal_connect(button, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
            delete static_cast<std::string*>(data);
        }), text_copy);
        gtk_box_pack_end(GTK_BOX(row), button, FALSE, FALSE, 0);

        gtk_list_box_insert(GTK_LIST_BOX(g_app.clipboard_history_list), row, -1);
    }

    gtk_widget_show_all(g_app.clipboard_history_list);
}

void refresh_paired_devices() {
    clear_list_box(g_app.devices_paired_list);
    tether::Crypto::instance().init();

    try {
        nlohmann::json devices = nlohmann::json::parse(tether::Crypto::instance().get_known_hosts_dump());
        if (devices.empty()) {
            GtkWidget* empty = gtk_label_new("No paired devices yet.");
            gtk_label_set_xalign(GTK_LABEL(empty), 0.0);
            gtk_style_context_add_class(gtk_widget_get_style_context(empty), "muted");
            gtk_container_add(GTK_CONTAINER(g_app.devices_paired_list), empty);
        } else {
            std::vector<std::pair<std::string, std::string>> rows;
            for (auto& [fingerprint, name_value] : devices.items()) {
                rows.emplace_back(fingerprint, name_value.is_string() ? name_value.get<std::string>() : "Unknown Device");
            }
            std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.second < rhs.second;
            });

            for (const auto& [fingerprint, name] : rows) {
                GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
                gtk_container_set_border_width(GTK_CONTAINER(row), 8);

                GtkWidget* icon = gtk_image_new_from_icon_name("emblem-ok-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
                gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

                GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
                std::string escaped_name = escape_markup(name);
                std::string escaped_fp = escape_markup(format_fingerprint(fingerprint));
                GtkWidget* title = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(title), ("<b>" + escaped_name + "</b>").c_str());
                gtk_label_set_xalign(GTK_LABEL(title), 0.0);
                gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

                GtkWidget* subtitle = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(subtitle), ("<span font_family='monospace'>" + escaped_fp + "</span>").c_str());
                gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
                gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
                gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(row), labels, TRUE, TRUE, 0);

                gtk_list_box_insert(GTK_LIST_BOX(g_app.devices_paired_list), row, -1);
            }
        }
    } catch (...) {
        GtkWidget* empty = gtk_label_new("Unable to read paired devices.");
        gtk_label_set_xalign(GTK_LABEL(empty), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(empty), "muted");
        gtk_container_add(GTK_CONTAINER(g_app.devices_paired_list), empty);
    }

    gtk_widget_show_all(g_app.devices_paired_list);
}

void refresh_discovered_devices() {
    auto render = [](GtkWidget* list_box, const std::vector<tether::DiscoveredDevice>& devices, bool compact) {
        clear_list_box(list_box);

        if (devices.empty()) {
            GtkWidget* empty = gtk_label_new(compact ? "No nearby devices found yet." : "Scan the network to discover nearby tetherd instances.");
            gtk_label_set_xalign(GTK_LABEL(empty), 0.0);
            gtk_style_context_add_class(gtk_widget_get_style_context(empty), "muted");
            gtk_container_add(GTK_CONTAINER(list_box), empty);
            gtk_widget_show_all(list_box);
            return;
        }

        for (const auto& device : devices) {
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
            gtk_container_set_border_width(GTK_CONTAINER(row), 8);

            GtkWidget* icon = gtk_image_new_from_icon_name("network-workgroup-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
            gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

            GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            std::string escaped_name = escape_markup(device.name);
            GtkWidget* title = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(title), ("<b>" + escaped_name + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

            std::string address = device.addresses.empty()
                ? "No reachable address"
                : device.addresses.front().address + ":" + std::to_string(device.addresses.front().port);
            std::string detail = address;
            if (!device.fingerprint.empty()) {
                detail += " · " + format_fingerprint(device.fingerprint.substr(0, std::min<size_t>(16, device.fingerprint.size()))) + "…";
            }
            GtkWidget* subtitle = gtk_label_new(detail.c_str());
            gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
            gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
            gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(row), labels, TRUE, TRUE, 0);

            if (!compact && !device.addresses.empty()) {
                auto* target = new PairTarget{device.addresses.front().address, device.addresses.front().port};
                GtkWidget* button = gtk_button_new_with_label("Pair");
                g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer data) {
                    auto* target = static_cast<PairTarget*>(data);
                    gtk_entry_set_text(GTK_ENTRY(g_app.entry_pair_host), target->host.c_str());
                    gtk_entry_set_text(GTK_ENTRY(g_app.entry_pair_port), std::to_string(target->port).c_str());
                    gtk_stack_set_visible_child_name(GTK_STACK(g_app.stack), "devices");
                }), target);
                g_signal_connect(button, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
                    delete static_cast<PairTarget*>(data);
                }), target);
                gtk_box_pack_end(GTK_BOX(row), button, FALSE, FALSE, 0);
            }

            gtk_list_box_insert(GTK_LIST_BOX(list_box), row, -1);
        }

        gtk_widget_show_all(list_box);
    };

    render(g_app.overview_device_list, g_app.discovered_devices, true);
    render(g_app.devices_discovered_list, g_app.discovered_devices, false);

    std::string count = std::to_string(g_app.discovered_devices.size()) + " device";
    if (g_app.discovered_devices.size() != 1) {
        count += "s";
    }
    set_status(g_app.lbl_devices_status, "Discovery ready · " + count);
    set_status(g_app.hero_discovery, count + " visible on the network");
}

void refresh_connected_clients() {
    if (!g_app.connected_clients_list) {
        return;
    }

    clear_list_box(g_app.connected_clients_list);

    if (g_app.connected_clients.empty()) {
        GtkWidget* empty = gtk_label_new("No mobile clients connected to tetherd.");
        gtk_label_set_xalign(GTK_LABEL(empty), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(empty), "muted");
        gtk_container_add(GTK_CONTAINER(g_app.connected_clients_list), empty);
        gtk_widget_show_all(g_app.connected_clients_list);
        return;
    }

    for (const auto& client : g_app.connected_clients) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_container_set_border_width(GTK_CONTAINER(row), 8);

        GtkWidget* icon = gtk_image_new_from_icon_name("smartphone-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

        GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        const std::string name = client.device_name.empty() ? client.address : client.device_name;
        GtkWidget* title = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(title), ("<b>" + escape_markup(name) + "</b>").c_str());
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

        std::string detail = client.address;
        if (!client.fingerprint.empty()) {
            detail += " · " + format_fingerprint(client.fingerprint.substr(0, std::min<size_t>(16, client.fingerprint.size()))) + "…";
        }
        GtkWidget* subtitle = gtk_label_new(detail.c_str());
        gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
        gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), labels, TRUE, TRUE, 0);

        gtk_list_box_insert(GTK_LIST_BOX(g_app.connected_clients_list), row, -1);
    }

    gtk_widget_show_all(g_app.connected_clients_list);
}

void apply_state_snapshot(const nlohmann::json& snapshot) {
    g_app.connected_clients.clear();
    if (snapshot.contains("connected_clients") && snapshot["connected_clients"].is_array()) {
        for (const auto& item : snapshot["connected_clients"]) {
            ConnectedClientEntry entry;
            entry.address = item.value("address", "");
            entry.fingerprint = item.value("fingerprint", "");
            entry.device_name = item.value("device_name", "");
            entry.paired = item.value("paired", false);
            g_app.connected_clients.push_back(std::move(entry));
        }
    }

    g_app.recent_received_files.clear();
    if (snapshot.contains("recent_received_files") && snapshot["recent_received_files"].is_array()) {
        for (const auto& item : snapshot["recent_received_files"]) {
            ReceivedFileEntry entry;
            entry.path = item.value("path", "");
            entry.filename = item.value("filename", "");
            entry.bytes_written = item.value("bytes_written", static_cast<size_t>(0));
            g_app.recent_received_files.push_back(std::move(entry));
        }
    }

    refresh_connected_clients();
    refresh_received_files();
    refresh_paired_devices();
    refresh_connection_summary();
}

gboolean start_event_subscription(gpointer);

void schedule_event_retry() {
    if (g_app.event_retry_id != 0) {
        return;
    }

    g_app.event_retry_id = g_timeout_add_seconds(2, start_event_subscription, nullptr);
}

void handle_daemon_event(const nlohmann::json& event) {
    const std::string command = event.value("command", "");
    if (command == "state_snapshot") {
        apply_state_snapshot(event);
        return;
    }

    if (command == "client_connected") {
        ConnectedClientEntry entry;
        entry.address = event.value("address", "");
        entry.fingerprint = event.value("fingerprint", "");
        entry.device_name = event.value("device_name", "");
        entry.paired = event.value("paired", false);

        auto it = std::find_if(g_app.connected_clients.begin(), g_app.connected_clients.end(), [&](const auto& existing) {
            return existing.fingerprint == entry.fingerprint && existing.address == entry.address;
        });
        if (it == g_app.connected_clients.end()) {
            g_app.connected_clients.push_back(std::move(entry));
        } else {
            *it = std::move(entry);
        }
        refresh_connected_clients();
        refresh_connection_summary();
        return;
    }

    if (command == "client_disconnected") {
        const std::string fingerprint = event.value("fingerprint", "");
        const std::string address = event.value("address", "");
        g_app.connected_clients.erase(
            std::remove_if(g_app.connected_clients.begin(), g_app.connected_clients.end(), [&](const auto& existing) {
                return existing.fingerprint == fingerprint && existing.address == address;
            }),
            g_app.connected_clients.end());
        refresh_connected_clients();
        refresh_connection_summary();
        return;
    }

    if (command == "file_received") {
        ReceivedFileEntry entry;
        entry.path = event.value("path", "");
        entry.filename = event.value("filename", "");
        entry.bytes_written = event.value("bytes_written", static_cast<size_t>(0));

        g_app.recent_received_files.erase(
            std::remove_if(g_app.recent_received_files.begin(), g_app.recent_received_files.end(), [&](const auto& existing) {
                return existing.path == entry.path;
            }),
            g_app.recent_received_files.end());
        g_app.recent_received_files.insert(g_app.recent_received_files.begin(), std::move(entry));
        if (g_app.recent_received_files.size() > 16) {
            g_app.recent_received_files.resize(16);
        }
        refresh_received_files();
        return;
    }

    if (command == "pair_request_received") {
        refresh_paired_devices();
        return;
    }

    if (command == "pair_accepted") {
        const std::string fingerprint = event.value("fingerprint", "");
        const std::string device_name = event.value("device_name", "");
        for (auto& client : g_app.connected_clients) {
            if (client.fingerprint == fingerprint) {
                client.paired = true;
                client.device_name = device_name;
            }
        }
        refresh_connected_clients();
        refresh_paired_devices();
        refresh_connection_summary();
        return;
    }

    if (command == "pair_rejected") {
        refresh_paired_devices();
        return;
    }
}

gboolean on_event_channel(GIOChannel* source, GIOCondition condition, gpointer) {
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        stop_event_subscription();
        schedule_event_retry();
        refresh_connection_summary();
        return FALSE;
    }

    while (true) {
        gchar* line = nullptr;
        gsize length = 0;
        GError* error = nullptr;
        GIOStatus status = g_io_channel_read_line(source, &line, &length, nullptr, &error);

        if (status == G_IO_STATUS_AGAIN) {
            if (line) g_free(line);
            break;
        }

        if (status == G_IO_STATUS_EOF) {
            if (line) g_free(line);
            stop_event_subscription();
            schedule_event_retry();
            refresh_connection_summary();
            return FALSE;
        }

        if (status == G_IO_STATUS_ERROR) {
            if (error) {
                std::cerr << "Daemon event stream error: " << error->message << std::endl;
                g_error_free(error);
            }
            if (line) g_free(line);
            stop_event_subscription();
            schedule_event_retry();
            refresh_connection_summary();
            return FALSE;
        }

        if (line && length > 0) {
            try {
                handle_daemon_event(nlohmann::json::parse(std::string(line, length)));
            } catch (...) {
            }
        }
        if (line) g_free(line);
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
        schedule_event_retry();
        return G_SOURCE_REMOVE;
    }

    g_app.event_fd = fd;
    g_app.event_channel = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(g_app.event_channel, nullptr, nullptr);
    g_io_channel_set_buffered(g_app.event_channel, TRUE);
    g_io_channel_set_flags(g_app.event_channel, G_IO_FLAG_NONBLOCK, nullptr);
    g_app.event_watch_id = g_io_add_watch(
        g_app.event_channel,
        static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
        on_event_channel,
        nullptr);

    static const char kSubscribe[] = "{\"command\":\"subscribe\"}\n";
    write(fd, kSubscribe, sizeof(kSubscribe) - 1);
    refresh_connection_summary();
    return G_SOURCE_REMOVE;
}

void refresh_received_files() {
    clear_list_box(g_app.receive_list);

    if (g_app.recent_received_files.empty()) {
        GtkWidget* empty = gtk_label_new("Received files reported by tetherd will appear here.");
        gtk_label_set_xalign(GTK_LABEL(empty), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(empty), "muted");
        gtk_container_add(GTK_CONTAINER(g_app.receive_list), empty);
        gtk_widget_show_all(g_app.receive_list);
        return;
    }

    for (const auto& entry : g_app.recent_received_files) {
        const auto path = std::filesystem::path(entry.path);
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_container_set_border_width(GTK_CONTAINER(row), 8);

        GtkWidget* icon = gtk_image_new_from_icon_name("folder-download-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

        GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        std::string escaped_name = escape_markup(entry.filename.empty() ? path.filename().string() : entry.filename);
        GtkWidget* title = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(title), ("<b>" + escaped_name + "</b>").c_str());
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

        GtkWidget* subtitle = gtk_label_new((format_file_size(entry.bytes_written) + " · " + path.parent_path().string()).c_str());
        gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
        gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), labels, TRUE, TRUE, 0);

        auto* file_path = new std::filesystem::path(path);
        auto* folder_path = new std::filesystem::path(path.parent_path());

        GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget* open_file = gtk_button_new_with_label("Open");
        g_signal_connect(open_file, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer data) {
            open_path(*static_cast<std::filesystem::path*>(data));
        }), file_path);
        g_signal_connect(open_file, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
            delete static_cast<std::filesystem::path*>(data);
        }), file_path);
        gtk_box_pack_start(GTK_BOX(buttons), open_file, FALSE, FALSE, 0);

        GtkWidget* open_folder = gtk_button_new_with_label("Folder");
        g_signal_connect(open_folder, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer data) {
            open_path(*static_cast<std::filesystem::path*>(data));
        }), folder_path);
        g_signal_connect(open_folder, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
            delete static_cast<std::filesystem::path*>(data);
        }), folder_path);
        gtk_box_pack_start(GTK_BOX(buttons), open_folder, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(row), buttons, FALSE, FALSE, 0);

        gtk_list_box_insert(GTK_LIST_BOX(g_app.receive_list), row, -1);
    }

    gtk_widget_show_all(g_app.receive_list);
}

void refresh_connection_summary() {
    bool daemon_ready = ensure_local_client();
    const bool subscribed = g_app.event_channel != nullptr;
    std::string mobile_summary = std::to_string(g_app.connected_clients.size()) + " mobile client";
    if (g_app.connected_clients.size() != 1) {
        mobile_summary += "s";
    }

    set_status(g_app.pill_status, daemon_ready ? "Daemon Online" : "Daemon Offline");
    set_status(g_app.hero_title, daemon_ready ? "tetherd is running" : "Waiting for tetherd");
    set_status(g_app.hero_subtitle,
        daemon_ready
            ? "Use this app as the desktop control surface for discovery, clipboard sync, and file transfers."
            : "Start the daemon or retry to restore clipboard and transfer controls.");
    set_status(g_app.hero_fingerprint, format_fingerprint(tether::Crypto::instance().get_my_fingerprint()));
    set_status(g_app.lbl_overview_status,
        daemon_ready
            ? (subscribed ? "Local daemon ready · " + mobile_summary : "Local daemon ready · event stream reconnecting")
            : "Local daemon unavailable");
}

gboolean on_discovery_complete(gpointer data) {
    std::unique_ptr<DiscoveredDevicesPayload> payload(static_cast<DiscoveredDevicesPayload*>(data));
    g_app.discovered_devices = std::move(payload->devices);
    refresh_discovered_devices();
    return G_SOURCE_REMOVE;
}

void trigger_discovery() {
    set_status(g_app.lbl_devices_status, "Scanning local network…");
    set_status(g_app.hero_discovery, "Scanning local network…");

    std::thread([]() {
        tether::Discovery discovery;
        auto hosts = discovery.discover(3000);
        auto devices = tether::group_discovered_hosts(hosts);
        auto* payload = new DiscoveredDevicesPayload{std::move(devices)};
        g_idle_add(on_discovery_complete, payload);
    }).detach();
}

gboolean on_file_send_complete(gpointer data) {
    std::unique_ptr<FileSendResult> result(static_cast<FileSendResult*>(data));
    set_status(g_app.lbl_file_status, result->message);
    set_status(g_app.lbl_overview_status, result->message);
    if (result->success) {
        refresh_received_files();
    }
    return G_SOURCE_REMOVE;
}

void send_file_async(const std::filesystem::path& path) {
    if (!ensure_local_client()) {
        set_status(g_app.lbl_file_status, "Cannot reach local tetherd.");
        return;
    }

    set_status(g_app.lbl_file_status, "Sending " + path.filename().string() + "…");
    std::thread([path]() {
        auto result = std::make_unique<FileSendResult>();
        result->filename = path.filename().string();

        tether::Client local_client;
        std::string err;
        if (!local_client.connect()) {
            result->success = false;
            result->message = "Send failed: cannot reach local tetherd";
            g_idle_add(on_file_send_complete, result.release());
            return;
        }

        result->success = local_client.send_file(path.string(), err);
        result->message = result->success
            ? "Sent " + result->filename
            : "Send failed: " + (err.empty() ? "unknown error" : err);

        g_idle_add(on_file_send_complete, result.release());
    }).detach();
}

void on_choose_file(GtkWidget*, gpointer) {
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Send File",
        GTK_WINDOW(g_app.window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Send", GTK_RESPONSE_ACCEPT,
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
    if (!ensure_local_client()) {
        set_status(g_app.lbl_clipboard_status, "Cannot reach local tetherd.");
        return;
    }

    std::string err;
    std::string content = g_app.local->get_clipboard(err);
    if (!err.empty()) {
        set_status(g_app.lbl_clipboard_status, err);
        return;
    }

    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_app.clipboard_text));
    gtk_text_buffer_set_text(buffer, content.c_str(), -1);
    add_clipboard_history(content, "Desktop");
    refresh_clipboard_history();
    set_status(g_app.lbl_clipboard_status, "Loaded desktop clipboard.");
}

void on_push_clipboard(GtkWidget*, gpointer) {
    if (!ensure_local_client()) {
        set_status(g_app.lbl_clipboard_status, "Cannot reach local tetherd.");
        return;
    }

    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_app.clipboard_text));
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    std::string err;
    bool ok = g_app.local->set_clipboard(text ? text : "", err);
    if (ok) {
        add_clipboard_history(text ? text : "", "Frontend");
        refresh_clipboard_history();
        set_status(g_app.lbl_clipboard_status, "Pushed clipboard to desktop.");
    } else {
        set_status(g_app.lbl_clipboard_status, err.empty() ? "Failed to push clipboard." : err);
    }

    g_free(text);
}

void on_scan_devices(GtkWidget*, gpointer) {
    trigger_discovery();
}

void on_accept_device(GtkWidget*, gpointer) {
    const gchar* fingerprint = gtk_entry_get_text(GTK_ENTRY(g_app.entry_fingerprint));
    if (!fingerprint || !*fingerprint) {
        set_status(g_app.lbl_devices_status, "Paste a fingerprint to trust.");
        return;
    }

    if (!ensure_local_client()) {
        set_status(g_app.lbl_devices_status, "Cannot reach local tetherd.");
        return;
    }

    g_app.local->accept_device(fingerprint);
    gtk_entry_set_text(GTK_ENTRY(g_app.entry_fingerprint), "");
    refresh_paired_devices();
    set_status(g_app.lbl_devices_status, "Trusted device fingerprint saved.");
}

void on_send_pair_request(GtkWidget* widget, gpointer) {
    const gchar* host = gtk_entry_get_text(GTK_ENTRY(g_app.entry_pair_host));
    const gchar* port_text = gtk_entry_get_text(GTK_ENTRY(g_app.entry_pair_port));
    if (!host || !*host) {
        set_status(g_app.lbl_devices_status, "Enter a host to pair with.");
        return;
    }

    int port = (port_text && *port_text) ? std::atoi(port_text) : 5134;

    tether::Client remote;
    if (!remote.connect(host, port)) {
        GtkWidget* dialog = gtk_message_dialog_new(
            GTK_WINDOW(widget ? gtk_widget_get_toplevel(widget) : g_app.window),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "Failed to connect to %s:%d", host, port);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    char hostname[256] = {};
    gethostname(hostname, sizeof(hostname) - 1);
    std::string err;
    std::string response = remote.pair(hostname, err);

    GtkWidget* dialog = gtk_message_dialog_new(
        GTK_WINDOW(widget ? gtk_widget_get_toplevel(widget) : g_app.window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK,
        "%s",
        (err.empty() ? "Pair request sent." : err).c_str());
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog),
        "%s",
        response.empty()
            ? "Approve the request on the remote device, then accept its fingerprint here if needed."
            : response.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    set_status(g_app.lbl_devices_status, err.empty() ? "Pair request sent." : err);
}

void on_open_downloads(GtkWidget*, gpointer) {
    open_path(g_app.downloads_dir);
}

void on_retry_daemon(GtkWidget*, gpointer) {
    refresh_connection_summary();
}

void on_sidebar_visible_child_changed(GObject*, GParamSpec*, gpointer) {
    refresh_connection_summary();
}

GtkWidget* build_overview_page() {
    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);

    GtkWidget* hero_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(hero_box), 20);
    gtk_style_context_add_class(gtk_widget_get_style_context(hero_box), "hero");

    g_app.pill_status = gtk_label_new("Daemon Online");
    gtk_widget_set_halign(g_app.pill_status, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_app.pill_status), "pill");
    gtk_box_pack_start(GTK_BOX(hero_box), g_app.pill_status, FALSE, FALSE, 0);

    g_app.hero_title = gtk_label_new("tetherd is running");
    gtk_widget_set_halign(g_app.hero_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_app.hero_title), "hero-title");
    gtk_box_pack_start(GTK_BOX(hero_box), g_app.hero_title, FALSE, FALSE, 0);

    g_app.hero_subtitle = gtk_label_new("Use this app as the desktop control surface for discovery, clipboard sync, and file transfers.");
    gtk_label_set_xalign(GTK_LABEL(g_app.hero_subtitle), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(g_app.hero_subtitle), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_app.hero_subtitle), "hero-subtitle");
    gtk_box_pack_start(GTK_BOX(hero_box), g_app.hero_subtitle, FALSE, FALSE, 0);

    g_app.hero_fingerprint = gtk_label_new("");
    gtk_label_set_selectable(GTK_LABEL(g_app.hero_fingerprint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_app.hero_fingerprint), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_app.hero_fingerprint), "fingerprint");
    gtk_box_pack_start(GTK_BOX(hero_box), g_app.hero_fingerprint, FALSE, FALSE, 0);

    g_app.hero_discovery = gtk_label_new("No discovery results yet");
    gtk_label_set_xalign(GTK_LABEL(g_app.hero_discovery), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_app.hero_discovery), "muted");
    gtk_box_pack_start(GTK_BOX(hero_box), g_app.hero_discovery, FALSE, FALSE, 0);

    GtkWidget* hero_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* scan_button = gtk_button_new_with_label("Scan Devices");
    g_signal_connect(scan_button, "clicked", G_CALLBACK(on_scan_devices), nullptr);
    gtk_box_pack_start(GTK_BOX(hero_actions), scan_button, FALSE, FALSE, 0);

    GtkWidget* downloads_button = gtk_button_new_with_label("Open Downloads");
    g_signal_connect(downloads_button, "clicked", G_CALLBACK(on_open_downloads), nullptr);
    gtk_box_pack_start(GTK_BOX(hero_actions), downloads_button, FALSE, FALSE, 0);

    GtkWidget* retry_button = gtk_button_new_with_label("Retry Daemon");
    g_signal_connect(retry_button, "clicked", G_CALLBACK(on_retry_daemon), nullptr);
    gtk_box_pack_start(GTK_BOX(hero_actions), retry_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hero_box), hero_actions, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(root), wrap_in_card(hero_box), FALSE, FALSE, 0);

    GtkWidget* quick_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(quick_grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(quick_grid), 12);

    auto build_quick = [](const char* title, const char* body, const char* button_text, GCallback cb) {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(box), 16);
        GtkWidget* title_label = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(title_label), title);
        gtk_label_set_xalign(GTK_LABEL(title_label), 0.0);
        gtk_box_pack_start(GTK_BOX(box), title_label, FALSE, FALSE, 0);
        GtkWidget* body_label = gtk_label_new(body);
        gtk_label_set_xalign(GTK_LABEL(body_label), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(body_label), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(body_label), "section-detail");
        gtk_box_pack_start(GTK_BOX(box), body_label, FALSE, FALSE, 0);
        GtkWidget* button = gtk_button_new_with_label(button_text);
        g_signal_connect(button, "clicked", cb, nullptr);
        gtk_box_pack_end(GTK_BOX(box), button, FALSE, FALSE, 0);
        return wrap_in_card(box);
    };

    gtk_grid_attach(GTK_GRID(quick_grid), build_quick("<b>Clipboard</b>", "Pull the desktop clipboard into the app or push text back into Wayland.", "Open Clipboard", G_CALLBACK(+[](GtkWidget*, gpointer) {
        gtk_stack_set_visible_child_name(GTK_STACK(g_app.stack), "clipboard");
    })), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(quick_grid), build_quick("<b>Files</b>", "Send files to connected mobile devices and keep recent incoming downloads close at hand.", "Send File", G_CALLBACK(on_choose_file)), 1, 0, 1, 1);

    gtk_box_pack_start(GTK_BOX(root), quick_grid, FALSE, FALSE, 0);

    GtkWidget* discovered_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(discovered_box), 16);
    gtk_box_pack_start(GTK_BOX(discovered_box), make_section_title("<b>Available Devices</b>", "Nearby tetherd instances discovered on the local network."), FALSE, FALSE, 0);
    g_app.overview_device_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_app.overview_device_list), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(discovered_box), g_app.overview_device_list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), wrap_in_card(discovered_box), FALSE, FALSE, 0);

    g_app.lbl_overview_status = gtk_label_new("Local daemon ready");
    gtk_label_set_xalign(GTK_LABEL(g_app.lbl_overview_status), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_app.lbl_overview_status), "muted");
    gtk_box_pack_start(GTK_BOX(root), g_app.lbl_overview_status, FALSE, FALSE, 0);

    return root;
}

GtkWidget* build_devices_page() {
    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);

    GtkWidget* connected = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(connected), 16);
    gtk_box_pack_start(GTK_BOX(connected), make_section_title("<b>Connected Clients</b>", "Remote mobile clients currently attached to tetherd."), FALSE, FALSE, 0);
    g_app.connected_clients_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_app.connected_clients_list), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(connected), g_app.connected_clients_list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), wrap_in_card(connected), FALSE, FALSE, 0);

    GtkWidget* discovered = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(discovered), 16);
    gtk_box_pack_start(GTK_BOX(discovered), make_section_title("<b>Discovered Devices</b>", "Scan for nearby tetherd instances and stage a pairing request."), FALSE, FALSE, 0);

    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* scan_button = gtk_button_new_with_label("Scan Network");
    g_signal_connect(scan_button, "clicked", G_CALLBACK(on_scan_devices), nullptr);
    gtk_box_pack_start(GTK_BOX(actions), scan_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(discovered), actions, FALSE, FALSE, 0);

    g_app.devices_discovered_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_app.devices_discovered_list), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(discovered), g_app.devices_discovered_list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), wrap_in_card(discovered), FALSE, FALSE, 0);

    GtkWidget* paired = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(paired), 16);
    gtk_box_pack_start(GTK_BOX(paired), make_section_title("<b>Paired Devices</b>", "Trusted fingerprints stored on this desktop."), FALSE, FALSE, 0);
    g_app.devices_paired_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_app.devices_paired_list), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(paired), g_app.devices_paired_list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), wrap_in_card(paired), FALSE, FALSE, 0);

    GtkWidget* pair_tools = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(pair_tools), 16);
    gtk_box_pack_start(GTK_BOX(pair_tools), make_section_title("<b>Pairing Tools</b>", "Accept a fingerprint manually or send a pairing request to another host."), FALSE, FALSE, 0);

    GtkWidget* accept_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_app.entry_fingerprint = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_app.entry_fingerprint), "Paste fingerprint to trust");
    gtk_widget_set_hexpand(g_app.entry_fingerprint, TRUE);
    gtk_box_pack_start(GTK_BOX(accept_row), g_app.entry_fingerprint, TRUE, TRUE, 0);
    GtkWidget* accept_button = gtk_button_new_with_label("Accept");
    g_signal_connect(accept_button, "clicked", G_CALLBACK(on_accept_device), nullptr);
    gtk_box_pack_start(GTK_BOX(accept_row), accept_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pair_tools), accept_row, FALSE, FALSE, 0);

    GtkWidget* request_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_app.entry_pair_host = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_app.entry_pair_host), "Host IP");
    gtk_widget_set_hexpand(g_app.entry_pair_host, TRUE);
    gtk_box_pack_start(GTK_BOX(request_row), g_app.entry_pair_host, TRUE, TRUE, 0);
    g_app.entry_pair_port = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_app.entry_pair_port), "5134");
    gtk_entry_set_width_chars(GTK_ENTRY(g_app.entry_pair_port), 6);
    gtk_box_pack_start(GTK_BOX(request_row), g_app.entry_pair_port, FALSE, FALSE, 0);
    GtkWidget* pair_button = gtk_button_new_with_label("Send Pair Request");
    g_signal_connect(pair_button, "clicked", G_CALLBACK(on_send_pair_request), nullptr);
    gtk_box_pack_start(GTK_BOX(request_row), pair_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pair_tools), request_row, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(root), wrap_in_card(pair_tools), FALSE, FALSE, 0);

    g_app.lbl_devices_status = gtk_label_new("Discovery ready");
    gtk_label_set_xalign(GTK_LABEL(g_app.lbl_devices_status), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_app.lbl_devices_status), "muted");
    gtk_box_pack_start(GTK_BOX(root), g_app.lbl_devices_status, FALSE, FALSE, 0);

    return root;
}

GtkWidget* build_clipboard_page() {
    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);

    GtkWidget* editor = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(editor), 16);
    gtk_box_pack_start(GTK_BOX(editor), make_section_title("<b>Clipboard</b>", "Read the current desktop clipboard or push text back through tetherd."), FALSE, FALSE, 0);

    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(scroller, -1, 220);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    g_app.clipboard_text = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_app.clipboard_text), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scroller), g_app.clipboard_text);
    gtk_box_pack_start(GTK_BOX(editor), scroller, TRUE, TRUE, 0);

    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* pull_button = gtk_button_new_with_label("Load Desktop Clipboard");
    g_signal_connect(pull_button, "clicked", G_CALLBACK(on_pull_clipboard), nullptr);
    gtk_box_pack_start(GTK_BOX(actions), pull_button, FALSE, FALSE, 0);
    GtkWidget* push_button = gtk_button_new_with_label("Push Text to Desktop");
    g_signal_connect(push_button, "clicked", G_CALLBACK(on_push_clipboard), nullptr);
    gtk_box_pack_start(GTK_BOX(actions), push_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(editor), actions, FALSE, FALSE, 0);

    g_app.lbl_clipboard_status = gtk_label_new("Clipboard ready");
    gtk_label_set_xalign(GTK_LABEL(g_app.lbl_clipboard_status), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_app.lbl_clipboard_status), "muted");
    gtk_box_pack_start(GTK_BOX(editor), g_app.lbl_clipboard_status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), wrap_in_card(editor), FALSE, FALSE, 0);

    GtkWidget* history = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(history), 16);
    gtk_box_pack_start(GTK_BOX(history), make_section_title("<b>Recent Clipboard Activity</b>", "Copy any recent item back into your local clipboard."), FALSE, FALSE, 0);
    g_app.clipboard_history_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_app.clipboard_history_list), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(history), g_app.clipboard_history_list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), wrap_in_card(history), FALSE, FALSE, 0);

    return root;
}

GtkWidget* build_files_page() {
    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);

    GtkWidget* sender = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(sender), 16);
    gtk_box_pack_start(GTK_BOX(sender), make_section_title("<b>Send Files</b>", "Choose a file to forward through the daemon to an attached mobile client."), FALSE, FALSE, 0);

    GtkWidget* sender_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* send_button = gtk_button_new_with_label("Choose File");
    gtk_style_context_add_class(gtk_widget_get_style_context(send_button), "suggested-action");
    g_signal_connect(send_button, "clicked", G_CALLBACK(on_choose_file), nullptr);
    gtk_box_pack_start(GTK_BOX(sender_actions), send_button, FALSE, FALSE, 0);
    GtkWidget* downloads_button = gtk_button_new_with_label("Open Downloads");
    g_signal_connect(downloads_button, "clicked", G_CALLBACK(on_open_downloads), nullptr);
    gtk_box_pack_start(GTK_BOX(sender_actions), downloads_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sender), sender_actions, FALSE, FALSE, 0);

    g_app.lbl_file_status = gtk_label_new("No file transfers yet.");
    gtk_label_set_xalign(GTK_LABEL(g_app.lbl_file_status), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_app.lbl_file_status), "muted");
    gtk_box_pack_start(GTK_BOX(sender), g_app.lbl_file_status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), wrap_in_card(sender), FALSE, FALSE, 0);

    GtkWidget* received = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(received), 16);
    gtk_box_pack_start(GTK_BOX(received), make_section_title("<b>Received Files</b>", "Recent files in your Downloads folder, including items received by tetherd."), FALSE, FALSE, 0);
    g_app.receive_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_app.receive_list), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(received), g_app.receive_list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), wrap_in_card(received), FALSE, FALSE, 0);

    return root;
}

void apply_css() {
    GtkCssProvider* provider = gtk_css_provider_new();
    const gchar* css =
        "window {"
        "  background: linear-gradient(180deg, #102431, #0b141a 220px, #10181d 220px, #10181d);"
        "  color: #eaf0f4;"
        "}"
        ".card {"
        "  background: rgba(19, 33, 41, 0.92);"
        "  border: 1px solid rgba(143, 189, 210, 0.18);"
        "  border-radius: 18px;"
        "}"
        ".hero {"
        "  background: linear-gradient(135deg, rgba(30, 110, 122, 0.92), rgba(18, 53, 66, 0.98));"
        "  border-radius: 18px;"
        "}"
        ".hero-title {"
        "  font-size: 26px;"
        "  font-weight: 700;"
        "}"
        ".hero-subtitle, .section-detail, .muted {"
        "  color: rgba(234, 240, 244, 0.72);"
        "}"
        ".pill {"
        "  background: rgba(235, 244, 247, 0.16);"
        "  border-radius: 999px;"
        "  padding: 5px 12px;"
        "  font-weight: 700;"
        "}"
        ".fingerprint {"
        "  font-family: Monospace;"
        "  color: #f6fbff;"
        "}"
        "label {"
        "  color: #eaf0f4;"
        "}"
        "entry, textview text, textview {"
        "  background: rgba(244, 248, 250, 0.06);"
        "  color: #f8fcff;"
        "  border-radius: 12px;"
        "}"
        "button {"
        "  border-radius: 12px;"
        "  padding: 8px 14px;"
        "  background: #214659;"
        "  color: #f8fcff;"
        "}"
        "button.suggested-action {"
        "  background: #3e9d7c;"
        "}"
        "stacksidebar.sidebar {"
        "  background: rgba(7, 14, 19, 0.32);"
        "}"
        "stacksidebar.sidebar row {"
        "  border-radius: 12px;"
        "  margin: 4px 0;"
        "  padding: 8px;"
        "}"
        "stacksidebar.sidebar row:selected {"
        "  background: rgba(73, 165, 173, 0.28);"
        "}"
        "list row {"
        "  background: transparent;"
        "}"
        "scrolledwindow, viewport {"
        "  background: transparent;"
        "}";

    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

void activate(GtkApplication* app, gpointer) {
    g_app.app = app;
    tether::Crypto::instance().init();
    ensure_local_client();

    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);
    apply_css();

    g_app.window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g_app.window), "Tether");
    gtk_window_set_default_size(GTK_WINDOW(g_app.window), 1120, 760);

    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(g_app.window), outer);

    GtkWidget* sidebar_wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(sidebar_wrap, 220, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(sidebar_wrap), "sidebar");
    gtk_box_pack_start(GTK_BOX(outer), sidebar_wrap, FALSE, FALSE, 0);

    GtkWidget* sidebar_header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(sidebar_header), 18);
    GtkWidget* app_title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(app_title), "<span size='x-large' weight='700'>Tether</span>");
    gtk_label_set_xalign(GTK_LABEL(app_title), 0.0);
    gtk_box_pack_start(GTK_BOX(sidebar_header), app_title, FALSE, FALSE, 0);
    GtkWidget* app_subtitle = gtk_label_new("Daemon frontend");
    gtk_label_set_xalign(GTK_LABEL(app_subtitle), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(app_subtitle), "muted");
    gtk_box_pack_start(GTK_BOX(sidebar_header), app_subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar_wrap), sidebar_header, FALSE, FALSE, 0);

    g_app.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(g_app.stack), GTK_STACK_TRANSITION_TYPE_SLIDE_UP_DOWN);
    gtk_stack_set_transition_duration(GTK_STACK(g_app.stack), 220);

    GtkWidget* sidebar = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(g_app.stack));
    gtk_widget_set_vexpand(sidebar, TRUE);
    gtk_box_pack_start(GTK_BOX(sidebar_wrap), sidebar, TRUE, TRUE, 0);

    GtkWidget* content_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(content_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(outer), content_scroll, TRUE, TRUE, 0);

    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(content), 24);
    gtk_container_add(GTK_CONTAINER(content_scroll), content);
    gtk_box_pack_start(GTK_BOX(content), g_app.stack, TRUE, TRUE, 0);

    gtk_stack_add_titled(GTK_STACK(g_app.stack), build_overview_page(), "overview", "Overview");
    gtk_stack_add_titled(GTK_STACK(g_app.stack), build_devices_page(), "devices", "Devices");
    gtk_stack_add_titled(GTK_STACK(g_app.stack), build_clipboard_page(), "clipboard", "Clipboard");
    gtk_stack_add_titled(GTK_STACK(g_app.stack), build_files_page(), "files", "Files");

    g_signal_connect(g_app.stack, "notify::visible-child-name", G_CALLBACK(on_sidebar_visible_child_changed), nullptr);

    g_app.downloads_dir = get_downloads_dir();
    refresh_connection_summary();
    refresh_paired_devices();
    refresh_discovered_devices();
    refresh_connected_clients();
    refresh_received_files();
    refresh_clipboard_history();
    trigger_discovery();
    start_event_subscription(nullptr);

    gtk_widget_show_all(g_app.window);
}

} // namespace

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("com.tether.desktop", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);

    stop_event_subscription();
    if (g_app.event_retry_id != 0) {
        g_source_remove(g_app.event_retry_id);
    }
    delete g_app.local;
    g_object_unref(app);
    return status;
}
