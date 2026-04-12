#include <gtk/gtk.h>
#include <tether/client.hpp>
#include <iostream>
#include <string>

// Global persistent instance to maintain Wayland socket
static tether::Client* g_client = nullptr;

// UI Globals
GtkWidget* text_view_clip;
GtkWidget* lbl_status;
GtkWidget* list_box_devices;

void on_btn_get_clicked(GtkWidget* widget, gpointer data) {
    if (!g_client) return;
    std::string err;
    std::string content = g_client->get_clipboard(err);
    if (!err.empty()) {
        gtk_label_set_text(GTK_LABEL(lbl_status), ("Error: " + err).c_str());
        return;
    }
    
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view_clip));
    gtk_text_buffer_set_text(buffer, content.c_str(), -1);
    gtk_label_set_text(GTK_LABEL(lbl_status), "Clipboard successfully retrieved.");
}

void on_btn_set_clicked(GtkWidget* widget, gpointer data) {
    if (!g_client) return;
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view_clip));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    std::string text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    
    std::string err;
    if (g_client->set_clipboard(text, err)) {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Clipboard successfully updated.");
    } else {
        gtk_label_set_text(GTK_LABEL(lbl_status), ("Error: " + err).c_str());
    }
}

void apply_css(GtkWidget* widget) {
    GtkCssProvider* provider = gtk_css_provider_new();
    const gchar* css = 
        "window { "
        "   background-color: @theme_bg_color; "
        "} "
        "textview { "
        "   background-color: @theme_base_color; "
        "   color: @theme_fg_color; "
        "   font-family: monospace; "
        "   border-radius: 8px; "
        "   padding: 12px; "
        "} "
        "button { "
        "   border-radius: 6px; "
        "   padding: 8px 16px; "
        "   font-weight: bold; "
        "} "
        "button:hover { "
        "   background-color: @theme_selected_bg_color; "
        "   color: @theme_selected_fg_color; "
        "} "
        ".header-label { "
        "   font-size: 16px; "
        "   font-weight: 800; "
        "   color: @theme_fg_color; "
        "} "
        ".status-label { "
        "   color: @theme_selected_bg_color; "
        "   font-weight: bold; "
        "}";
        
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void activate(GtkApplication* app, gpointer user_data) {
    // Scaffold initial network multiplexer natively
    if (!g_client) {
        g_client = new tether::Client();
        if (!g_client->connect()) {
            std::cerr << "CRITICAL: Could not engage local unix tetherd pipeline natively.\n";
            exit(1); 
        }
    }

    GtkWidget* window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Tether Control Center");
    gtk_window_set_default_size(GTK_WINDOW(window), 500, 400);

    // Prefer Dark Theme
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    apply_css(window);

    GtkWidget* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(main_box), 20);
    gtk_container_add(GTK_CONTAINER(window), main_box);

    // Header Frame
    GtkWidget* header = gtk_label_new("Wayland Clipboard Engine");
    gtk_widget_set_halign(header, GTK_ALIGN_START);
    GtkStyleContext* ctx = gtk_widget_get_style_context(header);
    gtk_style_context_add_class(ctx, "header-label");
    gtk_box_pack_start(GTK_BOX(main_box), header, FALSE, FALSE, 0);

    // Text View Frame
    GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scroll, TRUE);
    text_view_clip = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view_clip), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scroll), text_view_clip);
    gtk_box_pack_start(GTK_BOX(main_box), scroll, TRUE, TRUE, 0);

    // Button Row
    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* btn_get = gtk_button_new_with_label("Read From Host");
    GtkWidget* btn_set = gtk_button_new_with_label("Push To Host");
    
    g_signal_connect(btn_get, "clicked", G_CALLBACK(on_btn_get_clicked), NULL);
    g_signal_connect(btn_set, "clicked", G_CALLBACK(on_btn_set_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(btn_box), btn_get, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(btn_box), btn_set, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), btn_box, FALSE, FALSE, 0);

    // Status Footprint
    lbl_status = gtk_label_new("Connected natively via UNIX sockets.");
    gtk_widget_set_halign(lbl_status, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_status), "status-label");
    gtk_box_pack_start(GTK_BOX(main_box), lbl_status, FALSE, FALSE, 0);

    gtk_widget_show_all(window);
}

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("com.tether.desktop", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    if (g_client) { delete g_client; }
    return status;
}
