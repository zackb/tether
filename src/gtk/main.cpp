#include "daemon_client.hpp"
#include "devices_view.hpp"
#include "messages_view.hpp"
#include "notifications_view.hpp"
#include "ui_util.hpp"

#include <gtk/gtk.h>
#include <tether/crypto.hpp>

namespace {

    using namespace tether::ui;

    GtkWidget* g_refresh_button = nullptr;

    void on_visible_view_changed(GObject* stack, GParamSpec*, gpointer) {
        const gchar* name = gtk_stack_get_visible_child_name(GTK_STACK(stack));
        const std::string view = name ? name : "";

        // Refresh means "scan for devices", which is meaningless on the other
        // views, so it only appears where it does something.
        if (g_refresh_button)
            gtk_widget_set_visible(g_refresh_button, view == "devices");

        messages_view_set_visible(view == "messages");
        notifications_view_set_visible(view == "notifications");
    }

    void activate(GtkApplication* app, gpointer) {
        tether::Crypto::instance().init();

        GtkWidget* window = gtk_application_window_new(app);
        gtk_window_set_title(GTK_WINDOW(window), "Tether");
        gtk_window_set_default_size(GTK_WINDOW(window), 820, 560);
        set_main_window(window);

        GtkWidget* header_bar = gtk_header_bar_new();
        gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header_bar), TRUE);
        gtk_header_bar_set_title(GTK_HEADER_BAR(header_bar), "Tether");
        gtk_window_set_titlebar(GTK_WINDOW(window), header_bar);
        set_header_bar(header_bar);

        g_refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
        g_signal_connect(g_refresh_button,
                         "clicked",
                         G_CALLBACK(+[](GtkWidget*, gpointer) { devices_view_trigger_discovery(); }),
                         nullptr);
        gtk_header_bar_pack_start(GTK_HEADER_BAR(header_bar), g_refresh_button);

        GtkWidget* stack = gtk_stack_new();
        gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_stack_add_titled(GTK_STACK(stack), devices_view_new(), "devices", "Devices");
        gtk_stack_add_titled(GTK_STACK(stack), messages_view_new(), "messages", "Messages");
        gtk_stack_add_titled(GTK_STACK(stack), notifications_view_new(), "notifications", "Notifications");

        GtkWidget* switcher = gtk_stack_switcher_new();
        gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
        gtk_header_bar_set_custom_title(GTK_HEADER_BAR(header_bar), switcher);

        g_signal_connect(stack, "notify::visible-child-name", G_CALLBACK(on_visible_view_changed), nullptr);
        gtk_container_add(GTK_CONTAINER(window), stack);

        // One feed, dispatched to whichever view owns the event; the views never
        // hold the socket themselves.
        daemon_client_start([](const nlohmann::json& event) {
            if (devices_view_handle_event(event))
                return;
            if (messages_view_handle_event(event))
                return;
            notifications_view_handle_event(event);
        });

        devices_view_trigger_discovery();
        devices_view_refresh();

        // Ask once at startup so the messages banner reflects reality before the
        // user ever opens that view.
        nlohmann::json connection;
        connection["command"] = "bt_connection";
        daemon_send(connection);

        gtk_widget_show_all(window);
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "devices");
    }

} // namespace

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("com.tether.desktop", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    tether::ui::daemon_client_stop();
    g_object_unref(app);
    return status;
}
