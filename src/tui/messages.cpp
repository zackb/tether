#include "app.hpp"

#include <ctime>
#include <map>
#include <message_format.hpp>
#include <set>
#include <tether/bluetooth/bmessage.hpp>
#include <tether/i18n.hpp>
#include <ui_text.hpp>

namespace tether::tui {

    namespace {

        constexpr int SEND_TIMEOUT_SECONDS = 60;
        constexpr int64_t GROUP_WINDOW_SECONDS = 300;

        // One number spelled several ways
        bool same_thread(const std::string& a, const std::string& b) {
            if (a.empty() || b.empty())
                return a == b;
            return bluetooth::thread_bucket(a) == bluetooth::thread_bucket(b);
        }

        class MessagesPage;
        MessagesPage* g_page = nullptr;

        class MessagesPage : public Page {
        public:
            MessagesPage() { g_page = this; }

            std::string title() const override { return _("Messages"); }

            void set_visible(bool visible) override {
                visible_ = visible;
                if (!visible)
                    return;
                request_threads();
                request_messages(selected_thread_);
            }

            void handle_disconnect() override {
                clear_sending();
                map_open_ = false;
                marked_read_.clear();
                banner_ = _("The Tether daemon is not running.");
                offer_permissions_ = false;
            }

            std::vector<Row> list() override {
                std::vector<Row> rows;
                const int64_t now = std::time(nullptr);
                for (const auto& thread : threads_) {
                    const std::string key = thread.value("thread", "");
                    const std::string address = thread.value("address", "");
                    const std::string name = thread.value("name", address);
                    const std::string preview = thread.value("preview", "");
                    const int unread = thread.value("unread", 0);
                    Row row;
                    row.text = name + "\n  " + preview;
                    row.meta = ui::format_thread_time(thread.value("timestamp", static_cast<int64_t>(0)), now);
                    if (unread > 0)
                        row.meta += "  ●" + std::to_string(unread);
                    row.bold = unread > 0;
                    row.tone = same_thread(key, selected_thread_) && !composing_ ? Tone::Accent : Tone::Normal;
                    row.id = key;
                    row.selectable = true;
                    row.search = ui::fold(name + " " + address + " " + preview);
                    rows.push_back(row);
                }
                return rows;
            }

            void open(const std::string& key) override {
                const json* thread = find_thread(key);
                if (!thread)
                    return;
                leave_compose();
                apply_selection(*thread);
                request_messages(selected_thread_);
                app::pin_detail();
            }

            std::vector<Row> detail() override {
                update_composer();
                std::vector<Row> rows;
                if (!banner_.empty()) {
                    rows.push_back(text_row(banner_, Tone::Bad));
                    if (offer_permissions_)
                        rows.push_back(text_row(tr_format("{}: :perms", _("Show iPhone Permissions")), Tone::Muted));
                    rows.push_back(blank_row());
                }
                if (selected_thread_.empty() && !composing_) {
                    const bool bluetooth_needed = threads_known_ && threads_.empty() && !map_open_;
                    rows.push_back(text_row(bluetooth_needed ? _("Bluetooth connection needed to sync messages.")
                                                             : _("Select a conversation"),
                                            Tone::Muted));
                    return rows;
                }

                std::string header = selected_name_.empty() ? selected_thread_ : selected_name_;
                if (composing_) {
                    bluetooth::Recipient recipient;
                    std::string err;
                    header = _("New Message");
                    if (!selected_name_.empty())
                        header = selected_name_;
                    else if (bluetooth::recipient_from_thread_key(selected_thread_, recipient, err))
                        header = recipient.address;
                }
                rows.push_back(heading(header));

                const int64_t now = std::time(nullptr);
                int64_t last_stamp = 0;
                bool last_outgoing = false;
                for (size_t i = 0; i < messages_.size(); ++i) {
                    const auto& message = messages_[i];
                    const int64_t stamp = message.value("timestamp", static_cast<int64_t>(0));
                    const bool outgoing = message.value("outgoing", false);
                    const bool first = i == 0;
                    if (stamp > 0 && (first || !ui::same_local_day(last_stamp, stamp))) {
                        rows.push_back(blank_row());
                        Row day = text_row(ui::format_day_heading(stamp, now), Tone::Muted);
                        day.align = Align::Center;
                        rows.push_back(day);
                    }
                    const bool grouped = !first && outgoing == last_outgoing &&
                                         stamp - last_stamp < GROUP_WINDOW_SECONDS &&
                                         ui::same_local_day(last_stamp, stamp);
                    const Align side = outgoing ? Align::Right : Align::Left;
                    if (!grouped) {
                        Row time = text_row(ui::format_clock(stamp), Tone::Muted);
                        time.align = side;
                        rows.push_back(time);
                    }
                    Row body = text_row(message.value("body", ""), outgoing ? Tone::Outgoing : Tone::Normal);
                    body.align = side;
                    rows.push_back(body);
                    last_stamp = stamp;
                    last_outgoing = outgoing;
                }
                return rows;
            }

