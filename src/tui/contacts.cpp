#include "app.hpp"

#include <map>
#include <tether/i18n.hpp>
#include <ui_text.hpp>

namespace tether::tui {

    namespace contacts {

        namespace {

            constexpr int CONTACT_LIMIT = 5000;

            std::vector<ui::ContactEntry> g_entries;
            std::map<std::string, std::string> g_names;
            std::string g_shown;

        } // namespace

        void request() { app::send({{"command", "bt_list_contacts"}, {"limit", CONTACT_LIMIT}}); }

        void update(const json& event) {
            if (event.value("command", "") != "bt_contacts")
                return;
            const std::string payload = event.contains("contacts") ? event["contacts"].dump() : "";
            if (payload == g_shown)
                return;
            g_shown = payload;
            static const json none;
            g_entries = ui::contact_entries(event.contains("contacts") ? event["contacts"] : none, g_names);
        }

        std::string name_for(const std::string& thread_key) {
            const auto known = g_names.find(thread_key);
            return known == g_names.end() ? std::string() : known->second;
        }

        // Substring over the name and the number alike, so a surname or a
        // number typed without its formatting both match.
        std::vector<Candidate> complete(const std::string& text, bool tel_only) {
            std::vector<Candidate> out;
            if (text.empty())
                return out;
            const std::string needle = ui::fold(text);
            for (const auto& entry : g_entries) {
                if (tel_only && !entry.is_tel)
                    continue;
                if (entry.search.find(needle) != std::string::npos)
                    out.push_back({entry.display, entry.address});
                if (out.size() >= 50)
                    break;
            }
            return out;
        }

    } // namespace contacts

    namespace {

        class ContactsPage : public Page {
        public:
            std::string title() const override { return _("Contacts"); }

            void set_visible(bool visible) override {
                if (visible)
                    contacts::request();
            }

            std::vector<Row> list() override {
                std::vector<Row> rows;
                for (size_t i = 0; i < contacts_.size(); ++i) {
                    const json& contact = contacts_[i];
                    const auto addresses = addresses_of(contact);
                    std::string haystack = contact.value("name", "");
                    for (const auto& key : addresses)
                        haystack += " " + ui::display_address(key);
                    Row row;
                    row.text = display_name(contact, addresses);
                    if (contact.value("favorite", false))
                        row.meta = "★";
                    row.id = std::to_string(i);
                    row.selectable = true;
                    row.search = ui::fold(haystack);
                    rows.push_back(row);
                }
                return rows;
            }

            void cursor_moved(const std::string& id) override { selected_ = id; }

            std::vector<Row> detail() override {
                std::vector<Row> rows;
                const size_t index = selected_.empty() ? contacts_.size() : std::stoul(selected_);
                if (index >= contacts_.size()) {
                    rows.push_back(
                        text_row(_("No contacts yet. Check the Bluetooth link on the Devices page."), Tone::Muted));
                    return rows;
                }
                const json& contact = contacts_[index];
                const auto addresses = addresses_of(contact);
                Row name = heading(display_name(contact, addresses));
                if (contact.value("favorite", false))
                    name.meta = std::string("★ ") + _("Favorite");
                rows.push_back(name);
                rows.push_back(blank_row());

                for (const auto& key : addresses) {
                    const bool is_tel = key.rfind("tel:", 0) == 0;
                    Row row;
                    row.text = std::string(is_tel ? "☎  " : "✉  ") + ui::display_address(key);
                    row.id = key;
                    row.selectable = true;
                    row.yank = ui::display_address(key);
                    row.activate = [key] {
                        app::show_page("messages");
                        messages_open_thread(key);
                    };
                    rows.push_back(row);
                }
                return rows;
            }

            bool action(const std::string& action) override {
                if (action == "refresh") {
                    contacts::request();
                    return true;
                }
                return false;
            }

            bool handle_event(const json& event) override {
                if (event.value("command", "") == "bt_contacts")
                    contacts_ =
                        event.contains("contacts") && event["contacts"].is_array() ? event["contacts"] : json::array();
                return false;
            }

        private:
            static std::vector<std::string> addresses_of(const json& contact) {
                std::vector<std::string> out;
                if (contact.contains("addresses") && contact["addresses"].is_array())
                    for (const auto& entry : contact["addresses"])
                        if (entry.is_string())
                            out.push_back(entry.get<std::string>());
                return out;
            }

            static std::string display_name(const json& contact, const std::vector<std::string>& addresses) {
                const std::string name = contact.value("name", "");
                if (!name.empty() || addresses.empty())
                    return name;
                return ui::display_address(addresses.front());
            }

            json contacts_ = json::array();
            std::string selected_;
        };

    } // namespace

    std::unique_ptr<Page> make_contacts_page() { return std::make_unique<ContactsPage>(); }

} // namespace tether::tui
