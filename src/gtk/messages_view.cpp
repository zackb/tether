#include "messages_view.hpp"
#include "daemon_client.hpp"
#include "ui_util.hpp"

#include <ctime>
#include <string>

namespace tether::ui {

    namespace {

        struct MessagesState {
            GtkWidget* banner = nullptr;
            GtkWidget* banner_label = nullptr;
            GtkWidget* thread_list = nullptr;
            GtkWidget* conversation = nullptr;
            GtkWidget* conversation_scroll = nullptr;
            GtkWidget* composer = nullptr;
            GtkWidget* send_button = nullptr;
            GtkWidget* placeholder_stack = nullptr;

            std::string selected_thread;
            std::string selected_name;
            bool visible = false;
            bool map_open = false;
            bool sending = false;
            // Whether the daemon says the selected thread can be replied to, and
            // why not when it cannot.
            bool selected_repliable = false;
            std::string selected_block_reason;
        };

        MessagesState g_messages;

        void update_composer_sensitivity();

        std::string format_timestamp(int64_t epoch) {
            if (epoch <= 0)
                return "";
            std::time_t t = static_cast<std::time_t>(epoch);
            std::tm tm{};
            localtime_r(&t, &tm);

            std::time_t now = std::time(nullptr);
            std::tm now_tm{};
            localtime_r(&now, &now_tm);

            char buffer[64];
            // Same day gets a time, anything older gets a date: a wall of
            // identical timestamps is harder to scan than a mixed list.
            const char* format =
                (tm.tm_year == now_tm.tm_year && tm.tm_yday == now_tm.tm_yday) ? "%H:%M" : "%b %-d, %H:%M";
            std::strftime(buffer, sizeof(buffer), format, &tm);
            return buffer;
        }

        void request_threads() {
            nlohmann::json j;
            j["command"] = "bt_list_threads";
            daemon_send(j);
        }

        void request_messages(const std::string& thread_key) {
            if (thread_key.empty())
                return;
            nlohmann::json j;
            j["command"] = "bt_list_messages";
            j["thread"] = thread_key;
            daemon_send(j);
        }

        GtkWidget* build_thread_row(const nlohmann::json& thread) {
            const std::string key = thread.value("thread", "");
            const std::string address = thread.value("address", "");
            const std::string name = thread.value("name", address);
            const std::string preview = thread.value("preview", "");
            const int unread = thread.value("unread", 0);

            GtkWidget* row = gtk_list_box_row_new();
            g_object_set_data_full(G_OBJECT(row), "thread", g_strdup(key.c_str()), g_free);
            g_object_set_data_full(G_OBJECT(row), "name", g_strdup(name.c_str()), g_free);
            // The daemon owns the decision about whether a group can be replied
            // to; the UI must not re-derive it from the key shape.
            g_object_set_data(G_OBJECT(row), "repliable", GINT_TO_POINTER(thread.value("repliable", true) ? 1 : 0));
            g_object_set_data_full(
                G_OBJECT(row), "reply_reason", g_strdup(thread.value("reply_reason", "").c_str()), g_free);
            g_object_set_data(G_OBJECT(row), "group", GINT_TO_POINTER(thread.value("group", false) ? 1 : 0));

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_container_set_border_width(GTK_CONTAINER(box), 10);

            GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

            GtkWidget* title = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(title), ("<b>" + escape_markup(name) + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
            gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

            GtkWidget* subtitle = gtk_label_new(preview.c_str());
            gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
            gtk_label_set_single_line_mode(GTK_LABEL(subtitle), TRUE);
            gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
            gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);

            gtk_box_pack_start(GTK_BOX(box), labels, TRUE, TRUE, 0);

            if (unread > 0) {
                GtkWidget* badge = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(badge), ("<b>" + std::to_string(unread) + "</b>").c_str());
                gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
                gtk_box_pack_start(GTK_BOX(box), badge, FALSE, FALSE, 0);
            }

            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        }