            Composer* composer() override { return &composer_; }

            void submit() override {
                const std::string body = composer_.edit.text();
                if (body.empty() || selected_thread_.empty() || !composer_.enabled)
                    return;
                send_error_.clear();
                if (!app::send({{"command", "bt_send_message"}, {"thread", selected_thread_}, {"body", body}})) {
                    send_error_ = _("Could not reach the Tether daemon; the message was not sent.");
                    app::status(send_error_);
                    return;
                }
                // locked until the phone answers
                sending_ = true;
                send_watchdog_ = app::after(SEND_TIMEOUT_SECONDS, [this] {
                    send_watchdog_ = 0;
                    if (sending_) {
                        sending_ = false;
                        app::status(_("No answer about that message; it may still have been sent."));
                    }
                });
                app::status(_("Sending…"));
            }

            bool action(const std::string& action) override {
                if (action == "open") {
                    new_message();
                    return true;
                }
                if (action == "refresh") {
                    request_threads();
                    request_messages(selected_thread_);
                    return true;
                }
                return false;
            }

            bool command(const std::string& name, const std::string& args) override {
                if (name == "new" || (name == "msg" && args.empty())) {
                    app::show_page("messages");
                    new_message();
                    return true;
                }
                if (name == "msg") {
                    app::show_page("messages");
                    address(args);
                    return true;
                }
                if (name == "perms" || name == "solicit") {
                    if (!app::send({{"command", "bt_solicit"}}))
                        app::status(_("Could not reach the Tether daemon."));
                    else
                        app::status(_("Asking the iPhone to show its Bluetooth permissions…"));
                    return true;
                }
                return false;
            }

            bool handle_event(const json& event) override {
                const std::string command = event.value("command", "");
                if (command == "bt_threads") {
                    int unread = 0;
                    if (event.contains("threads") && event["threads"].is_array())
                        for (const auto& thread : event["threads"])
                            unread += thread.value("unread", 0);
                    app::set_unread(unread);
                    show_threads(event);
                    return true;
                }
                if (command == "bt_messages") {
                    show_messages(event);
                    return true;
                }
                if (command == "bt_contacts")
                    return true;
                if (command == "bt_connection_changed") {
                    update_connection(event);
                    return false;
                }
                if (command == "bt_message") {
                    // Re-read even while hidden, for the unread count.
                    request_threads();
                    if (visible_ && same_thread(event.value("thread", ""), selected_thread_))
                        request_messages(selected_thread_);
                    return true;
                }
                if (command == "bt_send_result") {
                    on_send_result(event);
                    return true;
                }
                if (command == "bt_message_read") {
                    if (visible_)
                        request_threads();
                    return true;
                }
                if (command == "bt_solicit_result") {
                    app::status(event.value("message", ""));
                    return true;
                }
                return false;
            }

            void open_thread(const std::string& key) {
                if (key.empty())
                    return;
                leave_compose();
                pending_new_thread_ = key;
                switch_thread(key);
                selected_name_.clear();
                selected_repliable_ = false;
                selected_block_reason_.clear();
                focus_composer_ = true;
                app::select_row(key);
                app::pin_detail();
                request_threads();
                request_messages(key);
            }

