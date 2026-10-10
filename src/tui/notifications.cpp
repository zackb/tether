#include "app.hpp"

#include <ctime>
#include <message_format.hpp>
#include <tether/i18n.hpp>
#include <ui_text.hpp>

namespace tether::tui {

    namespace {

        class NotificationsPage : public Page {
        public:
            std::string title() const override { return _("Notifications"); }

            void set_visible(bool visible) override {
                visible_ = visible;
                if (visible)
                    request();
            }

            std::vector<Row> list() override {
                std::vector<Row> rows;
                const int64_t now = std::time(nullptr);
                for (const auto& n : notifications_) {
                    const std::string app_name = n.value("app_name", n.value("app_id", ""));
                    Row row;
                    row.text = app_name + "\n  " + primary(n);
                    row.meta = ui::format_thread_time(n.value("timestamp", static_cast<int64_t>(0)), now);
                    row.id = std::to_string(n.value("uid", static_cast<uint32_t>(0)));
                    row.selectable = true;
                    row.search = ui::fold(app_name + " " + primary(n));
                    rows.push_back(row);
                }
                return rows;
            }

            void cursor_moved(const std::string& id) override { selected_ = id; }

            std::vector<Row> detail() override {
                std::vector<Row> rows;
                const json* n = find(selected_);
                if (!n) {
                    rows.push_back(text_row(status_, Tone::Muted));
                    return rows;
                }
                const int64_t now = std::time(nullptr);
                Row header = heading(n->value("app_name", n->value("app_id", "")));
                header.meta = ui::format_thread_time(n->value("timestamp", static_cast<int64_t>(0)), now);
                rows.push_back(header);
                rows.push_back(blank_row());
                Row main = text_row(primary(*n));
                main.bold = true;
                main.yank = main.text;
                rows.push_back(main);
                if (const std::string more = secondary(*n); !more.empty())
                    rows.push_back(text_row(more));
                if (n->value("negative_action", false)) {
                    rows.push_back(blank_row());
                    const uint32_t uid = n->value("uid", static_cast<uint32_t>(0));
                    rows.push_back(action_row(_("Dismiss on the iPhone"), [this, uid] { dismiss(uid); }));
                }
                return rows;
            }

            bool action(const std::string& action) override {
                if (action == "delete") {
                    if (const json* n = find(selected_); n && n->value("negative_action", false))
                        dismiss(n->value("uid", static_cast<uint32_t>(0)));
                    return true;
                }
                if (action == "refresh") {
                    request();
                    return true;
                }
                return false;
            }

            bool handle_event(const json& event) override {
                const std::string command = event.value("command", "");
                if (command == "bt_notifications") {
                    notifications_ = event.contains("notifications") && event["notifications"].is_array()
                                         ? event["notifications"]
                                         : json::array();
                    return true;
                }
                if (command == "bt_notification_action_result") {
                    if (!event.value("success", false))
                        app::status(_("The iPhone would not take the dismissal."));
                    return true;
                }
                if (command == "bt_notification" || command == "bt_notification_removed") {
                    if (visible_)
                        request();
                    return true;
                }
                if (command == "bt_connection_changed") {
                    const bool ready = event.value("ancs_ready", false);
                    // The daemon's reason names the next step, so it is shown verbatim.
                    const std::string reason = event.value("ancs_reason", "");
                    status_ = ready ? _("No notifications yet.")
                                    : (reason.empty() ? _("Notification mirroring is unavailable.") : reason);
                    return false;
                }
                return false;
            }

        private:
            void request() { app::send({{"command", "bt_list_notifications"}}); }

            void dismiss(uint32_t uid) {
                app::send({{"command", "bt_notification_action"}, {"uid", uid}, {"action", "negative"}});
            }

            const json* find(const std::string& id) const {
                for (const auto& n : notifications_)
                    if (std::to_string(n.value("uid", static_cast<uint32_t>(0))) == id)
                        return &n;
                return nullptr;
            }

            // With content mirroring off only the app is known, so the row says so.
            static std::string primary(const json& n) {
                const std::string title = n.value("title", "");
                const std::string body = n.value("body", "");
                return !title.empty() ? title : (!body.empty() ? body : _("New notification"));
            }

            // Whatever is left after the primary line claimed one of them.
            static std::string secondary(const json& n) {
                std::string out = n.value("subtitle", "");
                const std::string body = n.value("body", "");
                if (!body.empty() && body != primary(n))
                    out += (out.empty() ? "" : "\n") + body;
                return out;
            }

            json notifications_ = json::array();
            std::string selected_;
            std::string status_ = _("Waiting for the iPhone.");
            bool visible_ = false;
        };

    } // namespace

    std::unique_ptr<Page> make_notifications_page() { return std::make_unique<NotificationsPage>(); }

} // namespace tether::tui