        GtkWidget* build_message_row(const nlohmann::json& message) {
            const bool outgoing = message.value("outgoing", false);
            const std::string body = message.value("body", "");
            const std::string stamp = format_timestamp(message.value("timestamp", static_cast<int64_t>(0)));

            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_container_set_border_width(GTK_CONTAINER(box), 8);
            gtk_widget_set_halign(box, outgoing ? GTK_ALIGN_END : GTK_ALIGN_START);

            GtkWidget* bubble = gtk_label_new(body.c_str());
            gtk_label_set_line_wrap(GTK_LABEL(bubble), TRUE);
            gtk_label_set_line_wrap_mode(GTK_LABEL(bubble), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_max_width_chars(GTK_LABEL(bubble), 48);
            gtk_label_set_xalign(GTK_LABEL(bubble), outgoing ? 1.0 : 0.0);
            gtk_label_set_selectable(GTK_LABEL(bubble), TRUE);
            gtk_box_pack_start(GTK_BOX(box), bubble, FALSE, FALSE, 0);

            if (!stamp.empty()) {
                GtkWidget* time_label = gtk_label_new(stamp.c_str());
                gtk_label_set_xalign(GTK_LABEL(time_label), outgoing ? 1.0 : 0.0);
                gtk_style_context_add_class(gtk_widget_get_style_context(time_label), "muted");
                gtk_box_pack_start(GTK_BOX(box), time_label, FALSE, FALSE, 0);
            }

            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        }

        void scroll_conversation_to_end() {
            if (!g_messages.conversation_scroll)
                return;
            GtkAdjustment* adjustment =
                gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(g_messages.conversation_scroll));
            if (adjustment)
                gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment));
        }

        void show_threads(const nlohmann::json& event) {
            clear_list_box(g_messages.thread_list);
            if (!event.contains("threads") || !event["threads"].is_array())
                return;

            GtkWidget* reselect = nullptr;
            for (const auto& thread : event["threads"]) {
                GtkWidget* row = build_thread_row(thread);
                gtk_list_box_insert(GTK_LIST_BOX(g_messages.thread_list), row, -1);
                if (thread.value("thread", "") == g_messages.selected_thread)
                    reselect = row;
            }
            gtk_widget_show_all(g_messages.thread_list);

            // Rebuilding the list drops the selection; restoring it keeps the
            // open conversation from closing every time a message arrives.
            if (reselect)
                gtk_list_box_select_row(GTK_LIST_BOX(g_messages.thread_list), GTK_LIST_BOX_ROW(reselect));
        }

        void show_messages(const nlohmann::json& event) {
            if (event.value("thread", "") != g_messages.selected_thread)
                return;

            clear_list_box(g_messages.conversation);
            if (event.contains("messages") && event["messages"].is_array()) {
                for (const auto& message : event["messages"])
                    gtk_list_box_insert(GTK_LIST_BOX(g_messages.conversation), build_message_row(message), -1);
            }
            gtk_widget_show_all(g_messages.conversation);
            scroll_conversation_to_end();

            // Opening a conversation marks it read here and on the phone, which
            // is what makes the unread badge agree with what the user has seen.
            if (event.contains("messages") && event["messages"].is_array()) {
                for (const auto& message : event["messages"]) {
                    if (message.value("read", true) || message.value("outgoing", false))
                        continue;
                    nlohmann::json j;
                    j["command"] = "bt_mark_read";
                    j["handle"] = message.value("handle", "");
                    j["read"] = true;
                    daemon_send(j);
                }
            }
        }

        void set_banner(const std::string& text) {
            if (!g_messages.banner)
                return;
            if (text.empty()) {
                gtk_widget_hide(g_messages.banner);
                return;
            }
            set_text(g_messages.banner_label, text);
            gtk_widget_show_all(g_messages.banner);
        }

        void update_connection(const nlohmann::json& event) {
            const bool map_open = event.value("map_open", false);
            g_messages.map_open = map_open;
            update_composer_sensitivity();
            if (map_open) {
                set_banner("");
                if (g_messages.visible)
                    request_threads();
                return;
            }
            // The daemon's reason strings name the actual next step, including
            // which toggle to flip on the phone, so they are shown verbatim
            // rather than replaced with something vaguer.
            std::string reason = event.value("profile_reason", "");
            if (reason.empty())
                reason = event.value("link_reason", "");
            if (reason.empty())
                reason = "Messages are not connected.";
            set_banner(reason);
        }

        // Replying needs an open conversation and a live MAP session. A group
        // thread has no reply routing yet, and the daemon refuses one anyway, so
        // the composer stays shut rather than accepting text that will bounce.
        void update_composer_sensitivity() {
            const bool can_send =
                g_messages.map_open && !g_messages.selected_thread.empty() && g_messages.selected_repliable;
            if (!g_messages.composer)
                return;

            gtk_widget_set_sensitive(g_messages.composer, can_send && !g_messages.sending);
            gtk_widget_set_sensitive(g_messages.send_button, can_send && !g_messages.sending);

            const char* reason = nullptr;
            if (g_messages.sending)
                reason = "Sending…";
            else if (!g_messages.map_open)
                reason = "Messages are not connected.";
            else if (g_messages.selected_thread.empty())
                reason = "Select a conversation first.";
            else if (!g_messages.selected_block_reason.empty())
                reason = g_messages.selected_block_reason.c_str();
            else if (!can_send)
                reason = "Replying to this conversation is not available.";
            gtk_widget_set_tooltip_text(g_messages.send_button, reason);
        }

        std::string composer_text() {
            GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_messages.composer));
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buffer, &start, &end);
            gchar* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
            std::string out = text ? text : "";
            g_free(text);
            return out;
        }

        void on_send_clicked(GtkWidget*, gpointer) {
            const std::string body = composer_text();
            if (body.empty() || g_messages.selected_thread.empty())
                return;

            nlohmann::json j;
            j["command"] = "bt_send_message";
            j["thread"] = g_messages.selected_thread;
            j["body"] = body;
            daemon_send(j);

            // The composer stays locked until the phone answers. Sending is a
            // real OBEX transfer and takes a moment; an unlocked box invites a
            // second copy of the same message.
            g_messages.sending = true;
            update_composer_sensitivity();
            set_status_main("Sending…");
        }

        void on_send_result(const nlohmann::json& event) {
            g_messages.sending = false;
            update_composer_sensitivity();

            if (event.value("success", false)) {
                // Only cleared once the phone accepted it, so a failed send
                // leaves the text where the user can retry or copy it out.
                GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_messages.composer));
                gtk_text_buffer_set_text(buffer, "", -1);
                set_status_main("Sent");
                return;
            }
            set_status_main(event.value("message", "The message was not sent."));
        }

        void on_thread_selected(GtkListBox*, GtkListBoxRow* row, gpointer) {
            if (!row) {
                g_messages.selected_thread.clear();
                g_messages.selected_repliable = false;
                g_messages.selected_block_reason.clear();
                update_composer_sensitivity();
                return;
            }
            const char* thread = (const char*)g_object_get_data(G_OBJECT(row), "thread");
            const char* name = (const char*)g_object_get_data(G_OBJECT(row), "name");
            const char* block_reason = (const char*)g_object_get_data(G_OBJECT(row), "reply_reason");
            g_messages.selected_thread = thread ? thread : "";
            g_messages.selected_name = name ? name : "";
            g_messages.selected_repliable = g_object_get_data(G_OBJECT(row), "repliable") != nullptr;
            g_messages.selected_block_reason = block_reason ? block_reason : "";
            update_composer_sensitivity();
            gtk_stack_set_visible_child_name(GTK_STACK(g_messages.placeholder_stack), "conversation");
            request_messages(g_messages.selected_thread);
        }

    } // namespace

    void messages_view_set_visible(bool visible) {
        g_messages.visible = visible;
        if (visible)
            request_threads();
    }

    bool messages_view_handle_event(const nlohmann::json& event) {
        const std::string command = event.value("command", "");
        if (command == "bt_threads") {
            show_threads(event);
            return true;
        }
        if (command == "bt_messages") {
            show_messages(event);
            return true;
        }
        if (command == "bt_connection_changed") {
            update_connection(event);
            return true;
        }
        if (command == "bt_message") {
            // A new message changes both the thread list and, when it lands in
            // the open conversation, the conversation itself. Refreshing while
            // the view is hidden would just be traffic nobody sees; switching
            // back re-reads everything anyway.
            if (!g_messages.visible)
                return true;
            request_threads();
            if (event.value("thread", "") == g_messages.selected_thread)
                request_messages(g_messages.selected_thread);
            return true;
        }
        if (command == "bt_send_result") {
            on_send_result(event);
            return true;
        }
        if (command == "bt_message_read") {
            if (g_messages.visible)
                request_threads();
            return true;
        }
        return false;
    }

    GtkWidget* messages_view_new() {
        GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        g_messages.banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(g_messages.banner), 8);
        GtkWidget* banner_icon = gtk_image_new_from_icon_name("dialog-information-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_box_pack_start(GTK_BOX(g_messages.banner), banner_icon, FALSE, FALSE, 0);
        g_messages.banner_label = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_messages.banner_label), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_messages.banner_label), TRUE);
        gtk_box_pack_start(GTK_BOX(g_messages.banner), g_messages.banner_label, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(root), g_messages.banner, FALSE, FALSE, 0);

        GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_box_pack_start(GTK_BOX(root), paned, TRUE, TRUE, 0);

        GtkWidget* thread_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(thread_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(thread_scroll, 240, -1);
        g_messages.thread_list = gtk_list_box_new();
        g_signal_connect(g_messages.thread_list, "row-selected", G_CALLBACK(on_thread_selected), nullptr);
        gtk_container_add(GTK_CONTAINER(thread_scroll), g_messages.thread_list);
        gtk_paned_pack1(GTK_PANED(paned), thread_scroll, FALSE, FALSE);

        g_messages.placeholder_stack = gtk_stack_new();

        GtkWidget* placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER);
        GtkWidget* placeholder_icon = gtk_image_new_from_icon_name("mail-unread-symbolic", GTK_ICON_SIZE_DIALOG);
        gtk_box_pack_start(GTK_BOX(placeholder), placeholder_icon, FALSE, FALSE, 0);
        GtkWidget* placeholder_label = gtk_label_new("Select a conversation");
        gtk_style_context_add_class(gtk_widget_get_style_context(placeholder_label), "muted");
        gtk_box_pack_start(GTK_BOX(placeholder), placeholder_label, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_messages.placeholder_stack), placeholder, "placeholder");

        GtkWidget* conversation_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        g_messages.conversation_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(
            GTK_SCROLLED_WINDOW(g_messages.conversation_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        g_messages.conversation = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_messages.conversation), GTK_SELECTION_NONE);
        gtk_container_add(GTK_CONTAINER(g_messages.conversation_scroll), g_messages.conversation);
        gtk_box_pack_start(GTK_BOX(conversation_box), g_messages.conversation_scroll, TRUE, TRUE, 0);

        GtkWidget* composer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(composer_box), 8);
        g_messages.composer = gtk_text_view_new();
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_messages.composer), GTK_WRAP_WORD_CHAR);
        gtk_widget_set_size_request(g_messages.composer, -1, 48);
        GtkWidget* composer_frame = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(composer_frame), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(composer_frame), g_messages.composer);
        gtk_box_pack_start(GTK_BOX(composer_box), composer_frame, TRUE, TRUE, 0);

        g_messages.send_button = gtk_button_new_with_label("Send");
        gtk_widget_set_valign(g_messages.send_button, GTK_ALIGN_END);
        gtk_box_pack_start(GTK_BOX(composer_box), g_messages.send_button, FALSE, FALSE, 0);

        g_signal_connect(g_messages.send_button, "clicked", G_CALLBACK(on_send_clicked), nullptr);
        // Enabled only once MAP is up and a conversation is open.
        update_composer_sensitivity();

        gtk_box_pack_start(GTK_BOX(conversation_box), composer_box, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_messages.placeholder_stack), conversation_box, "conversation");

        gtk_paned_pack2(GTK_PANED(paned), g_messages.placeholder_stack, TRUE, FALSE);
        gtk_stack_set_visible_child_name(GTK_STACK(g_messages.placeholder_stack), "placeholder");

        gtk_widget_set_no_show_all(g_messages.banner, TRUE);
        return root;
    }

} // namespace tether::ui