            void new_message() {
                contacts::request();
                app::prompt(
                    std::string(_("To:")) + " ",
                    [this](const std::string& text) { address(text); },
                    [](const std::string& text) { return contacts::complete(text, false); });
            }

        private:
            void request_threads() { app::send({{"command", "bt_list_threads"}}); }

            void request_messages(const std::string& key) {
                if (!key.empty())
                    app::send({{"command", "bt_list_messages"}, {"thread", key}});
            }

            const json* find_thread(const std::string& key) const {
                for (const auto& thread : threads_)
                    if (same_thread(thread.value("thread", ""), key))
                        return &thread;
                return nullptr;
            }

            // Starts a message to whatever was typed into "To:".
            void address(const std::string& text) {
                bluetooth::Recipient recipient;
                std::string err;
                if (text.empty())
                    return;
                if (!bluetooth::recipient_from_input(text, recipient, err)) {
                    app::status(err);
                    return;
                }
                enter_compose(bluetooth::thread_key_for(recipient));
                app::insert_mode();
            }

            void enter_compose(const std::string& key) {
                stash_draft();
                composing_ = true;
                selected_thread_ = key;
                selected_repliable_ = !key.empty();
                selected_block_reason_.clear();
                selected_name_ = contacts::name_for(key);
                messages_ = json::array();
                send_error_.clear();
                const auto draft = drafts_.find(key);
                composer_.edit.set(draft == drafts_.end() ? "" : draft->second);
                // Whatever has already been said to this person belongs above the composer.
                request_messages(key);
                app::pin_detail();
                app::focus_detail();
            }

            void leave_compose() { composing_ = false; }

            void stash_draft() {
                if (selected_thread_.empty())
                    return;
                const std::string text = composer_.edit.text();
                if (text.empty())
                    drafts_.erase(selected_thread_);
                else
                    drafts_[selected_thread_] = text;
            }

            void switch_thread(const std::string& key) {
                if (key == selected_thread_)
                    return;
                stash_draft();
                selected_thread_ = key;
                messages_ = json::array();
                send_error_.clear();
                const auto draft = drafts_.find(key);
                composer_.edit.set(draft == drafts_.end() ? "" : draft->second);
            }

            // The daemon owns reply eligibility; the UI must not re-derive it.
            void apply_selection(const json& thread) {
                switch_thread(thread.value("thread", ""));
                const std::string address = thread.value("address", "");
                selected_name_ = thread.value("name", address);
                selected_repliable_ = thread.value("repliable", true);
                selected_block_reason_ = thread.value("reply_reason", "");
            }

            void clear_selection() {
                switch_thread("");
                selected_name_.clear();
                selected_repliable_ = false;
                selected_block_reason_.clear();
            }

            void show_threads(const json& event) {
                if (!event.contains("threads") || !event["threads"].is_array())
                    return;
                threads_ = event["threads"];
                threads_known_ = true;

                if (const json* thread = find_thread(selected_thread_); thread && !selected_thread_.empty()) {
                    pending_new_thread_.clear();
                    if (!composing_)
                        apply_selection(*thread);
                } else if (!selected_thread_.empty() && !composing_) {
                    bluetooth::Recipient recipient;
                    std::string err;
                    const bool wanted = same_thread(pending_new_thread_, selected_thread_) &&
                                        bluetooth::recipient_from_thread_key(selected_thread_, recipient, err);
                    pending_new_thread_.clear();
                    if (wanted)
                        enter_compose(bluetooth::thread_key_for(recipient));
                    else
                        clear_selection();
                }

                if (focus_composer_) {
                    focus_composer_ = false;
                    app::focus_detail();
                    update_composer();
                    app::insert_mode();
                }
            }

