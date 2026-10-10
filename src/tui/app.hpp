#pragma once

#include "util.hpp"

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace tether::tui {

    using json = nlohmann::json;

    enum class Tone { Normal, Heading, Muted, Good, Bad, Accent, Outgoing };
    enum class Align { Left, Right, Center };

    // One logical line in a pane
    struct Row {
        std::string text;
        std::string meta;
        Tone tone = Tone::Normal;
        Align align = Align::Left;
        bool bold = false;
        // Keeps the cursor on the same item when the rows are rebuilt.
        std::string id;
        bool selectable = false;
        // Enter on the row.
        std::function<void()> activate;
        // What "y" copies.
        std::string yank;
        // Folded haystack for the list filter.
        std::string search;
    };

    Row heading(const std::string& text);
    Row text_row(const std::string& text, Tone tone = Tone::Normal);
    Row action_row(const std::string& label, std::function<void()> activate, const std::string& id = "");
    Row blank_row();

    // The message box under a conversation.
    struct Composer {
        LineEdit edit;
        bool enabled = false;
        // Why it is shut, or what went wrong with the last send.
        std::string notice;
        bool notice_is_error = false;
    };

    class Page {
    public:
        virtual ~Page() = default;
        virtual std::string title() const = 0;
        // A hidden page has no tab.
        virtual bool shown() const { return true; }
        virtual void set_visible(bool) {}
        // True claims the event so later pages do not see it.
        virtual bool handle_event(const json& event) = 0;
        virtual void handle_disconnect() {}

        virtual std::vector<Row> list() = 0;
        // Detail for whatever the list cursor is on.
        virtual std::vector<Row> detail() = 0;
        // The list cursor moved onto this row.
        virtual void cursor_moved(const std::string&) {}
        // Enter on a list row.
        virtual void open(const std::string&) {}

        // Page-specific normal-mode actions ("open", "delete", ...).
        virtual bool action(const std::string&) { return false; }
        // Ex commands this page understands.
        virtual bool command(const std::string&, const std::string&) { return false; }

        virtual Composer* composer() { return nullptr; }
        virtual void submit() {}
    };

    struct Candidate {
        std::string label;
        std::string value;
    };
    using Completer = std::function<std::vector<Candidate>(const std::string&)>;

    // Services the pages call back into; implemented in main.cpp.
    namespace app {

        bool send(const json& message);
        bool connected();

        // Transient one-line message at the bottom of the screen.
        void status(const std::string& text);

        // Asks y/n; `yes` runs only on y.
        void confirm(const std::string& question, std::function<void()> yes, std::function<void()> no = {});

        // Reads a line on the command row.
        void prompt(const std::string& label,
                    std::function<void(const std::string&)> done,
                    Completer completer = {},
                    const std::string& initial = "");

        // One-shot timer; returns an id for cancel().
        int after(int seconds, std::function<void()> run);
        void cancel(int& id);

        void show_page(const std::string& name);
        // Moves the current tab's list cursor onto this row.
        void select_row(const std::string& id);
        // Keeps the detail pane scrolled to its end, the way a chat does.
        void pin_detail();
        void focus_detail();
        void focus_list();
        void insert_mode();

        // Puts text on the terminal's clipboard (OSC 52).
        void yank(const std::string& text);

        enum class Route { WiFi, Bluetooth };
        void set_route(Route route, bool ok, const std::string& detail);
        void set_unread(int unread);
        void set_airpods(const std::string& name, const std::string& battery);

    } // namespace app

    std::unique_ptr<Page> make_devices_page();
    std::unique_ptr<Page> make_messages_page();
    std::unique_ptr<Page> make_notifications_page();
    std::unique_ptr<Page> make_contacts_page();
    std::unique_ptr<Page> make_calls_page();
    std::unique_ptr<Page> make_settings_page();

    // Cross-page entry points.
    void messages_open_thread(const std::string& thread_key);
    void messages_new_message();
    void devices_trigger_discovery();

    // The phonebook, shared by every address prompt.
    namespace contacts {
        void request();
        void update(const json& event);
        std::string name_for(const std::string& thread_key);
        std::vector<Candidate> complete(const std::string& text, bool tel_only);
    } // namespace contacts

} // namespace tether::tui
