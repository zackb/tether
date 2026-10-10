#include "contact_completion.hpp"

#include "daemon_client.hpp"
#include "ui_util.hpp"

#include <cstring>
#include <map>

namespace tether::ui {

    namespace {

        enum ContactColumn { COL_DISPLAY, COL_ADDRESS, COL_SEARCH, COL_IS_TEL, COL_COUNT };

        constexpr int CONTACT_COMPLETION_LIMIT = 5000;

        // One model for every completion
        GtkListStore* g_model = nullptr;
        std::map<std::string, std::string> g_names;
        std::string g_shown;

        GtkListStore* model() {
            if (!g_model)
                g_model = gtk_list_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
            return g_model;
        }

        ContactKind completion_kind(GtkEntryCompletion* completion) {
            return g_object_get_data(G_OBJECT(completion), "tel-only") ? ContactKind::Tel : ContactKind::Any;
        }

        // Substring, over the name and the number alike. The default match is a
        // prefix test on the display column, which would miss both a surname and
        // any number typed without its formatting.
        gboolean completion_match(GtkEntryCompletion* completion, const gchar* key, GtkTreeIter* iter, gpointer) {
            if (!key || !*key)
                return FALSE;
            GtkTreeModel* tree = gtk_entry_completion_get_model(completion);
            gchar* haystack = nullptr;
            gboolean is_tel = FALSE;
            gtk_tree_model_get(tree, iter, COL_SEARCH, &haystack, COL_IS_TEL, &is_tel, -1);
            const gboolean hit = haystack && std::strstr(haystack, key) != nullptr &&
                                 (is_tel || completion_kind(completion) == ContactKind::Any);
            g_free(haystack);
            return hit;
        }

        // The entry always ends up with a bare address, not a display name.
        gboolean on_match_selected(GtkEntryCompletion* completion, GtkTreeModel* tree, GtkTreeIter* iter, gpointer) {
            GtkWidget* entry = GTK_WIDGET(gtk_entry_completion_get_entry(completion));
            gchar* address = nullptr;
            gtk_tree_model_get(tree, iter, COL_ADDRESS, &address, -1);
            if (address)
                gtk_entry_set_text(GTK_ENTRY(entry), address);
            g_free(address);
            gtk_editable_set_position(GTK_EDITABLE(entry), -1);
            return TRUE;
        }

    } // namespace

    void attach_contact_completion(GtkWidget* entry, ContactKind kind) {
        GtkEntryCompletion* completion = gtk_entry_completion_new();
        gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(model()));
        gtk_entry_completion_set_text_column(completion, COL_DISPLAY);
        gtk_entry_completion_set_match_func(completion, completion_match, nullptr, nullptr);
        gtk_entry_completion_set_minimum_key_length(completion, 1);
        gtk_entry_completion_set_inline_completion(completion, FALSE);
        if (kind == ContactKind::Tel)
            g_object_set_data(G_OBJECT(completion), "tel-only", GINT_TO_POINTER(1));
        g_signal_connect(completion, "match-selected", G_CALLBACK(on_match_selected), nullptr);
        gtk_entry_set_completion(GTK_ENTRY(entry), completion);
        g_object_unref(completion);
    }

    void contact_completion_request() {
        nlohmann::json j;
        j["command"] = "bt_list_contacts";
        j["limit"] = CONTACT_COMPLETION_LIMIT;
        daemon_send(j);
    }

    void contact_completion_update(const nlohmann::json& event) {
        if (event.value("command", "") != "bt_contacts")
            return;

        const std::string payload = event.contains("contacts") ? event["contacts"].dump() : "";
        if (payload == g_shown)
            return;
        g_shown = payload;

        gtk_list_store_clear(model());
        static const nlohmann::json none;
        for (const ContactEntry& entry : contact_entries(event.contains("contacts") ? event["contacts"] : none, g_names)) {
            GtkTreeIter iter;
            gtk_list_store_append(model(), &iter);
            gtk_list_store_set(model(),
                               &iter,
                               COL_DISPLAY,
                               entry.display.c_str(),
                               COL_ADDRESS,
                               entry.address.c_str(),
                               COL_SEARCH,
                               entry.search.c_str(),
                               COL_IS_TEL,
                               entry.is_tel,
                               -1);
        }
    }

    std::string contact_name_for(const std::string& thread_key) {
        const auto known = g_names.find(thread_key);
        return known == g_names.end() ? std::string() : known->second;
    }

} // namespace tether::ui