            // Opening a conversation marks it read here and on the phone.
            void mark_read(const json& messages) {
                std::vector<std::string> pending;
                for (const auto& message : messages) {
                    if (message.value("read", true) || message.value("outgoing", false))
                        continue;
                    const std::string handle = message.value("handle", "");
                    if (!handle.empty() && !marked_read_.count(handle))
                        pending.push_back(handle);
                }
                if (pending.empty())
                    return;
                if (app::send({{"command", "bt_mark_read"}, {"handles", pending}, {"read", true}}))
                    marked_read_.insert(pending.begin(), pending.end());
            }

            void show_messages(const json& event) {
                if (!same_thread(event.value("thread", ""), selected_thread_))
                    return;
                const json messages =
                    event.contains("messages") && event["messages"].is_array() ? event["messages"] : json::array();
                mark_read(messages);
                messages_ = messages;
            }

            void update_connection(const json& event) {
                map_open_ = event.value("map_open", false);
                if (map_open_) {
                    banner_.clear();
                    offer_permissions_ = false;
                    if (visible_)
                        request_threads();
                    return;
                }
                // The daemon's reason names the next step, so it is shown verbatim.
                std::string reason = event.value("profile_reason", "");
                if (reason.empty())
                    reason = event.value("link_reason", "");
                if (reason.empty())
                    reason = _("Messages are not connected.");
                banner_ = reason;
                const std::string map_error = event.value("map_error", "none");
                offer_permissions_ = map_error == "forbidden" || map_error == "no_record";
            }

            // Replying needs an open conversation, a live MAP session, and a
            // thread the daemon says can take a reply.
            void update_composer() {
                const bool can_send = map_open_ && !selected_thread_.empty() && selected_repliable_;
                composer_.enabled = can_send && !sending_;

                const char* reason = nullptr;
                if (sending_)
                    reason = _("Sending…");
                else if (!map_open_)
                    reason = _("Messages are not connected.");
                else if (selected_thread_.empty())
                    reason = composing_ ? (selected_block_reason_.empty() ? _("Enter a recipient.")
                                                                          : selected_block_reason_.c_str())
                                        : _("Select a conversation first.");
                else if (!selected_block_reason_.empty())
                    reason = selected_block_reason_.c_str();
                else if (!can_send)
                    reason = _("Replying to this conversation is not available.");

                if (!send_error_.empty()) {
                    composer_.notice = send_error_;
                    composer_.notice_is_error = true;
                } else {
                    composer_.notice = reason && (!composer_.enabled || sending_) ? reason : "";
                    composer_.notice_is_error = false;
                }
            }

            void clear_sending() {
                app::cancel(send_watchdog_);
                sending_ = false;
            }

            void on_send_result(const json& event) {
                clear_sending();
                if (event.value("success", false)) {
                    // Cleared only once the phone accepted it, so a failure leaves
                    // the text where the user can retry.
                    drafts_.erase(selected_thread_);
                    composer_.edit.clear();
                    send_error_.clear();
                    app::pin_detail();
                    // The conversation now exists, so the next thread list selects it.
                    leave_compose();
                    app::status(_("Sent"));
                    return;
                }
                send_error_ = event.value("message", _("The message was not sent."));
                app::status(send_error_);
            }

            json threads_ = json::array();
            json messages_ = json::array();
            bool threads_known_ = false;
            bool visible_ = false;
            bool map_open_ = false;

            std::string selected_thread_;
            std::string selected_name_;
            bool selected_repliable_ = false;
            std::string selected_block_reason_;
            bool composing_ = false;
            std::string pending_new_thread_;
            bool focus_composer_ = false;

            std::string banner_;
            bool offer_permissions_ = false;

            Composer composer_;
            std::map<std::string, std::string> drafts_;
            std::set<std::string> marked_read_;
            bool sending_ = false;
            int send_watchdog_ = 0;
            std::string send_error_;
        };

    } // namespace

    std::unique_ptr<Page> make_messages_page() { return std::make_unique<MessagesPage>(); }

    void messages_open_thread(const std::string& thread_key) {
        if (g_page)
            g_page->open_thread(thread_key);
    }

    void messages_new_message() {
        if (g_page)
            g_page->new_message();
    }

} // namespace tether::tui
