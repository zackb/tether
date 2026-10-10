#include "app.hpp"

#include <tether/i18n.hpp>

namespace tether::tui {

    namespace {

        // One daemon-backed setting. Retention is the only one with more than two values.
        struct Setting {
            const char* id;
            const char* group;
            const char* title;
            const char* subtitle;
            const char* command; // sent with {"enabled": bool}
            const char* status_key;
            bool fallback;
        };

        const Setting SETTINGS[] = {
            {"ancs",
             N_("iPhone notifications"),
             N_("Mirror iPhone notifications"),
             N_("Disabled when pairing cannot support LE."),
             "bt_set_ancs",
             "ancs_enabled",
             true},
            {"content",
             N_("iPhone notifications"),
             N_("Include the notification text"),
             N_("Title and message content are shown, not just which app sent it."),
             "bt_set_ancs_content",
             "ancs_content_enabled",
             true},
            {"popups",
             N_("Desktop popups"),
             N_("Show desktop popups"),
             N_("Off silences iPhone alerts, new messages and arriving files."),
             "set_desktop_popups",
             "desktop_popups_enabled",
             true},
            {"previews",
             N_("Desktop popups"),
             N_("Show message previews"),
             N_("Off shows only the sender."),
             "set_popup_previews",
             "popup_previews_enabled",
             true},
            {"lock",
             N_("Security"),
             N_("Lock the session when the iPhone goes out of range"),
             N_("Locks about half a minute after the link times out. Switching Bluetooth off on the iPhone does not "
                "lock, and neither does suspending this machine."),
             "bt_set_lock_on_away",
             "lock_on_away",
             false},
            {"retention",
             N_("Messages"),
             N_("Keep message history"),
             N_("Encrypted uses a key from your desktop keyring. Do not keep deletes what is stored."),
             "bt_set_retention",
             "retention",
             false},
            {"calls",
             N_("Experimental"),
             N_("Call control over Bluetooth"),
             N_("Answer and place calls from the Calls tab."),
             "bt_set_calls",
             "calls_enabled",
             false},
        };

        struct Retention {
            const char* wire;
            const char* label;
        };
        const Retention RETENTIONS[] = {
            {"encrypted", N_("Encrypted")},
            {"plaintext", N_("Unencrypted")},
            {"none", N_("Do not keep")},
        };

        class SettingsPage : public Page {
        public:
            std::string title() const override { return _("Settings"); }

            void set_visible(bool visible) override {
                // The page can open before any broadcast has arrived.
                if (visible)
                    app::send({{"command", "bt_status"}});
            }

            std::vector<Row> list() override {
                std::vector<Row> rows;
                const char* group = nullptr;
                for (const Setting& setting : SETTINGS) {
                    if (!group || std::string(group) != setting.group) {
                        group = setting.group;
                        if (!rows.empty())
                            rows.push_back(blank_row());
                        rows.push_back(heading(_(group)));
                    }
                    Row row;
                    row.id = setting.id;
                    row.selectable = true;
                    row.tone = usable(setting) ? Tone::Normal : Tone::Muted;
                    if (std::string(setting.id) == "retention") {
                        row.text = _(setting.title);
                        row.meta = retention_label(status_.value("retention", "encrypted"));
                    } else {
                        row.text = std::string(value(setting) ? "[x] " : "[ ] ") + _(setting.title);
                    }
                    rows.push_back(row);
                }
                return rows;
            }

            void cursor_moved(const std::string& id) override { selected_ = id; }

