#include <gtk/gtk.h>
#include <tether/client.hpp>
#include <tether/crypto.hpp>
#include <tether/discovery.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <nlohmann/json.hpp>

// Always-connected local unix socket client.
static tether::Client* g_local = nullptr;

// Optional remote TCP+TLS client, created when "Connect" is clicked.
static tether::Client* g_remote = nullptr;

// UI Widgets
static GtkWidget* text_view_clip;
static GtkWidget* lbl_status;
static GtkWidget* devices_list_box;
static GtkWidget* discovered_list_box;
static GtkWidget* lbl_discover_status;
static GtkWidget* entry_fingerprint;
static GtkWidget* entry_pair_host;
static GtkWidget* entry_pair_port;
static GtkWidget* lbl_file_status;
static GtkWidget* entry_connect_host;
static GtkWidget* entry_connect_port;
static GtkWidget* lbl_connect_status;
static GtkWidget* btn_connect;

// Returns whichever client is active — remote if connected, otherwise local.
static tether::Client* active_client() {
    if (g_remote && g_remote->is_connected()) return g_remote;
    return g_local;
}

// ─── Connection Bar ───

static void on_btn_connect_clicked(GtkWidget* w, gpointer) {
    const gchar* h = gtk_entry_get_text(GTK_ENTRY(entry_connect_host));
    const gchar* p = gtk_entry_get_text(GTK_ENTRY(entry_connect_port));

    if (!h || strlen(h) == 0) {
        // Disconnect back to local
        if (g_remote) { delete g_remote; g_remote = nullptr; }
        gtk_label_set_text(GTK_LABEL(lbl_connect_status), "Local daemon");
        gtk_button_set_label(GTK_BUTTON(btn_connect), "Connect");
        return;
    }

    // If already connected remotely, disconnect first
    if (g_remote) { delete g_remote; g_remote = nullptr; }

    int port = (p && strlen(p) > 0) ? atoi(p) : 5134;
    g_remote = new tether::Client();
    if (g_remote->connect(h, port)) {
        std::string msg = std::string("Connected to ") + h + ":" + std::to_string(port);
        gtk_label_set_text(GTK_LABEL(lbl_connect_status), msg.c_str());
        gtk_button_set_label(GTK_BUTTON(btn_connect), "Disconnect");
        gtk_entry_set_text(GTK_ENTRY(entry_connect_host), "");
    } else {
        gtk_label_set_text(GTK_LABEL(lbl_connect_status), "Connection failed.");
        delete g_remote; g_remote = nullptr;
    }
}

// ─── Clipboard Tab ───

static void on_btn_get_clicked(GtkWidget* w, gpointer) {
    tether::Client* c = active_client();
    if (!c) return;
    std::string err;
    std::string content = c->get_clipboard(err);
    if (!err.empty()) {
        gtk_label_set_text(GTK_LABEL(lbl_status), err.c_str());
        return;
    }
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view_clip));
    gtk_text_buffer_set_text(buf, content.c_str(), -1);
    gtk_label_set_text(GTK_LABEL(lbl_status), "Clipboard retrieved.");
}

static void on_btn_set_clicked(GtkWidget* w, gpointer) {
    tether::Client* c = active_client();
    if (!c) return;
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view_clip));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    std::string text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    std::string err;
    if (c->set_clipboard(text, err)) {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Clipboard updated.");
    } else {
        gtk_label_set_text(GTK_LABEL(lbl_status), err.c_str());
    }
}

static GtkWidget* build_clipboard_tab() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    GtkWidget* header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(header), "<b>Clipboard</b>");
    gtk_widget_set_halign(header, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);

    GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scroll, TRUE);
    text_view_clip = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view_clip), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scroll), text_view_clip);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* btn_get = gtk_button_new_with_label("Read From Host");
    GtkWidget* btn_set = gtk_button_new_with_label("Push To Host");
    g_signal_connect(btn_get, "clicked", G_CALLBACK(on_btn_get_clicked), NULL);
    g_signal_connect(btn_set, "clicked", G_CALLBACK(on_btn_set_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(btn_box), btn_get, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(btn_box), btn_set, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), btn_box, FALSE, FALSE, 0);

    lbl_status = gtk_label_new("Ready.");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_status), "status-label");
    gtk_box_pack_start(GTK_BOX(box), lbl_status, FALSE, FALSE, 0);

    return box;
}

