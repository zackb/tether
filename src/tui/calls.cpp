#include "app.hpp"

#include <tether/i18n.hpp>
#include <ui_text.hpp>

namespace tether::tui {

    namespace {

        const std::string LIVE_PREFIX = "live:";
        const std::string HISTORY_PREFIX = "history:";

        class CallsPage;
        CallsPage* g_page = nullptr;

        class CallsPage : public Page {
        public:
            CallsPage() { g_page = this; }

            std::string title() const override { return _("Calls"); }

            // Call control is off by default.
            bool shown() const override { return enabled_; }

            void set_visible(bool visible) override {
                visible_ = visible;
                if (!visible)
                    return;
                request();
                contacts::request();
            }

            std::vector<Row> list() override {
                std::vector<Row> rows;
                if (!calls_.empty()) {
                    rows.push_back(heading(_("Calls")));
                    for (size_t i = 0; i < calls_.size(); ++i) {
                        const json& call = calls_[i];
                        Row row;
                        row.text = primary(call) + "\n  " + live_secondary(call);
                        row.tone = call.value("ringing", false) ? Tone::Accent : Tone::Good;
                        row.id = LIVE_PREFIX + std::to_string(i);
                        row.selectable = true;
                        rows.push_back(row);
                    }
                }
                if (!history_.empty()) {
                    rows.push_back(heading(_("Recent calls")));
                    for (size_t i = 0; i < history_.size(); ++i) {
                        const json& entry = history_[i];
                        Row row;
                        row.text = primary(entry) + "\n  " + history_secondary(entry);
                        row.tone = entry.value("type", "") == "missed" ? Tone::Bad : Tone::Normal;
                        row.id = HISTORY_PREFIX + std::to_string(i);
                        row.selectable = true;
                        row.search = ui::fold(entry.value("name", "") + " " + entry.value("number", ""));
                        rows.push_back(row);
                    }
                }
                return rows;
            }

            void cursor_moved(const std::string& id) override { selected_ = id; }

            std::vector<Row> detail() override {
                std::vector<Row> rows;
                if (available_ && !network_.empty())
                    rows.push_back(text_row(network_, Tone::Muted));
                if (const json* call = selected_call()) {
                    const std::string path = call->value("path", "");
                    const bool ringing = call->value("ringing", false);
                    rows.push_back(heading(primary(*call)));
                    rows.push_back(text_row(live_secondary(*call)));
                    rows.push_back(blank_row());
                    if (ringing)
                        rows.push_back(action_row(_("Answer"), [path] { send_action("answer", path); }));
                    if (call->value("connected", false) && audio_routable_)
                        rows.push_back(action_row(_("Audio here"), [] { send_action("audio_here", ""); }));
                    if (call->value("state", "") != "disconnected")
                        rows.push_back(
                            action_row(ringing ? _("Decline") : _("Hang up"), [path] { send_action("hangup", path); }));
                    return rows;
                }
                if (const json* entry = selected_history()) {
                    const std::string number = entry->value("number", "");
                    rows.push_back(heading(primary(*entry)));
                    Row info = text_row(history_secondary(*entry));
                    info.yank = number;
                    rows.push_back(info);
                    rows.push_back(blank_row());
                    if (!number.empty() && available_)
                        // TRANSLATORS: Button that calls a recent caller back, {} is their name or number.
                        rows.push_back(
                            action_row(tr_format(_("Call {}"), primary(*entry)), [number] { dial(number); }));
                    return rows;
                }
                rows.push_back(text_row(status_, Tone::Muted));
                if (available_) {
                    rows.push_back(blank_row());
                    rows.push_back(action_row(_("Call"), [this] { dial_prompt(); }));
                }
                return rows;
            }

            bool action(const std::string& action) override {
                if (action == "open") {
                    dial_prompt();
                    return true;
                }
                if (action == "delete") {
                    if (const json* call = selected_call(); call && call->value("state", "") != "disconnected")
                        send_action("hangup", call->value("path", ""));
                    return true;
                }
                if (action == "refresh") {
                    request();
                    return true;
                }
                return false;
            }

            bool command(const std::string& name, const std::string& args) override {
                if (name == "dial" || name == "call") {
                    if (!available_) {
                        app::status(status_);
                        return true;
                    }
                    if (args.empty())
                        dial_prompt();
                    else
                        dial(args);
                    return true;
                }
                if (name == "answer" || name == "hangup") {
                    for (const auto& call : calls_) {
                        if (name == "answer" ? call.value("ringing", false)
                                             : call.value("state", "") != "disconnected") {
                            send_action(name, call.value("path", ""));
                            break;
                        }
                    }
                    return true;
                }
                return false;
            }