            std::vector<Row> detail() override {
                std::vector<Row> rows;
                const bool available = status_.value("available", false);
                const bool bonded = !status_.value("device_address", std::string()).empty();
                if (!available || !bonded) {
                    rows.push_back(text_row(_("No iPhone is paired over Bluetooth, so the Bluetooth settings below "
                                              "cannot be changed yet."),
                                            Tone::Bad));
                    rows.push_back(blank_row());
                }
                const Setting* setting = find(selected_);
                if (!setting)
                    return rows;
                rows.push_back(heading(_(setting->title)));
                rows.push_back(text_row(_(setting->subtitle), Tone::Muted));
                rows.push_back(blank_row());
                if (std::string(setting->id) == "retention") {
                    const std::string current = status_.value("retention", "encrypted");
                    for (const auto& option : RETENTIONS) {
                        Row row;
                        row.text = std::string(current == option.wire ? "(•) " : "( ) ") + _(option.label);
                        row.id = option.wire;
                        row.selectable = true;
                        row.tone = usable(*setting) ? Tone::Normal : Tone::Muted;
                        if (usable(*setting))
                            row.activate = [wire = option.wire] {
                                app::send({{"command", "bt_set_retention"}, {"retention", wire}});
                            };
                        rows.push_back(row);
                    }
                    if (current == "encrypted" && !status_.value("retention_ready", true)) {
                        rows.push_back(blank_row());
                        rows.push_back(
                            text_row(_("Paused: no key from the desktop keyring. Unlock it, or choose Unencrypted."),
                                     Tone::Bad));
                    }
                }
                return rows;
            }

            // Enter or Space on a list row flips it; retention opens its choices.
            void open(const std::string& id) override {
                const Setting* setting = find(id);
                if (!setting || std::string(setting->id) == "retention")
                    return;
                if (!usable(*setting)) {
                    app::status(_("No iPhone is paired over Bluetooth, so the Bluetooth settings below cannot be "
                                  "changed yet."));
                    return;
                }
                app::send({{"command", setting->command}, {"enabled", !value(*setting)}});
            }

            bool command(const std::string& name, const std::string& args) override {
                if (name != "set")
                    return false;
                // ":set popups", ":set nopopups", ":set retention=none"
                const size_t eq = args.find('=');
                const std::string key = args.substr(0, eq);
                if (eq != std::string::npos && key == "retention") {
                    app::send({{"command", "bt_set_retention"}, {"retention", args.substr(eq + 1)}});
                    return true;
                }
                const bool off = key.rfind("no", 0) == 0;
                const Setting* setting = find(off ? key.substr(2) : key);
                if (!setting || std::string(setting->id) == "retention") {
                    std::string names;
                    for (const Setting& s : SETTINGS)
                        names += std::string(names.empty() ? "" : ", ") + s.id;
                    app::status(tr_format(_("Settings: {}"), names));
                    return true;
                }
                app::send({{"command", setting->command}, {"enabled", !off}});
                return true;
            }

            bool handle_event(const json& event) override {
                if (event.value("command", "") == "bt_status")
                    status_ = event;
                return false;
            }

        private:
            static const Setting* find(const std::string& id) {
                for (const Setting& setting : SETTINGS)
                    if (id == setting.id)
                        return &setting;
                return nullptr;
            }

            bool value(const Setting& setting) const { return status_.value(setting.status_key, setting.fallback); }

            // Which rows can change depends on the bond, as in the GTK settings.
            bool usable(const Setting& setting) const {
                const bool available = status_.value("available", false);
                const bool bonded = !status_.value("device_address", std::string()).empty();
                const bool bt_on = status_.value("enabled", true);
                const std::string id = setting.id;
                if (id == "ancs" || id == "calls")
                    return available && bonded && bt_on;
                if (id == "content")
                    return available && bonded && bt_on && status_.value("ancs_enabled", true);
                if (id == "retention")
                    return available && bonded;
                // Popups cover Wi-Fi file transfers too, so they never depend on a bond.
                if (id == "previews")
                    return status_.value("desktop_popups_enabled", true);
                return true;
            }

            static std::string retention_label(const std::string& wire) {
                for (const auto& option : RETENTIONS)
                    if (wire == option.wire)
                        return _(option.label);
                return wire;
            }

            json status_ = json::object();
            std::string selected_;
        };

    } // namespace

    std::unique_ptr<Page> make_settings_page() { return std::make_unique<SettingsPage>(); }

} // namespace tether::tui