// ─── Devices Tab ───

static void refresh_device_list() {
    GList* children = gtk_container_get_children(GTK_CONTAINER(devices_list_box));
    for (GList* iter = children; iter; iter = g_list_next(iter))
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    g_list_free(children);

    tether::Crypto::instance().init();
    std::string dump = tether::Crypto::instance().get_known_hosts_dump();

    try {
        nlohmann::json hosts = nlohmann::json::parse(dump);
        if (hosts.empty()) {
            GtkWidget* row = gtk_label_new("No paired devices.");
            gtk_widget_set_halign(row, GTK_ALIGN_START);
            gtk_container_add(GTK_CONTAINER(devices_list_box), row);
        } else {
            for (auto& [fp, name_val] : hosts.items()) {
                std::string name = name_val.is_string() ? name_val.get<std::string>() : "Unknown";
                std::string markup = "<b>" + name + "</b>\n<small><tt>" + fp + "</tt></small>";

                GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                gtk_container_set_border_width(GTK_CONTAINER(row_box), 6);

                GtkWidget* icon = gtk_image_new_from_icon_name("network-wired-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
                gtk_box_pack_start(GTK_BOX(row_box), icon, FALSE, FALSE, 0);

                GtkWidget* label = gtk_label_new(NULL);
                gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
                gtk_label_set_xalign(GTK_LABEL(label), 0.0);
                gtk_label_set_selectable(GTK_LABEL(label), TRUE);
                gtk_box_pack_start(GTK_BOX(row_box), label, TRUE, TRUE, 0);

                gtk_list_box_insert(GTK_LIST_BOX(devices_list_box), row_box, -1);
            }
        }
    } catch (...) {
        GtkWidget* row = gtk_label_new("No paired devices.");
        gtk_widget_set_halign(row, GTK_ALIGN_START);
        gtk_container_add(GTK_CONTAINER(devices_list_box), row);
    }
    gtk_widget_show_all(devices_list_box);
}

// ─── mDNS Discovery ───

struct DiscoverResult {
    std::vector<tether::DiscoveredHost> hosts;
};

static gboolean on_discover_complete(gpointer data) {
    auto* result = static_cast<DiscoverResult*>(data);

    // Clear previous discovered list
    GList* children = gtk_container_get_children(GTK_CONTAINER(discovered_list_box));
    for (GList* iter = children; iter; iter = g_list_next(iter))
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    g_list_free(children);

    auto devices = tether::group_discovered_hosts(result->hosts);

    if (devices.empty()) {
        GtkWidget* row = gtk_label_new("No tetherd instances found on the network.");
        gtk_widget_set_halign(row, GTK_ALIGN_START);
        gtk_container_add(GTK_CONTAINER(discovered_list_box), row);
    } else {
        tether::Crypto::instance().init();
        for (const auto& dev : devices) {
            bool known = !dev.fingerprint.empty() &&
                         tether::Crypto::instance().is_host_known(dev.fingerprint);
            std::string status_str = known ? "paired" : "new";
            std::string status_color = known ? "#8ec07c" : "#fabd2f";

            // Build address summary string
            std::string addr_summary;
            for (const auto& a : dev.addresses) {
                if (!addr_summary.empty()) addr_summary += "  ·  ";
                addr_summary += a.address + ":" + std::to_string(a.port);
            }

            std::string markup =
                "<b>" + dev.name + "</b>  <span foreground='" + status_color +
                "'>[" + status_str + "]</span>\n<small>" + addr_summary + "</small>";

            GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_container_set_border_width(GTK_CONTAINER(row_box), 6);

            const char* icon_name = known ? "network-wired-symbolic" : "network-wireless-symbolic";
            GtkWidget* icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
            gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
            gtk_box_pack_start(GTK_BOX(row_box), icon, FALSE, FALSE, 0);

            GtkWidget* label = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_box_pack_start(GTK_BOX(row_box), label, TRUE, TRUE, 0);

            // "Connect" button uses the first address (IPv4 preferred, already sorted)
            if (!dev.addresses.empty()) {
                std::string* host_copy = new std::string(dev.addresses[0].address);
                GtkWidget* btn_use = gtk_button_new_with_label("Connect");
                gtk_widget_set_valign(btn_use, GTK_ALIGN_CENTER);
                g_signal_connect(btn_use, "clicked",
                    G_CALLBACK(+[](GtkWidget*, gpointer data) {
                        auto* host_str = static_cast<std::string*>(data);
                        gtk_entry_set_text(GTK_ENTRY(entry_connect_host), host_str->c_str());
                        on_btn_connect_clicked(btn_connect, nullptr);
                    }),
                    host_copy);
                g_signal_connect(btn_use, "destroy",
                    G_CALLBACK(+[](GtkWidget*, gpointer data) {
                        delete static_cast<std::string*>(data);
                    }),
                    host_copy);
                gtk_box_pack_end(GTK_BOX(row_box), btn_use, FALSE, FALSE, 0);
            }

            gtk_list_box_insert(GTK_LIST_BOX(discovered_list_box), row_box, -1);
        }
    }

    std::string status = "Found " + std::to_string(devices.size()) + " device(s)";
    gtk_label_set_text(GTK_LABEL(lbl_discover_status), status.c_str());
    gtk_widget_show_all(discovered_list_box);

    delete result;
    return G_SOURCE_REMOVE;
}

static void on_btn_scan_clicked(GtkWidget*, gpointer) {
    gtk_label_set_text(GTK_LABEL(lbl_discover_status), "Scanning...");

    // Clear the list immediately to show scanning state
    GList* children = gtk_container_get_children(GTK_CONTAINER(discovered_list_box));
    for (GList* iter = children; iter; iter = g_list_next(iter))
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    g_list_free(children);

    GtkWidget* spinner_row = gtk_label_new("Scanning the local network...");
    gtk_widget_set_halign(spinner_row, GTK_ALIGN_START);
    gtk_container_add(GTK_CONTAINER(discovered_list_box), spinner_row);
    gtk_widget_show_all(discovered_list_box);

    // Run discovery in a background thread to avoid blocking the GTK main loop
    std::thread([]() {
        tether::Discovery discovery;
        auto hosts = discovery.discover(3000);

        auto* result = new DiscoverResult{std::move(hosts)};
        g_idle_add(on_discover_complete, result);
    }).detach();
}

static void on_btn_accept_clicked(GtkWidget*, gpointer) {
    const gchar* fp = gtk_entry_get_text(GTK_ENTRY(entry_fingerprint));
    if (!fp || strlen(fp) == 0) return;

    tether::Client tmp;
    tmp.accept_device(fp);
    gtk_entry_set_text(GTK_ENTRY(entry_fingerprint), "");
    refresh_device_list();
}

static void on_btn_pair_clicked(GtkWidget* widget, gpointer) {
    const gchar* h = gtk_entry_get_text(GTK_ENTRY(entry_pair_host));
    const gchar* p = gtk_entry_get_text(GTK_ENTRY(entry_pair_port));
    if (!h || strlen(h) == 0) return;
    int port = (p && strlen(p) > 0) ? atoi(p) : 5134;

    tether::Client pair_client;
    if (!pair_client.connect(h, port)) {
        GtkWidget* dlg = gtk_message_dialog_new(
            GTK_WINDOW(gtk_widget_get_toplevel(widget)),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Failed to connect to %s:%d", h, port);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        return;
    }

    std::string err;
    char hostname[256] = {};
    gethostname(hostname, sizeof(hostname) - 1);
    std::string resp = pair_client.pair(hostname, err);

    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(gtk_widget_get_toplevel(widget)),
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "%s\n\n%s\n\nCheck the remote daemon's output for the fingerprint,\n"
        "then paste it below and click Accept.",
        err.empty() ? "Pair request sent." : err.c_str(),
        resp.c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static GtkWidget* build_devices_tab() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    // ── Discovered on Network ──
    GtkWidget* discover_header_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* discover_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(discover_lbl), "<b>Discovered on Network</b>");
    gtk_widget_set_halign(discover_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(discover_header_row), discover_lbl, TRUE, TRUE, 0);

    lbl_discover_status = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_discover_status), "dim-label");
    gtk_box_pack_start(GTK_BOX(discover_header_row), lbl_discover_status, FALSE, FALSE, 0);

    GtkWidget* btn_scan = gtk_button_new_with_label("Scan");
    g_signal_connect(btn_scan, "clicked", G_CALLBACK(on_btn_scan_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(discover_header_row), btn_scan, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), discover_header_row, FALSE, FALSE, 0);

    GtkWidget* discover_frame = gtk_frame_new(NULL);
    GtkWidget* discover_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(discover_scroll), 100);
    discovered_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(discovered_list_box), GTK_SELECTION_NONE);
    GtkWidget* placeholder = gtk_label_new("Click Scan to discover devices.");
    gtk_container_add(GTK_CONTAINER(discovered_list_box), placeholder);
    gtk_container_add(GTK_CONTAINER(discover_scroll), discovered_list_box);
    gtk_container_add(GTK_CONTAINER(discover_frame), discover_scroll);
    gtk_box_pack_start(GTK_BOX(box), discover_frame, FALSE, FALSE, 0);

    // ── Trusted Devices ──
    GtkWidget* sep0 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(box), sep0, FALSE, FALSE, 2);

    GtkWidget* header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(header), "<b>Trusted Devices</b>");
    gtk_widget_set_halign(header, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);

    // Device list
    GtkWidget* frame = gtk_frame_new(NULL);
    GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scroll, TRUE);
    devices_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(devices_list_box), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scroll), devices_list_box);
    gtk_container_add(GTK_CONTAINER(frame), scroll);
    gtk_box_pack_start(GTK_BOX(box), frame, TRUE, TRUE, 0);

    // Accept
    GtkWidget* sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(box), sep1, FALSE, FALSE, 2);

    GtkWidget* accept_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(accept_lbl), "<b>Accept a Device</b>");
    gtk_widget_set_halign(accept_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), accept_lbl, FALSE, FALSE, 0);

    GtkWidget* accept_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    entry_fingerprint = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_fingerprint), "Paste SHA-256 fingerprint");
    gtk_widget_set_hexpand(entry_fingerprint, TRUE);
    GtkWidget* btn_accept = gtk_button_new_with_label("Accept");
    g_signal_connect(btn_accept, "clicked", G_CALLBACK(on_btn_accept_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(accept_row), entry_fingerprint, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(accept_row), btn_accept, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), accept_row, FALSE, FALSE, 0);

    // Pair with remote
    GtkWidget* sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(box), sep2, FALSE, FALSE, 2);

    GtkWidget* pair_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(pair_lbl), "<b>Pair with Remote Host</b>");
    gtk_widget_set_halign(pair_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), pair_lbl, FALSE, FALSE, 0);

    GtkWidget* pair_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    entry_pair_host = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_pair_host), "Host IP");
    gtk_widget_set_hexpand(entry_pair_host, TRUE);
    entry_pair_port = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_pair_port), "5134");
    gtk_entry_set_width_chars(GTK_ENTRY(entry_pair_port), 6);
    GtkWidget* btn_pair = gtk_button_new_with_label("Send Pair Request");
    g_signal_connect(btn_pair, "clicked", G_CALLBACK(on_btn_pair_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(pair_row), entry_pair_host, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(pair_row), entry_pair_port, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pair_row), btn_pair, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), pair_row, FALSE, FALSE, 0);

    return box;
}