            bool handle_event(const json& event) override {
                const std::string command = event.value("command", "");
                if (command == "bt_status") {
                    enabled_ = event.value("calls_enabled", false);
                    return false;
                }
                if (command == "bt_calls") {
                    calls_ = event.contains("calls") && event["calls"].is_array() ? event["calls"] : json::array();
                    return true;
                }
                if (command == "bt_call_history") {
                    history_ = event.contains("calls") && event["calls"].is_array() ? event["calls"] : json::array();
                    return true;
                }
                if (command == "bt_call_result") {
                    if (!event.value("success", false))
                        app::status(event.value("message", _("The call could not be placed.")));
                    return true;
                }
                if (command == "bt_connection_changed") {
                    const json calls = event.contains("calls") ? event["calls"] : json();
                    available_ = calls.is_object() && calls.value("available", false);
                    network_ = available_ ? ui::network_text(calls) : "";
                    const std::string reason = calls.is_object() ? calls.value("reason", "") : "";
                    const std::string audio = calls.is_object() ? calls.value("audio", "") : "";
                    audio_routable_ = !audio.empty() && audio != "active";
                    status_ =
                        available_
                            ? std::string(_("No calls.")) + (reason.empty() ? "" : "\n" + reason)
                            : (reason.empty() ? _("Call control is off. Turn it on with 'tether --bt-calls-enable on'.")
                                              : reason);
                    if (visible_)
                        request();
                    return false;
                }
                return false;
            }

            void dial_prompt() {
                if (!available_) {
                    app::status(status_);
                    return;
                }
                app::prompt(
                    std::string(_("Call")) + ": ",
                    [](const std::string& number) {
                        if (!number.empty())
                            dial(number);
                    },
                    [](const std::string& text) { return contacts::complete(text, true); });
            }

        private:
            void request() {
                app::send({{"command", "bt_list_calls"}});
                app::send({{"command", "bt_list_call_history"}});
            }

            static void send_action(const std::string& action, const std::string& path) {
                json j = {{"command", "bt_call_action"}, {"action", action}};
                if (!path.empty())
                    j["path"] = path;
                app::send(j);
            }

            static void dial(const std::string& number) {
                app::send({{"command", "bt_call_dial"}, {"number", number}});
            }

            static std::string primary(const json& call) {
                const std::string name = call.value("name", "");
                const std::string number = call.value("number", "");
                return !name.empty() ? name : (!number.empty() ? number : _("Unknown caller"));
            }

            static std::string live_secondary(const json& call) {
                const std::string name = call.value("name", "");
                const std::string number = call.value("number", "");
                std::string out = ui::call_state_text(call.value("state", ""));
                if (!name.empty() && !number.empty())
                    out += out.empty() ? number : "  -  " + number;
                return out;
            }

            static std::string history_secondary(const json& entry) {
                const std::string type = entry.value("type", "");
                std::string out = type == "missed" ? _("Missed") : type == "dialed" ? _("Outgoing") : _("Incoming");
                const std::string name = entry.value("name", "");
                const std::string number = entry.value("number", "");
                if (!name.empty() && !number.empty())
                    out += "  -  " + number;
                if (const std::string when = ui::format_call_time(entry.value("timestamp", static_cast<int64_t>(0)));
                    !when.empty())
                    out += "  -  " + when;
                return out;
            }

            const json* selected_call() const {
                if (selected_.rfind(LIVE_PREFIX, 0) != 0)
                    return nullptr;
                const size_t index = std::stoul(selected_.substr(LIVE_PREFIX.size()));
                return index < calls_.size() ? &calls_[index] : nullptr;
            }

            const json* selected_history() const {
                if (selected_.rfind(HISTORY_PREFIX, 0) != 0)
                    return nullptr;
                const size_t index = std::stoul(selected_.substr(HISTORY_PREFIX.size()));
                return index < history_.size() ? &history_[index] : nullptr;
            }

            json calls_ = json::array();
            json history_ = json::array();
            std::string selected_;
            std::string network_;
            std::string status_ = _("Call control is off. Turn it on with 'tether --bt-calls-enable on'.");
            bool enabled_ = false;
            bool visible_ = false;
            bool available_ = false;
            bool audio_routable_ = false;
        };

    } // namespace

    std::unique_ptr<Page> make_calls_page() { return std::make_unique<CallsPage>(); }

} // namespace tether::tui
