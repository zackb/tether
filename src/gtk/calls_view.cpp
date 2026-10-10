#include "calls_view.hpp"
#include "contact_completion.hpp"
#include "daemon_client.hpp"
#include "ui_util.hpp"
#include <tether/i18n.hpp>

#include <ctime>
#include <string>

namespace tether::ui {

    namespace {

        struct CallsState {
            GtkWidget* status_label = nullptr;
            GtkWidget* list = nullptr;
            GtkWidget* history = nullptr;
            GtkWidget* stack = nullptr;
            GtkWidget* entry = nullptr;
            GtkWidget* dial_button = nullptr;
            GtkWidget* network_label = nullptr;
            bool visible = false;
            bool available = false;
            bool audio_routable = false;
            bool has_calls = false;
            bool has_history = false;
        };

        CallsState g_calls;

        void request_calls() {
            nlohmann::json j;
            j["command"] = "bt_list_calls";
            daemon_send(j);
            j["command"] = "bt_list_call_history";
            daemon_send(j);
        }

        // Live calls take the page; otherwise recent calls; otherwise the status text.
        void update_stack() {
            const char* page = g_calls.has_calls ? "list" : (g_calls.has_history ? "history" : "status");
            gtk_stack_set_visible_child_name(GTK_STACK(g_calls.stack), page);
        }

        void send_action(const std::string& action, const std::string& path) {
            nlohmann::json j;
            j["command"] = "bt_call_action";
            j["action"] = action;
            if (!path.empty())
                j["path"] = path;
            daemon_send(j);
        }

        // The row's call path, owned by the button.
        std::string button_path(GtkButton* button) {
            const char* path = static_cast<const char*>(g_object_get_data(G_OBJECT(button), "call-path"));
            return path ? path : "";
        }

        void on_answer_clicked(GtkButton* button, gpointer) { send_action("answer", button_path(button)); }

        void on_audio_here_clicked(GtkButton*, gpointer) { send_action("audio_here", ""); }

        void on_hangup_clicked(GtkButton* button, gpointer) { send_action("hangup", button_path(button)); }

        void on_dial_clicked(GtkButton*, gpointer) {
            const gchar* text = gtk_entry_get_text(GTK_ENTRY(g_calls.entry));
            const std::string number = text ? text : "";
            if (number.empty())
                return;
            nlohmann::json j;
            j["command"] = "bt_call_dial";
            j["number"] = number;
            daemon_send(j);
            gtk_entry_set_text(GTK_ENTRY(g_calls.entry), "");
        }

        void on_redial_clicked(GtkButton* button, gpointer) {
            const char* number = static_cast<const char*>(g_object_get_data(G_OBJECT(button), "call-number"));
            if (!number || !*number)
                return;
            nlohmann::json j;
            j["command"] = "bt_call_dial";
            j["number"] = number;
            daemon_send(j);
        }

        void attach_path(GtkWidget* button, const std::string& path) {
            g_object_set_data_full(G_OBJECT(button), "call-path", g_strdup(path.c_str()), g_free);
        }

        GtkWidget* build_row(const nlohmann::json& call) {
            const std::string number = call.value("number", "");
            const std::string name = call.value("name", "");
            const std::string state = call.value("state", "");
            const std::string path = call.value("path", "");
            const bool ringing = call.value("ringing", false);

            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_container_set_border_width(GTK_CONTAINER(box), 10);

            gtk_box_pack_start(
                GTK_BOX(box),
                gtk_image_new_from_icon_name(ringing ? "call-incoming" : "call-start", GTK_ICON_SIZE_LARGE_TOOLBAR),
                FALSE,
                FALSE,
                0);

            GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            const std::string primary = !name.empty() ? name : (!number.empty() ? number : _("Unknown caller"));
            GtkWidget* primary_label = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(primary_label), ("<b>" + escape_markup(primary) + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(primary_label), 0.0);
            gtk_box_pack_start(GTK_BOX(text), primary_label, FALSE, FALSE, 0);

            std::string secondary = call_state_text(state);
            if (!name.empty() && !number.empty())
                secondary += secondary.empty() ? number : "  -  " + number;

            GtkWidget* secondary_label = gtk_label_new(secondary.c_str());
            gtk_label_set_xalign(GTK_LABEL(secondary_label), 0.0);
            gtk_style_context_add_class(gtk_widget_get_style_context(secondary_label), "muted");
            gtk_box_pack_start(GTK_BOX(text), secondary_label, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);

            if (ringing) {
                GtkWidget* answer = gtk_button_new_with_label(_("Answer"));
                gtk_style_context_add_class(gtk_widget_get_style_context(answer), "suggested-action");
                attach_path(answer, path);
                g_signal_connect(answer, "clicked", G_CALLBACK(on_answer_clicked), nullptr);
                gtk_widget_set_valign(answer, GTK_ALIGN_CENTER);
                gtk_box_pack_start(GTK_BOX(box), answer, FALSE, FALSE, 0);
            }

            if (call.value("connected", false) && g_calls.audio_routable) {
                GtkWidget* audio = gtk_button_new_with_label(_("Audio here"));
                g_signal_connect(audio, "clicked", G_CALLBACK(on_audio_here_clicked), nullptr);
                gtk_widget_set_valign(audio, GTK_ALIGN_CENTER);
                gtk_box_pack_start(GTK_BOX(box), audio, FALSE, FALSE, 0);
            }

            if (state != "disconnected") {
                GtkWidget* hangup = gtk_button_new_with_label(ringing ? _("Decline") : _("Hang up"));
                gtk_style_context_add_class(gtk_widget_get_style_context(hangup), "destructive-action");
                attach_path(hangup, path);
                g_signal_connect(hangup, "clicked", G_CALLBACK(on_hangup_clicked), nullptr);
                gtk_widget_set_valign(hangup, GTK_ALIGN_CENTER);
                gtk_box_pack_start(GTK_BOX(box), hangup, FALSE, FALSE, 0);
            }

            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        }

