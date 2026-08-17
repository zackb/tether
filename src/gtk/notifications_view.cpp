#include "notifications_view.hpp"
#include "daemon_client.hpp"
#include "ui_util.hpp"

#include <ctime>
#include <string>

namespace tether::ui {

    namespace {

        struct NotificationsState {
            GtkWidget* status_label = nullptr;
            GtkWidget* list = nullptr;
            GtkWidget* stack = nullptr;
            bool visible = false;
            bool ready = false;
        };

        NotificationsState g_notifications;

        std::string format_timestamp(int64_t epoch) {
            if (epoch <= 0)
                return "";
            std::time_t t = static_cast<std::time_t>(epoch);
            std::tm tm{};
            localtime_r(&t, &tm);
            char buffer[32];
            std::strftime(buffer, sizeof(buffer), "%H:%M", &tm);
            return buffer;
        }

        void request_notifications() {
            nlohmann::json j;
            j["command"] = "bt_list_notifications";
            daemon_send(j);
        }

        GtkWidget* build_row(const nlohmann::json& notification) {
            const std::string app = notification.value("app_name", notification.value("app_id", ""));
            const std::string title = notification.value("title", "");
            const std::string body = notification.value("body", "");
            const std::string stamp = format_timestamp(notification.value("timestamp", static_cast<int64_t>(0)));

            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_container_set_border_width(GTK_CONTAINER(box), 10);

            GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GtkWidget* app_label = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(app_label), ("<b>" + escape_markup(app) + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(app_label), 0.0);
            gtk_box_pack_start(GTK_BOX(header), app_label, TRUE, TRUE, 0);

            if (!stamp.empty()) {
                GtkWidget* time_label = gtk_label_new(stamp.c_str());
                gtk_style_context_add_class(gtk_widget_get_style_context(time_label), "muted");
                gtk_box_pack_start(GTK_BOX(header), time_label, FALSE, FALSE, 0);
            }
            gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);

            // With content mirroring off, only the app is known — which is the
            // point of that default, so the row says so rather than looking broken.
            const std::string detail = !title.empty() ? title : (!body.empty() ? body : "New notification");
            GtkWidget* detail_label = gtk_label_new(detail.c_str());
            gtk_label_set_xalign(GTK_LABEL(detail_label), 0.0);
            gtk_label_set_line_wrap(GTK_LABEL(detail_label), TRUE);
            gtk_box_pack_start(GTK_BOX(box), detail_label, FALSE, FALSE, 0);

            if (!title.empty() && !body.empty()) {
                GtkWidget* body_label = gtk_label_new(body.c_str());
                gtk_label_set_xalign(GTK_LABEL(body_label), 0.0);
                gtk_label_set_line_wrap(GTK_LABEL(body_label), TRUE);
                gtk_style_context_add_class(gtk_widget_get_style_context(body_label), "muted");
                gtk_box_pack_start(GTK_BOX(box), body_label, FALSE, FALSE, 0);
            }

            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        }

        void show_notifications(const nlohmann::json& event) {
            clear_list_box(g_notifications.list);
            const bool empty = !event.contains("notifications") || event["notifications"].empty();
            if (!empty) {
                for (const auto& notification : event["notifications"])
                    gtk_list_box_insert(GTK_LIST_BOX(g_notifications.list), build_row(notification), -1);
            }
            gtk_widget_show_all(g_notifications.list);
            gtk_stack_set_visible_child_name(GTK_STACK(g_notifications.stack), empty ? "status" : "list");
        }

    } // namespace

    void notifications_view_set_visible(bool visible) {
        g_notifications.visible = visible;
        if (visible)
            request_notifications();
    }

    bool notifications_view_handle_event(const nlohmann::json& event) {
        const std::string command = event.value("command", "");

        if (command == "bt_notifications") {
            show_notifications(event);
            return true;
        }
        if (command == "bt_notification" || command == "bt_notification_removed") {
            if (g_notifications.visible)
                request_notifications();
            return true;
        }
        if (command == "bt_connection_changed") {
            g_notifications.ready = event.value("ancs_ready", false);
            // The daemon's reason names the actual next step, including the
            // toggle to enable on the phone, so it is shown verbatim.
            const std::string reason = event.value("ancs_reason", "");
            set_text(g_notifications.status_label,
                     g_notifications.ready ? "No notifications yet."
                                           : (reason.empty() ? "Notification mirroring is unavailable." : reason));
            return false;
        }
        return false;
    }

    GtkWidget* notifications_view_new() {
        g_notifications.stack = gtk_stack_new();

        GtkWidget* status_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_valign(status_box, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(status_box, GTK_ALIGN_CENTER);
        GtkWidget* icon =
            gtk_image_new_from_icon_name("preferences-system-notifications-symbolic", GTK_ICON_SIZE_DIALOG);
        gtk_box_pack_start(GTK_BOX(status_box), icon, FALSE, FALSE, 0);
        g_notifications.status_label = gtk_label_new("Waiting for the iPhone.");
        gtk_label_set_line_wrap(GTK_LABEL(g_notifications.status_label), TRUE);
        gtk_label_set_justify(GTK_LABEL(g_notifications.status_label), GTK_JUSTIFY_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_notifications.status_label), "muted");
        gtk_box_pack_start(GTK_BOX(status_box), g_notifications.status_label, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_notifications.stack), status_box, "status");

        GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        g_notifications.list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_notifications.list), GTK_SELECTION_NONE);
        gtk_container_add(GTK_CONTAINER(scroll), g_notifications.list);
        gtk_stack_add_named(GTK_STACK(g_notifications.stack), scroll, "list");

        gtk_stack_set_visible_child_name(GTK_STACK(g_notifications.stack), "status");
        return g_notifications.stack;
    }

} // namespace tether::ui