// ─── File Transfer Tab ───

static void on_file_chosen(GtkWidget* widget, gpointer) {
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Select File to Transfer",
        GTK_WINDOW(gtk_widget_get_toplevel(widget)),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Send", GTK_RESPONSE_ACCEPT,
        NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        std::string path(filename);
        g_free(filename);
        gtk_widget_destroy(dialog);

        tether::Client* c = active_client();
        if (!c) {
            gtk_label_set_text(GTK_LABEL(lbl_file_status), "Not connected.");
            return;
        }

        gtk_label_set_text(GTK_LABEL(lbl_file_status), ("Sending: " + path + "...").c_str());
        while (gtk_events_pending()) gtk_main_iteration();

        std::string err;
        if (c->send_file(path, err)) {
            gtk_label_set_text(GTK_LABEL(lbl_file_status), ("Sent: " + path).c_str());
        } else {
            gtk_label_set_text(GTK_LABEL(lbl_file_status), ("Error: " + err).c_str());
        }
    } else {
        gtk_widget_destroy(dialog);
    }
}

static GtkWidget* build_file_transfer_tab() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    GtkWidget* header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(header), "<b>File Transfer</b>");
    gtk_widget_set_halign(header, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);

    GtkWidget* desc = gtk_label_new(
        "Pick a file to send through tether.\n"
        "The local daemon forwards files to connected iPhone clients.");
    gtk_label_set_xalign(GTK_LABEL(desc), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(desc), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(desc), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), desc, FALSE, FALSE, 0);

    GtkWidget* center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_vexpand(center, TRUE);
    gtk_widget_set_valign(center, GTK_ALIGN_CENTER);

    GtkWidget* icon = gtk_image_new_from_icon_name("document-send-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_box_pack_start(GTK_BOX(center), icon, FALSE, FALSE, 0);

    GtkWidget* btn_send = gtk_button_new_with_label("Choose File to Send");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_send), "suggested-action");
    g_signal_connect(btn_send, "clicked", G_CALLBACK(on_file_chosen), NULL);
    gtk_box_pack_start(GTK_BOX(center), btn_send, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), center, TRUE, TRUE, 0);

    lbl_file_status = gtk_label_new("No transfers yet.");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_file_status), "status-label");
    gtk_box_pack_start(GTK_BOX(box), lbl_file_status, FALSE, FALSE, 0);

    return box;
}