        GtkWidget* build_history_row(const nlohmann::json& entry) {
            const std::string number = entry.value("number", "");
            const std::string name = entry.value("name", "");
            const std::string type = entry.value("type", "");

            const char* icon = "call-incoming-symbolic";
            const char* kind = _("Incoming");
            if (type == "missed") {
                icon = "call-missed-symbolic";
                kind = _("Missed");
            } else if (type == "dialed") {
                icon = "call-outgoing-symbolic";
                kind = _("Outgoing");
            }

            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_container_set_border_width(GTK_CONTAINER(box), 10);
            GtkWidget* image = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
            set_accessible_name(image, kind);
            gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);

            GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            const std::string primary = !name.empty() ? name : (!number.empty() ? number : _("Unknown caller"));
            GtkWidget* primary_label = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(primary_label), ("<b>" + escape_markup(primary) + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(primary_label), 0.0);
            if (type == "missed")
                gtk_style_context_add_class(gtk_widget_get_style_context(primary_label), "error");
            gtk_box_pack_start(GTK_BOX(text), primary_label, FALSE, FALSE, 0);

            std::string secondary = kind;
            if (!name.empty() && !number.empty())
                secondary += "  -  " + number;
            if (const std::string when = format_call_time(entry.value("timestamp", static_cast<int64_t>(0)));
                !when.empty())
                secondary += "  -  " + when;
            GtkWidget* secondary_label = gtk_label_new(secondary.c_str());
            gtk_label_set_xalign(GTK_LABEL(secondary_label), 0.0);
            gtk_style_context_add_class(gtk_widget_get_style_context(secondary_label), "muted");
            gtk_box_pack_start(GTK_BOX(text), secondary_label, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);

            if (!number.empty()) {
                GtkWidget* call = gtk_button_new_from_icon_name("call-start-symbolic", GTK_ICON_SIZE_BUTTON);
                // TRANSLATORS: Button that calls a recent caller back, {} is their name or number.
                const std::string label = tether::tr_format(_("Call {}"), primary);
                gtk_widget_set_tooltip_text(call, label.c_str());
                set_accessible_name(call, label);
                g_object_set_data_full(G_OBJECT(call), "call-number", g_strdup(number.c_str()), g_free);
                g_signal_connect(call, "clicked", G_CALLBACK(on_redial_clicked), nullptr);
                gtk_widget_set_sensitive(call, g_calls.available);
                gtk_widget_set_valign(call, GTK_ALIGN_CENTER);
                gtk_box_pack_start(GTK_BOX(box), call, FALSE, FALSE, 0);
            }

            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        }

        void show_history(const nlohmann::json& event) {
            clear_list_box(g_calls.history);
            const auto& calls = event.contains("calls") ? event["calls"] : nlohmann::json();
            g_calls.has_history = calls.is_array() && !calls.empty();
            if (g_calls.has_history)
                for (const auto& entry : calls)
                    gtk_list_box_insert(GTK_LIST_BOX(g_calls.history), build_history_row(entry), -1);
            gtk_widget_show_all(g_calls.history);
            update_stack();
        }

