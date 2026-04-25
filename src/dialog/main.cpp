#include <gtk-layer-shell.h>
#include <gtk/gtk.h>

#include "../core/include/tether/log.hpp"
#include <cstdlib>
#include <cstring>
#include <string>

// Exit codes:
//   0 = accept button clicked
//   1 = reject button clicked
//   2 = timeout expired
//   3 = error (bad args, display failure, etc.)
static int exit_code = 3;

static void on_accept(GtkWidget*, gpointer) {
    exit_code = 0;
    gtk_main_quit();
}

static void on_reject(GtkWidget*, gpointer) {
    exit_code = 1;
    gtk_main_quit();
}

static gboolean on_timeout(gpointer) {
    exit_code = 2;
    gtk_main_quit();
    return FALSE;
}

static gboolean on_key_press(GtkWidget*, GdkEventKey* event, gpointer has_reject) {
    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        exit_code = 0;
        gtk_main_quit();
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Escape && GPOINTER_TO_INT(has_reject)) {
        exit_code = 1;
        gtk_main_quit();
        return TRUE;
    }
    return FALSE;
}

static void print_usage(const char* argv0) {
    debug::log(ERR,
               "Usage: {} --title <text> --body <text> --accept <label> [--reject <label>] [--timeout <seconds>]\n",
               argv0);
}

static const char* CSS = R"(
    window {
        background-color: transparent;
    }
    .dialog-frame {
        background-color: rgba(24, 24, 32, 0.92);
        border: 1px solid rgba(255, 255, 255, 0.08);
        border-radius: 16px;
        padding: 28px 32px;
        margin: 12px;
    }
    .dialog-title {
        font-size: 16px;
        font-weight: 700;
        color: #e8e8ef;
        margin-bottom: 10px;
    }
    .dialog-body {
        font-size: 13px;
        color: #a0a0b0;
        margin-bottom: 22px;
    }
    .dialog-btn-row {
        margin-top: 4px;
    }
    .btn-accept {
        background: linear-gradient(135deg, #6366f1, #818cf8);
        color: white;
        border: none;
        border-radius: 8px;
        padding: 8px 22px;
        font-weight: 600;
        font-size: 13px;
        min-width: 90px;
    }
    .btn-accept:hover {
        background: linear-gradient(135deg, #818cf8, #a5b4fc);
    }
    .btn-reject {
        background-color: rgba(255, 255, 255, 0.06);
        color: #a0a0b0;
        border: 1px solid rgba(255, 255, 255, 0.1);
        border-radius: 8px;
        padding: 8px 22px;
        font-weight: 600;
        font-size: 13px;
        min-width: 90px;
    }
    .btn-reject:hover {
        background-color: rgba(255, 255, 255, 0.12);
        color: #e8e8ef;
    }
)";

int main(int argc, char** argv) {
    std::string title, body, accept_label, reject_label;
    int timeout_secs = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--title" && i + 1 < argc) {
            title = argv[++i];
        } else if (arg == "--body" && i + 1 < argc) {
            body = argv[++i];
        } else if (arg == "--accept" && i + 1 < argc) {
            accept_label = argv[++i];
        } else if (arg == "--reject" && i + 1 < argc) {
            reject_label = argv[++i];
        } else if (arg == "--timeout" && i + 1 < argc) {
            timeout_secs = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (title.empty() || body.empty() || accept_label.empty()) {
        debug::log(ERR, "Error: --title, --body, and --accept are required.\n");
        print_usage(argv[0]);
        return 3;
    }

    gtk_init(&argc, &argv);

    // Apply CSS
    GtkCssProvider* css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);

    // Create window
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 380, -1);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    // Layer shell setup
    gtk_layer_init_for_window(GTK_WINDOW(window));
    gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_layer_set_namespace(GTK_WINDOW(window), "tether-dialog");

    // Center on screen (no anchors = centered)

    // Main container
    GtkWidget* frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkStyleContext* frame_ctx = gtk_widget_get_style_context(frame);
    gtk_style_context_add_class(frame_ctx, "dialog-frame");
    gtk_container_add(GTK_CONTAINER(window), frame);

    // Title
    GtkWidget* lbl_title = gtk_label_new(title.c_str());
    gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(lbl_title), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_title), "dialog-title");
    gtk_box_pack_start(GTK_BOX(frame), lbl_title, FALSE, FALSE, 0);

    // Body
    GtkWidget* lbl_body = gtk_label_new(body.c_str());
    gtk_label_set_xalign(GTK_LABEL(lbl_body), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(lbl_body), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl_body), 50);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_body), "dialog-body");
    gtk_box_pack_start(GTK_BOX(frame), lbl_body, FALSE, FALSE, 0);

    // Button row
    GtkWidget* btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_row), "dialog-btn-row");
    gtk_widget_set_halign(btn_row, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(frame), btn_row, FALSE, FALSE, 0);

    bool has_reject = !reject_label.empty();

    if (has_reject) {
        GtkWidget* btn_reject = gtk_button_new_with_label(reject_label.c_str());
        gtk_style_context_add_class(gtk_widget_get_style_context(btn_reject), "btn-reject");
        g_signal_connect(btn_reject, "clicked", G_CALLBACK(on_reject), NULL);
        gtk_box_pack_start(GTK_BOX(btn_row), btn_reject, FALSE, FALSE, 0);
    }

    GtkWidget* btn_accept = gtk_button_new_with_label(accept_label.c_str());
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_accept), "btn-accept");
    g_signal_connect(btn_accept, "clicked", G_CALLBACK(on_accept), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), btn_accept, FALSE, FALSE, 0);

    // Keyboard shortcuts
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), GINT_TO_POINTER(has_reject));

    // Auto-dismiss timeout
    if (timeout_secs > 0) {
        g_timeout_add_seconds(timeout_secs, on_timeout, NULL);
    }

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(window);
    gtk_main();

    return exit_code;
}
