#include "contacts_view.hpp"
#include "daemon_client.hpp"
#include "ui_util.hpp"
#include <tether/i18n.hpp>

#include <cstring>
#include <string>

namespace tether::ui {

    namespace {

        struct ContactsState {
            GtkWidget* stack = nullptr;
            GtkWidget* status_label = nullptr;
            GtkWidget* list = nullptr;
            GtkWidget* search_entry = nullptr;
            std::string search_needle;
            std::string shown;
            bool visible = false;
            void (*open_thread)(const std::string&) = nullptr;
        };

        ContactsState g_contacts;

        constexpr int CONTACT_LIMIT = 5000;

        void request_contacts() {
            nlohmann::json j;
            j["command"] = "bt_list_contacts";
            j["limit"] = CONTACT_LIMIT;
            daemon_send(j);
        }

        gboolean contact_visible(GtkListBoxRow* row, gpointer) {
            if (g_contacts.search_needle.empty())
                return TRUE;
            const char* haystack = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "search"));
            return haystack && std::strstr(haystack, g_contacts.search_needle.c_str()) != nullptr;
        }

        void on_search_changed(GtkSearchEntry* entry, gpointer) {
            g_contacts.search_needle = fold(gtk_entry_get_text(GTK_ENTRY(entry)));
            gtk_list_box_invalidate_filter(GTK_LIST_BOX(g_contacts.list));
        }

        // Addresses arrive namespaced the way thread keys are. The prefix is
        // what makes them sendable, so it is kept on the button and stripped
        // only for display.
        std::string display_address(const std::string& key) {
            const size_t colon = key.find(':');
            return colon == std::string::npos ? key : key.substr(colon + 1);
        }

        const char* stashed(GtkWidget* widget) {
            const char* value = static_cast<const char*>(g_object_get_data(G_OBJECT(widget), "address"));
            return value ? value : "";
        }

        // "name" is spoken, so it carries the address the row shows beside the icon.
        GtkWidget* action_button(const char* icon_name,
                                 const std::string& address,
                                 const char* tooltip,
                                 const std::string& name,
                                 GCallback on_click) {
            GtkWidget* button = gtk_button_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
            gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
            gtk_widget_set_tooltip_text(button, tooltip);
            set_accessible_name(button, name);
            g_object_set_data_full(G_OBJECT(button), "address", g_strdup(address.c_str()), g_free);
            g_signal_connect(button, "clicked", on_click, nullptr);
            return button;
        }

        GtkWidget* build_address_row(const std::string& key) {
            const bool is_tel = key.rfind("tel:", 0) == 0;

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget* icon = gtk_image_new_from_icon_name(is_tel ? "call-start-symbolic" : "mail-send-symbolic",
                                                           GTK_ICON_SIZE_BUTTON);
            gtk_style_context_add_class(gtk_widget_get_style_context(icon), "muted");
            gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);

            GtkWidget* label = gtk_label_new(display_address(key).c_str());
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_label_set_selectable(GTK_LABEL(label), TRUE);
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(label), 28);
            gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);

            gtk_box_pack_end(
                GTK_BOX(box),
                action_button("edit-copy-symbolic",
                              display_address(key),
                              _("Copy"),
                              // TRANSLATORS: Spoken name of the copy button beside a phone number or email.
                              tether::tr_format(_("Copy {}"), display_address(key)),
                              G_CALLBACK(+[](GtkButton* button, gpointer) {
                                  gtk_clipboard_set_text(
                                      gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), stashed(GTK_WIDGET(button)), -1);
                                  set_status_main(_("Copied to the clipboard."));
                              })),
                FALSE,
                FALSE,
                0);

            GtkWidget* message = action_button("mail-message-new-symbolic",
                                               key,
                                               _("Message"),
                                               // TRANSLATORS: Spoken name of the message button beside a phone
                                               // number or email.
                                               tether::tr_format(_("Message {}"), display_address(key)),
                                               G_CALLBACK(+[](GtkButton* button, gpointer) {
                                                   if (g_contacts.open_thread)
                                                       g_contacts.open_thread(stashed(GTK_WIDGET(button)));
                                               }));
            gtk_box_pack_end(GTK_BOX(box), message, FALSE, FALSE, 0);
            return box;
        }

        // Lazily built the first time a contact is opened.
        void on_expanded(GObject* expander, GParamSpec*, gpointer) {
            if (!gtk_expander_get_expanded(GTK_EXPANDER(expander)))
                return;
            const char* pending = static_cast<const char*>(g_object_get_data(expander, "addresses"));
            if (!pending)
                return;

            GtkWidget* box = gtk_bin_get_child(GTK_BIN(expander));
            std::string keys = pending;
            size_t start = 0;
            while (start <= keys.size()) {
                const size_t end = keys.find('\n', start);
                const std::string key = keys.substr(start, end - start);
                if (!key.empty())
                    gtk_box_pack_start(GTK_BOX(box), build_address_row(key), FALSE, FALSE, 0);
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            g_object_set_data(expander, "addresses", nullptr);
            gtk_widget_show_all(box);
        }

        GtkWidget* build_row(const nlohmann::json& contact) {
            const std::string name = contact.value("name", "");

            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

            GtkWidget* expander = gtk_expander_new(nullptr);
            gtk_container_set_border_width(GTK_CONTAINER(expander), 10);

            GtkWidget* title = gtk_label_new(nullptr);
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(title), 28);
            GtkWidget* identity = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
            gtk_box_pack_start(GTK_BOX(identity), avatar(name), FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(identity), title, TRUE, TRUE, 0);
            gtk_expander_set_label_widget(GTK_EXPANDER(expander), identity);

            GtkWidget* addresses = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            gtk_widget_set_margin_top(addresses, 6);
            gtk_widget_set_margin_start(addresses, 18);

            std::string haystack = name;
            std::string first_address;
            std::string keys;
            if (contact.contains("addresses") && contact["addresses"].is_array()) {
                for (const auto& entry : contact["addresses"]) {
                    if (!entry.is_string())
                        continue;
                    const std::string key = entry.get<std::string>();
                    if (first_address.empty())
                        first_address = display_address(key);
                    haystack += " " + display_address(key);
                    keys += key + "\n";
                }
            }

            set_markup(title, "<b>" + escape_markup(name.empty() ? first_address : name) + "</b>");
            g_object_set_data_full(G_OBJECT(row), "search", g_strdup(fold(haystack).c_str()), g_free);
            g_object_set_data_full(G_OBJECT(expander), "addresses", g_strdup(keys.c_str()), g_free);
            g_signal_connect(expander, "notify::expanded", G_CALLBACK(on_expanded), nullptr);

            gtk_container_add(GTK_CONTAINER(expander), addresses);
            gtk_container_add(GTK_CONTAINER(row), expander);
            return row;
        }

        void show_contacts(const nlohmann::json& event) {
            const std::string payload = event.contains("contacts") ? event["contacts"].dump() : "";
            if (payload == g_contacts.shown)
                return;
            g_contacts.shown = payload;

            clear_list_box(g_contacts.list);
            const bool empty = !event.contains("contacts") || event["contacts"].empty();
            if (!empty) {
                for (const auto& contact : event["contacts"])
                    gtk_list_box_insert(GTK_LIST_BOX(g_contacts.list), build_row(contact), -1);
            }
            gtk_widget_show_all(g_contacts.list);
            gtk_stack_set_visible_child_name(GTK_STACK(g_contacts.stack), empty ? "status" : "list");
        }

    } // namespace

    void contacts_view_set_visible(bool visible) {
        g_contacts.visible = visible;
        if (!visible)
            return;
        request_contacts();
        gtk_widget_grab_focus(g_contacts.search_entry);
    }

    bool contacts_view_handle_event(const nlohmann::json& event) {
        if (event.value("command", "") == "bt_contacts")
            show_contacts(event);
        return false;
    }

    GtkWidget* contacts_view_new(void (*open_thread)(const std::string& thread_key)) {
        g_contacts.open_thread = open_thread;
        g_contacts.stack = gtk_stack_new();

        GtkWidget* status_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_valign(status_box, GTK_ALIGN_CENTER);
        style(status_box, "tether-empty");
        gtk_widget_set_halign(status_box, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(status_box), navigation_icon("contacts", 56), FALSE, FALSE, 0);
        g_contacts.status_label = gtk_label_new(_("No contacts yet. Check the Bluetooth link on the Devices page."));
        gtk_label_set_line_wrap(GTK_LABEL(g_contacts.status_label), TRUE);
        gtk_label_set_justify(GTK_LABEL(g_contacts.status_label), GTK_JUSTIFY_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_contacts.status_label), "muted");
        gtk_box_pack_start(GTK_BOX(status_box), g_contacts.status_label, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_contacts.stack), status_box, "status");

        GtkWidget* list_side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        g_contacts.search_entry = gtk_search_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(g_contacts.search_entry), _("Search contacts"));
        set_accessible_name(g_contacts.search_entry, _("Search contacts"));
        gtk_widget_set_margin_top(g_contacts.search_entry, 24);
        gtk_widget_set_margin_start(g_contacts.search_entry, 24);
        gtk_widget_set_margin_end(g_contacts.search_entry, 24);
        g_signal_connect(g_contacts.search_entry, "search-changed", G_CALLBACK(on_search_changed), nullptr);
        g_signal_connect(g_contacts.search_entry,
                         "stop-search",
                         G_CALLBACK(+[](GtkSearchEntry* entry, gpointer) { gtk_entry_set_text(GTK_ENTRY(entry), ""); }),
                         nullptr);
        gtk_box_pack_start(GTK_BOX(list_side), g_contacts.search_entry, FALSE, FALSE, 0);

        GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        g_contacts.list = gtk_list_box_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(g_contacts.list), "tether-feed");
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_contacts.list), GTK_SELECTION_NONE);
        gtk_list_box_set_filter_func(GTK_LIST_BOX(g_contacts.list), contact_visible, nullptr, nullptr);
        gtk_container_add(GTK_CONTAINER(scroll), g_contacts.list);
        gtk_box_pack_start(GTK_BOX(list_side), scroll, TRUE, TRUE, 0);
        gtk_stack_add_named(GTK_STACK(g_contacts.stack), list_side, "list");

        gtk_stack_set_visible_child_name(GTK_STACK(g_contacts.stack), "status");
        return g_contacts.stack;
    }

} // namespace tether::ui