        void show_calls(const nlohmann::json& event) {
            clear_list_box(g_calls.list);
            const bool empty = !event.contains("calls") || event["calls"].empty();
            if (!empty) {
                for (const auto& call : event["calls"])
                    gtk_list_box_insert(GTK_LIST_BOX(g_calls.list), build_row(call), -1);
            }
            gtk_widget_show_all(g_calls.list);
            g_calls.has_calls = !empty;
            update_stack();
        }

    } // namespace

    void calls_view_set_visible(bool visible) {
        g_calls.visible = visible;
        if (!visible)
            return;
        request_calls();
        // Nothing else pulls the address book when this is the first view opened.
        contact_completion_request();
    }

    bool calls_view_handle_event(const nlohmann::json& event) {
        const std::string command = event.value("command", "");

        if (command == "bt_calls") {
            show_calls(event);
            return true;
        }
        if (command == "bt_call_history") {
            show_history(event);
            return true;
        }
        if (command == "bt_call_result") {
            if (!event.value("success", false))
                set_status_main(event.value("message", _("The call could not be placed.")));
            return true;
        }
        if (command == "bt_connection_changed") {
            const auto& calls = event.contains("calls") ? event["calls"] : nlohmann::json();
            g_calls.available = calls.is_object() && calls.value("available", false);

            gtk_widget_set_sensitive(g_calls.entry, g_calls.available);
            gtk_widget_set_sensitive(g_calls.dial_button, g_calls.available);
            set_text(g_calls.network_label, g_calls.available ? network_text(calls) : "");
            set_accessible_name(g_calls.network_label, g_calls.available ? network_text(calls, true) : "");

            const std::string reason = calls.is_object() ? calls.value("reason", "") : "";
            const std::string audio = calls.is_object() ? calls.value("audio", "") : "";
            g_calls.audio_routable = !audio.empty() && audio != "active";
            set_text(g_calls.status_label,
                     g_calls.available
                         ? std::string(_("No calls.")) + (reason.empty() ? "" : "\n" + reason)
                         : (reason.empty() ? _("Call control is off. Turn it on with 'tether --bt-calls-enable on'.")
                                           : reason));
            if (g_calls.visible)
                request_calls();
            return false;
        }
        return false;
    }

    GtkWidget* calls_view_new() {
        GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        GtkWidget* dial_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(dial_bar), 24);
        g_calls.entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(g_calls.entry), _("Number to call"));
        set_accessible_name(g_calls.entry, _("Number to call"));
        gtk_entry_set_input_purpose(GTK_ENTRY(g_calls.entry), GTK_INPUT_PURPOSE_PHONE);
        attach_contact_completion(g_calls.entry, ContactKind::Tel);
        gtk_widget_set_sensitive(g_calls.entry, FALSE);
        g_signal_connect(g_calls.entry, "activate", G_CALLBACK(on_dial_clicked), nullptr);
        gtk_box_pack_start(GTK_BOX(dial_bar), g_calls.entry, TRUE, TRUE, 0);

        g_calls.dial_button = gtk_button_new_with_label(_("Call"));
        gtk_style_context_add_class(gtk_widget_get_style_context(g_calls.dial_button), "suggested-action");
        gtk_widget_set_sensitive(g_calls.dial_button, FALSE);
        g_signal_connect(g_calls.dial_button, "clicked", G_CALLBACK(on_dial_clicked), nullptr);
        gtk_box_pack_start(GTK_BOX(dial_bar), g_calls.dial_button, FALSE, FALSE, 0);

        g_calls.network_label = gtk_label_new("");
        gtk_widget_set_tooltip_text(g_calls.network_label, _("The iPhone's carrier, signal, and battery."));
        gtk_style_context_add_class(gtk_widget_get_style_context(g_calls.network_label), "muted");
        gtk_box_pack_start(GTK_BOX(dial_bar), g_calls.network_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(root), dial_bar, FALSE, FALSE, 0);

        g_calls.stack = gtk_stack_new();

        GtkWidget* status_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_valign(status_box, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(status_box, GTK_ALIGN_CENTER);
        gtk_box_pack_start(
            GTK_BOX(status_box), gtk_image_new_from_icon_name("call-start", GTK_ICON_SIZE_DIALOG), FALSE, FALSE, 0);
        g_calls.status_label = gtk_label_new(_("Waiting for the iPhone."));
        gtk_label_set_line_wrap(GTK_LABEL(g_calls.status_label), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(g_calls.status_label), 42);
        style(status_box, "tether-empty");
        gtk_label_set_justify(GTK_LABEL(g_calls.status_label), GTK_JUSTIFY_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_calls.status_label), "muted");
        gtk_box_pack_start(GTK_BOX(status_box), g_calls.status_label, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_calls.stack), status_box, "status");

        GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        g_calls.list = gtk_list_box_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(g_calls.list), "tether-feed");
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_calls.list), GTK_SELECTION_NONE);
        gtk_container_add(GTK_CONTAINER(scroll), g_calls.list);
        gtk_stack_add_named(GTK_STACK(g_calls.stack), scroll, "list");

        GtkWidget* history_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(history_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        g_calls.history = gtk_list_box_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(g_calls.history), "tether-feed");
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_calls.history), GTK_SELECTION_NONE);
        set_accessible_name(g_calls.history, _("Recent calls"));
        gtk_container_add(GTK_CONTAINER(history_scroll), g_calls.history);
        gtk_stack_add_named(GTK_STACK(g_calls.stack), history_scroll, "history");

        gtk_stack_set_visible_child_name(GTK_STACK(g_calls.stack), "status");
        gtk_box_pack_start(GTK_BOX(root), g_calls.stack, TRUE, TRUE, 0);
        return root;
    }

} // namespace tether::ui