// ─── CSS ───

static void apply_css() {
    GtkCssProvider* provider = gtk_css_provider_new();
    const gchar* css =
        "textview text { "
        "   background-color: @theme_base_color; "
        "   color: @theme_fg_color; "
        "} "
        "textview { "
        "   font-family: monospace; "
        "   border-radius: 8px; "
        "   padding: 8px; "
        "} "
        "button { "
        "   border-radius: 6px; "
        "   padding: 6px 14px; "
        "} "
        ".status-label { "
        "   font-weight: bold; "
        "   color: @theme_selected_bg_color; "
        "} "
        "notebook tab { "
        "   padding: 6px 16px; "
        "} "
        "frame { "
        "   border-radius: 8px; "
        "} ";

    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

// ─── Application ───

static void activate(GtkApplication* app, gpointer) {
    if (!g_local) {
        g_local = new tether::Client();
        if (!g_local->connect()) {
            std::cerr << "Warning: could not connect to local tetherd.\n";
        }
    }

    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    apply_css();

    GtkWidget* window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Tether");
    gtk_window_set_default_size(GTK_WINDOW(window), 580, 500);

    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), outer);

    // Connection bar at top
    GtkWidget* conn_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(conn_bar), 8);

    GtkWidget* conn_lbl = gtk_label_new("Remote:");
    gtk_box_pack_start(GTK_BOX(conn_bar), conn_lbl, FALSE, FALSE, 0);

    entry_connect_host = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_connect_host), "Host IP (leave empty for local)");
    gtk_widget_set_hexpand(entry_connect_host, TRUE);
    gtk_box_pack_start(GTK_BOX(conn_bar), entry_connect_host, TRUE, TRUE, 0);

    entry_connect_port = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_connect_port), "5134");
    gtk_entry_set_width_chars(GTK_ENTRY(entry_connect_port), 6);
    gtk_box_pack_start(GTK_BOX(conn_bar), entry_connect_port, FALSE, FALSE, 0);

    btn_connect = gtk_button_new_with_label("Connect");
    g_signal_connect(btn_connect, "clicked", G_CALLBACK(on_btn_connect_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(conn_bar), btn_connect, FALSE, FALSE, 0);

    lbl_connect_status = gtk_label_new("Local daemon");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_connect_status), "status-label");
    gtk_box_pack_start(GTK_BOX(conn_bar), lbl_connect_status, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(outer), conn_bar, FALSE, FALSE, 0);

    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(outer), sep, FALSE, FALSE, 0);

    // Notebook
    GtkWidget* notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
    gtk_box_pack_start(GTK_BOX(outer), notebook, TRUE, TRUE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_clipboard_tab(), gtk_label_new("Clipboard"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_devices_tab(), gtk_label_new("Devices"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_file_transfer_tab(), gtk_label_new("Files"));

    refresh_device_list();
    gtk_widget_show_all(window);
}

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("com.tether.desktop", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    delete g_local;
    delete g_remote;
    return status;
}
