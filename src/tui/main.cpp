#include "app.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <tether/base64.hpp>
#include <tether/client.hpp>
#include <tether/crypto.hpp>
#include <tether/i18n.hpp>
#include <tether/net.hpp>
#include <tether/version.hpp>
#include <ui_text.hpp>
#include <unistd.h>

// Keeps ncurses from defining move(), erase() and friends as macros.
#define NCURSES_NOMACROS
#include <ncurses.h>

namespace tether::tui {

    Row heading(const std::string& text) {
        Row row;
        row.text = text;
        row.tone = Tone::Heading;
        row.bold = true;
        return row;
    }

    Row text_row(const std::string& text, Tone tone) {
        Row row;
        row.text = text;
        row.tone = tone;
        return row;
    }

    Row action_row(const std::string& label, std::function<void()> activate, const std::string& id) {
        Row row;
        row.text = "[ " + label + " ]";
        row.tone = Tone::Accent;
        row.selectable = true;
        row.activate = std::move(activate);
        row.id = id.empty() ? label : id;
        return row;
    }

    Row blank_row() { return Row{}; }

    namespace {

        using Clock = std::chrono::steady_clock;

        enum class Mode { Normal, Insert, Command, Prompt, Confirm, Help };
        enum class Focus { List, Detail };

        struct Timer {
            int id;
            Clock::time_point at;
            std::function<void()> run;
        };

        // Where the cursor and scroll sit on one page.
        struct View {
            std::string list_id;
            int list_index = 0;
            int list_top = 0;
            std::string detail_id;
            int detail_index = 0;
            // INT_MAX keeps a pane pinned to its bottom, the way a chat scrolls.
            int detail_top = 0;
            std::string filter;
            std::string reported_id = "\x01";
        };

        struct Tab {
            const char* name;
            std::unique_ptr<Page> page;
            View view;
        };

        struct RouteState {
            bool ok = false;
            std::string detail;
        };

        struct State {
            std::vector<Tab> tabs;
            size_t current = 0;
            Focus focus = Focus::List;
            Mode mode = Mode::Normal;
            bool quit = false;

            Keymap keys;

            int daemon_fd = -1;
            LineBuffer daemon_lines;
            int reconnect_timer = 0;
            bool autostart_attempted = false;

            std::string message;
            bool message_is_error = false;

            LineEdit line;
            std::string prompt_label;
            std::function<void(const std::string&)> prompt_done;
            Completer completer;
            std::vector<Candidate> candidates;
            int candidate = -1;

            std::string confirm_question;
            std::function<void()> confirm_yes;
            std::function<void()> confirm_no;
            // Several confirmations can be asked for at once; they queue.
            std::vector<std::tuple<std::string, std::function<void()>, std::function<void()>>> confirm_queue;
            bool searching = false;

            std::vector<Timer> timers;
            int next_timer = 1;

            RouteState routes[2];
            int unread = 0;
            std::string airpods_name;
            std::string airpods_battery;

            // Rows from the last frame, so keys act on what is on screen.
            std::vector<Row> list_rows;
            std::vector<Row> detail_rows;
            int list_height = 0;
            int detail_height = 0;
            int detail_lines = 0;
        };

        State g;

        volatile std::sig_atomic_t g_signalled = 0;

        Tab& tab() { return g.tabs[g.current]; }

        Tab* find_tab(const std::string& name) {
            for (auto& t : g.tabs)
                if (name == t.name)
                    return &t;
            return nullptr;
        }

        void dispatch(const json& event);
        void switch_tab(size_t index);
        void handle_disconnect();

        void close_daemon() {
            if (g.daemon_fd >= 0)
                close(g.daemon_fd);
            g.daemon_fd = -1;
            g.daemon_lines.clear();
        }

        void schedule_reconnect();

        void connect_daemon() {
            close_daemon();
            const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0) {
                schedule_reconnect();
                return;
            }
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::string path;
            try {
                path = tether::get_runtime_dir() + "/tetherd.sock";
            } catch (const std::exception&) {
                close(fd);
                schedule_reconnect();
                return;
            }
            std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                close(fd);
                if (!g.autostart_attempted) {
                    g.autostart_attempted = true;
                    tether::spawn_daemon();
                }
                app::status(_("Daemon Offline"));
                app::set_route(app::Route::WiFi, false, _("The Tether daemon is not running."));
                app::set_route(app::Route::Bluetooth, false, _("The Tether daemon is not running."));
                schedule_reconnect();
                return;
            }
            g.daemon_fd = fd;
            app::send({{"command", "subscribe"}});
            app::send({{"command", "bt_connection"}});
            app::send({{"command", "bt_status"}});
            app::send({{"command", "bt_list_devices"}});
            app::send({{"command", "bt_airpods"}});
            // primes the unread count
            app::send({{"command", "bt_list_threads"}});
            app::status(_("Daemon Online"));
            devices_trigger_discovery();
            tab().page->set_visible(true);
        }

        void schedule_reconnect() {
            if (g.reconnect_timer == 0)
                g.reconnect_timer = app::after(2, [] {
                    g.reconnect_timer = 0;
                    connect_daemon();
                });
        }

        void handle_disconnect() {
            close_daemon();
            app::status(_("Daemon Offline"));
            app::set_route(app::Route::WiFi, false, _("The Tether daemon is not running."));
            app::set_route(app::Route::Bluetooth, false, _("The Tether daemon is not running."));
            for (auto& t : g.tabs)
                t.page->handle_disconnect();
            schedule_reconnect();
        }

        void read_daemon() {
            char buffer[65536];
            while (true) {
                const ssize_t n = ::read(g.daemon_fd, buffer, sizeof(buffer));
                if (n > 0) {
                    for (const std::string& line : g.daemon_lines.feed(std::string_view(buffer, n))) {
                        if (line.empty() || line == "OK")
                            continue;
                        try {
                            dispatch(json::parse(line));
                        } catch (const std::exception&) {
                            // an unreadable event is dropped
                        }
                        if (g.daemon_fd < 0)
                            return;
                    }
                    if (static_cast<size_t>(n) < sizeof(buffer))
                        return;
                    continue;
                }
                if (n < 0 && errno == EINTR)
                    continue;
                handle_disconnect();
                return;
            }
        }

        void dispatch(const json& event) {
            contacts::update(event);
            find_tab("settings")->page->handle_event(event);
            if (event.value("command", "") == "bt_status") {
                find_tab("calls")->page->handle_event(event);
                if (!tab().page->shown())
                    switch_tab(0);
            }
            for (const char* name : {"devices", "contacts", "messages", "notifications", "calls"}) {
                if (find_tab(name)->page->handle_event(event))
                    return;
            }
        }

        int poll_timeout() {
            int timeout = 1000;
            const auto now = Clock::now();
            for (const auto& timer : g.timers) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(timer.at - now).count();
                timeout = std::min<int>(timeout, std::max<long>(0, left));
            }
            return timeout;
        }

        void run_timers() {
            const auto now = Clock::now();
            std::vector<Timer> due;
            for (auto it = g.timers.begin(); it != g.timers.end();) {
                if (it->at <= now) {
                    due.push_back(std::move(*it));
                    it = g.timers.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto& timer : due)
                timer.run();
        }

        enum Pair { P_HEADING = 1, P_MUTED, P_GOOD, P_BAD, P_ACCENT, P_OUTGOING, P_BAR };

        attr_t tone_attr(Tone tone) {
            switch (tone) {
            case Tone::Heading:
                return COLOR_PAIR(P_HEADING) | A_BOLD;
            case Tone::Muted:
                return COLOR_PAIR(P_MUTED) | A_DIM;
            case Tone::Good:
                return COLOR_PAIR(P_GOOD);
            case Tone::Bad:
                return COLOR_PAIR(P_BAD);
            case Tone::Accent:
                return COLOR_PAIR(P_ACCENT);
            case Tone::Outgoing:
                return COLOR_PAIR(P_OUTGOING);
            case Tone::Normal:
                break;
            }
            return A_NORMAL;
        }

        void put(int y, int x, int width, const std::wstring& text, attr_t attr = A_NORMAL) {
            if (width <= 0)
                return;
            const std::wstring shown = fit(text, width);
            attron(attr);
            mvaddnwstr(y, x, shown.c_str(), static_cast<int>(shown.size()));
            attroff(attr);
        }

        void fill(int y, int x, int width, attr_t attr) {
            attron(attr);
            mvhline(y, x, ' ', width);
            attroff(attr);
        }

        struct Line {
            std::wstring text;
            int row;
        };

        // The list keeps rows to their own lines; the detail wraps them.
        std::vector<Line> layout(const std::vector<Row>& rows, int width, bool wrap_text) {
            std::vector<Line> lines;
            for (size_t i = 0; i < rows.size(); ++i) {
                const Row& row = rows[i];
                std::vector<std::wstring> parts;
                if (wrap_text) {
                    parts = wrap(row.text, width);
                } else {
                    size_t start = 0;
                    while (true) {
                        const size_t nl = row.text.find('\n', start);
                        parts.push_back(widen(row.text.substr(start, nl == std::string::npos ? nl : nl - start)));
                        if (nl == std::string::npos)
                            break;
                        start = nl + 1;
                    }
                }
                for (auto& part : parts)
                    lines.push_back({std::move(part), static_cast<int>(i)});
            }
            return lines;
        }

        std::vector<Row> filtered(std::vector<Row> rows, const std::string& filter) {
            if (filter.empty())
                return rows;
            const std::string needle = ui::fold(filter);
            rows.erase(std::remove_if(rows.begin(),
                                      rows.end(),
                                      [&](const Row& row) {
                                          return !row.search.empty() && row.search.find(needle) == std::string::npos;
                                      }),
                       rows.end());
            return rows;
        }

        std::vector<int> selectable_rows(const std::vector<Row>& rows) {
            std::vector<int> out;
            for (size_t i = 0; i < rows.size(); ++i)
                if (rows[i].selectable)
                    out.push_back(static_cast<int>(i));
            return out;
        }

        // Puts the cursor back on the row it was on, by id, else nearby.
        int resolve_cursor(const std::vector<Row>& rows, const std::string& id, int index) {
            const auto selectable = selectable_rows(rows);
            if (selectable.empty())
                return -1;
            if (!id.empty())
                for (int i : selectable)
                    if (rows[i].id == id)
                        return i;
            for (int i : selectable)
                if (i >= index)
                    return i;
            return selectable.back();
        }

        // Draws a pane and keeps the cursor row in view; returns the lines laid out.
        int draw_pane(int top,
                      int left,
                      int height,
                      int width,
                      const std::vector<Row>& rows,
                      int cursor,
                      int& scroll,
                      bool focused,
                      bool wrap_text) {
            const std::vector<Line> lines = layout(rows, width - 1, wrap_text);
            const int total = static_cast<int>(lines.size());
            int first = -1, last = -1;
            for (int i = 0; i < total; ++i) {
                if (lines[i].row == cursor) {
                    if (first < 0)
                        first = i;
                    last = i;
                }
            }
            const int max_scroll = std::max(0, total - height);
            if (scroll == INT_MAX) {
                scroll = max_scroll;
            } else {
                if (first >= 0) {
                    if (last >= scroll + height)
                        scroll = last - height + 1;
                    if (first < scroll)
                        scroll = first;
                    // Text above the first control would otherwise never be seen.
                    const auto selectable = selectable_rows(rows);
                    if (!selectable.empty() && cursor == selectable.front() && first < height)
                        scroll = 0;
                }
                scroll = std::clamp(scroll, 0, max_scroll);
            }

            for (int y = 0; y < height && scroll + y < total; ++y) {
                const Line& line = lines[scroll + y];
                const Row& row = rows[line.row];
                attr_t attr = tone_attr(row.tone) | (row.bold ? A_BOLD : A_NORMAL);

                const bool selected = line.row == cursor && (focused || !wrap_text);
                if (selected)
                    attr = focused ? A_REVERSE : (tone_attr(Tone::Accent) | A_BOLD);
                const int line_width = width_of(line.text);
                int x = left;
                if (row.align == Align::Right)
                    x = left + std::max(0, width - 1 - line_width);
                else if (row.align == Align::Center)
                    x = left + std::max(0, (width - 1 - line_width) / 2);
                if (selected)
                    fill(top + y, left, width - 1, attr);
                // Meta sits right-aligned on a row's first line.
                const bool first_line = scroll + y == 0 || lines[scroll + y - 1].row != line.row;
                int room = width - 1 - (x - left);
                if (first_line && !row.meta.empty()) {
                    const std::wstring meta = widen(row.meta);
                    const int meta_width = width_of(meta);
                    if (meta_width + 4 < room) {
                        put(top + y,
                            left + width - 1 - meta_width,
                            meta_width,
                            meta,
                            selected ? attr : tone_attr(Tone::Muted));
                        room -= meta_width + 1;
                    }
                }
                put(top + y, x, room, line.text, attr);
            }
            if (total > height) {
                // A scroll mark on the right edge.
                const int mark = height <= 1 ? 0 : (scroll * (height - 1)) / std::max(1, max_scroll);
                attron(COLOR_PAIR(P_MUTED));
                mvaddwstr(top + mark, left + width - 1, L"▐");
                attroff(COLOR_PAIR(P_MUTED));
            }
            return total;
        }

        std::string route_text(app::Route route) {
            const RouteState& r = g.routes[static_cast<int>(route)];
            // TRANSLATORS: {} is a transport name, "Wi-Fi" or "Bluetooth".
            return tr_format(_("{}: {}"),
                             route == app::Route::WiFi ? "Wi-Fi" : "Bluetooth",
                             r.ok ? _("connected") : _("not connected"));
        }

        void draw_tabs(int width) {
            fill(0, 0, width, COLOR_PAIR(P_BAR));
            int x = 0;
            put(0, x, width, L" Tether ", COLOR_PAIR(P_BAR) | A_BOLD);
            x += 8;
            for (size_t i = 0; i < g.tabs.size(); ++i) {
                const Tab& t = g.tabs[i];
                if (!t.page->shown())
                    continue;
                std::string label = " " + std::to_string(i + 1) + " " + t.page->title();
                if (std::string(t.name) == "messages" && g.unread > 0)
                    label += " (" + std::to_string(g.unread) + ")";
                label += " ";
                const std::wstring wide = widen(label);
                put(0, x, width - x, wide, i == g.current ? (A_REVERSE | A_BOLD) : COLOR_PAIR(P_BAR));
                x += width_of(wide);
                if (x >= width)
                    break;
            }
        }

        void draw_status_line(int y, int width) {
            fill(y, 0, width, COLOR_PAIR(P_BAR));
            std::vector<std::string> parts = {route_text(app::Route::WiFi), route_text(app::Route::Bluetooth)};
            if (!g.airpods_battery.empty())
                parts.push_back(tr_format(_("{}: {}"), g.airpods_name, g.airpods_battery));
            if (g.daemon_fd < 0)
                parts.push_back(_("Daemon: not running"));
            if (g.unread > 0)
                parts.push_back(tr_format(P_("{} unread message", "{} unread messages", g.unread), g.unread));
            std::string text = " ";
            for (size_t i = 0; i < parts.size(); ++i)
                text += (i ? "  │  " : "") + parts[i];
            put(y, 0, width, widen(text), COLOR_PAIR(P_BAR));
            const std::wstring version = widen("v" TETHER_VERSION " ");
            const int vw = width_of(version);
            if (width_of(widen(text)) + vw + 2 < width)
                put(y, width - vw, vw, version, COLOR_PAIR(P_BAR) | A_DIM);
        }

        // Returns the cursor position the terminal should show, or {-1,-1}.
        std::pair<int, int> draw_bottom_line(int y, int width) {
            move(y, 0);
            clrtoeol();
            switch (g.mode) {
            case Mode::Command:
            case Mode::Prompt: {
                const std::wstring label = widen(g.mode == Mode::Command ? (g.searching ? "/" : ":") : g.prompt_label);
                const std::wstring& text = g.line.wide();
                const int label_width = width_of(label);
                const int room = std::max(1, width - label_width - 1);
                // Scrolls horizontally so the cursor stays on screen.
                const int before = width_of(std::wstring_view(text).substr(0, g.line.cursor()));
                int skip = 0;
                std::wstring visible = text;
                while (before - width_of(std::wstring_view(text).substr(0, skip)) >= room && skip < (int)text.size())
                    ++skip;
                visible = text.substr(skip);
                put(y, 0, width, label, A_BOLD);
                put(y, label_width, room, visible);
                return {y, label_width + before - width_of(std::wstring_view(text).substr(0, skip))};
            }
            case Mode::Confirm: {
                // A long question grows upward over the panes.
                const auto lines = wrap(g.confirm_question + "  [y/N]", width - 1);
                const int first = std::max(1, y - static_cast<int>(lines.size()) + 1);
                for (int i = 0; first + i <= y; ++i) {
                    fill(first + i, 0, width, A_BOLD);
                    put(first + i, 0, width, lines[lines.size() - (y - first + 1) + i], A_BOLD);
                }
                return {-1, -1};
            }
            case Mode::Insert:
                put(y, 0, width, widen(_("-- INSERT --")), A_BOLD);
                return {-1, -1};
            default:
                break;
            }
            if (!g.message.empty())
                put(y, 0, width, widen(g.message), g.message_is_error ? tone_attr(Tone::Bad) : A_NORMAL);
            return {-1, -1};
        }

        void draw_candidates(int bottom, int width) {
            if (g.mode != Mode::Prompt && g.mode != Mode::Command)
                return;
            const int shown = std::min<int>(g.candidates.size(), 8);
            const int start = std::clamp(g.candidate - shown + 1, 0, std::max(0, (int)g.candidates.size() - shown));
            for (int i = 0; i < shown; ++i) {
                const int index = start + i;
                const int y = bottom - shown + i;
                const attr_t attr = index == g.candidate ? A_REVERSE : (COLOR_PAIR(P_BAR));
                fill(y, 0, width, attr);
                put(y, 1, width - 2, widen(g.candidates[index].label), attr);
            }
        }

        // What the help overlay lists, in order.
        const std::pair<const char*, const char*> HELP[] = {
            {"j k  ↓ ↑", N_("Move down / up")},
            {"gg G", N_("First / last")},
            {"C-d C-u", N_("Half a page down / up")},
            {"h l  C-w h/l  Tab", N_("Focus the list / the detail pane")},
            {"Enter", N_("Open, or run the highlighted action")},
            {"Space", N_("Toggle the highlighted setting")},
            {"1-6  gt gT", N_("Switch tab")},
            {"/", N_("Filter the list")},
            {"Esc", N_("Clear the filter, leave insert mode")},
            {"i a", N_("Write in the open conversation")},
            {"Enter / C-j", N_("Send / new line (insert mode)")},
            {"o", N_("New message, or dial on Calls")},
            {"dd", N_("Dismiss, forget, unpair, or hang up")},
            {"yy", N_("Copy the highlighted address or text")},
            {"r", N_("Refresh, or scan for devices")},
            {":send FILE…", N_("Send files to the selected device")},
            {":clip", N_("Send the clipboard")},
            {":dial NUMBER", N_("Place a call")},
            {":msg ADDRESS", N_("Message someone")},
            {":q  ZZ", N_("Quit")},
        };

        void draw_help(int height, int width) {
            const int rows = static_cast<int>(std::size(HELP)) + 4;
            const int box_width = std::min(width - 4, 72);
            const int top = std::max(1, (height - rows) / 2);
            const int left = std::max(0, (width - box_width) / 2);
            for (int y = 0; y < rows && top + y < height - 1; ++y)
                fill(top + y, left, box_width, COLOR_PAIR(P_BAR));
            put(top + 1, left + 2, box_width - 4, widen(_("Keys")), COLOR_PAIR(P_BAR) | A_BOLD);
            int y = top + 2;
            for (const auto& [keys, what] : HELP) {
                if (y >= height - 2)
                    break;
                put(y, left + 2, 20, widen(keys), COLOR_PAIR(P_BAR) | A_BOLD);
                put(y, left + 22, box_width - 24, widen(_(what)), COLOR_PAIR(P_BAR));
                ++y;
            }
        }

        // Reconciles cursors with the rows the pages now report, and tells a
        // page when its list cursor landed on something new.
        void sync() {
            for (int pass = 0; pass < 2; ++pass) {
                Tab& t = tab();
                View& v = t.view;
                g.list_rows = filtered(t.page->list(), v.filter);
                const int cursor = resolve_cursor(g.list_rows, v.list_id, v.list_index);
                v.list_index = std::max(0, cursor);
                v.list_id = cursor >= 0 ? g.list_rows[cursor].id : "";
                if (v.list_id == v.reported_id)
                    break;
                v.reported_id = v.list_id;
                v.detail_id.clear();
                v.detail_index = 0;
                v.detail_top = 0;
                t.page->cursor_moved(v.list_id);
            }
            g.detail_rows = tab().page->detail();
            View& v = tab().view;
            const int cursor = resolve_cursor(g.detail_rows, v.detail_id, v.detail_index);
            v.detail_index = std::max(0, cursor);
            v.detail_id = cursor >= 0 ? g.detail_rows[cursor].id : "";
            if (selectable_rows(g.detail_rows).empty() && g.focus == Focus::Detail && !tab().page->composer())
                g.focus = Focus::List;
        }

        void draw() {
            sync();
            erase();
            int height, width;
            getmaxyx(stdscr, height, width);
            if (height < 6 || width < 30) {
                put(0, 0, width, widen(_("Terminal too small")));
                refresh();
                return;
            }

            draw_tabs(width);
            View& v = tab().view;
            const int body_top = 1;
            const int body_height = height - 3;
            const int list_width = std::clamp(width / 3, 24, 48);
            const int detail_left = list_width + 1;
            const int detail_width = width - detail_left;

            int list_top_row = body_top;
            int list_height = body_height;
            if (!v.filter.empty() || (g.mode == Mode::Command && g.searching)) {
                put(body_top, 0, list_width, widen("/" + v.filter), tone_attr(Tone::Accent));
                ++list_top_row;
                --list_height;
            }
            g.list_height = list_height;
            draw_pane(list_top_row,
                      0,
                      list_height,
                      list_width,
                      g.list_rows,
                      g.list_rows.empty() ? -1 : v.list_index,
                      v.list_top,
                      g.focus == Focus::List,
                      false);

            attron(COLOR_PAIR(P_MUTED));
            mvvline(body_top, list_width, ACS_VLINE, body_height);
            attroff(COLOR_PAIR(P_MUTED));

            // The composer takes the bottom of the detail pane.
            Composer* composer = tab().page->composer();
            std::vector<std::wstring> composer_lines;
            int composer_height = 0;
            std::pair<int, int> cursor_at{-1, -1};
            if (composer) {
                const int inner = std::max(1, detail_width - 3);
                composer_lines = wrap_chars(composer->edit.wide(), inner);
                const int text_rows = std::min<int>(composer_lines.size(), 5);
                composer_height = 1 + text_rows + (composer->notice.empty() ? 0 : 1);
            }

            const int detail_height = body_height - composer_height;
            g.detail_height = detail_height;
            const bool detail_has_cursor = !selectable_rows(g.detail_rows).empty();
            g.detail_lines = draw_pane(body_top,
                                       detail_left + 1,
                                       detail_height,
                                       detail_width - 1,
                                       g.detail_rows,
                                       detail_has_cursor ? v.detail_index : -1,
                                       v.detail_top,
                                       g.focus == Focus::Detail,
                                       true);

            if (composer) {
                int y = body_top + detail_height;
                attron(COLOR_PAIR(P_MUTED));
                mvhline(y, detail_left, ACS_HLINE, detail_width);
                attroff(COLOR_PAIR(P_MUTED));
                ++y;
                if (!composer->notice.empty()) {
                    put(y++,
                        detail_left + 1,
                        detail_width - 2,
                        widen(composer->notice),
                        tone_attr(composer->notice_is_error ? Tone::Bad : Tone::Muted));
                }
                const int inner = std::max(1, detail_width - 3);
                const std::wstring& text = composer->edit.wide();
                const auto before = wrap_chars(std::wstring_view(text).substr(0, composer->edit.cursor()), inner);
                const int cursor_line = static_cast<int>(before.size()) - 1;
                const int text_rows = std::min<int>(composer_lines.size(), 5);
                const int first =
                    std::clamp(cursor_line - text_rows + 1, 0, std::max(0, (int)composer_lines.size() - text_rows));
                const attr_t attr = composer->enabled ? A_NORMAL : tone_attr(Tone::Muted);
                put(y, detail_left, 2, L"› ", tone_attr(Tone::Accent));
                if (composer->edit.empty() && g.mode != Mode::Insert)
                    put(y,
                        detail_left + 2,
                        inner,
                        widen(composer->enabled ? _("Press i to write a message") : ""),
                        tone_attr(Tone::Muted));
                for (int i = 0; i < text_rows; ++i)
                    put(y + i, detail_left + 2, inner, composer_lines[first + i], attr);
                if (g.mode == Mode::Insert)
                    cursor_at = {y + cursor_line - first, detail_left + 2 + width_of(before.back())};
            }

            draw_status_line(height - 2, width);
            const auto bottom_cursor = draw_bottom_line(height - 1, width);
            if (bottom_cursor.first >= 0)
                cursor_at = bottom_cursor;
            draw_candidates(height - 2, width);
            if (g.mode == Mode::Help)
                draw_help(height, width);

            if (cursor_at.first >= 0) {
                curs_set(1);
                move(cursor_at.first, cursor_at.second);
            } else {
                curs_set(0);
            }
            refresh();
        }

        // ---- input ---------------------------------------------------------

        std::string key_token(int kind, wint_t ch) {
            if (kind == KEY_CODE_YES) {
                switch (ch) {
                case KEY_UP:
                    return "<Up>";
                case KEY_DOWN:
                    return "<Down>";
                case KEY_LEFT:
                    return "<Left>";
                case KEY_RIGHT:
                    return "<Right>";
                case KEY_HOME:
                    return "<Home>";
                case KEY_END:
                    return "<End>";
                case KEY_BACKSPACE:
                    return "<BS>";
                case KEY_DC:
                    return "<Del>";
                case KEY_ENTER:
                    return "<CR>";
                case KEY_NPAGE:
                    return "<PageDown>";
                case KEY_PPAGE:
                    return "<PageUp>";
                case KEY_BTAB:
                    return "<S-Tab>";
                case KEY_RESIZE:
                    return "<Resize>";
                default:
                    return "";
                }
            }
            switch (ch) {
            case 27:
                return "<Esc>";
            case '\r':
                return "<CR>";
            case '\n':
                return "<C-j>";
            case '\t':
                return "<Tab>";
            case 127:
            case 8:
                return "<BS>";
            case ' ':
                return "<Space>";
            default:
                break;
            }
            if (ch < 32)
                return std::string("<C-") + static_cast<char>('a' + ch - 1) + ">";
            return narrow(std::wstring(1, static_cast<wchar_t>(ch)));
        }

        void move_cursor(int delta) {
            View& v = tab().view;
            if (g.focus == Focus::List) {
                const auto selectable = selectable_rows(g.list_rows);
                if (selectable.empty())
                    return;
                auto it = std::find(selectable.begin(), selectable.end(), v.list_index);
                int at = it == selectable.end() ? 0 : static_cast<int>(it - selectable.begin());
                at = std::clamp(at + delta, 0, static_cast<int>(selectable.size()) - 1);
                v.list_index = selectable[at];
                v.list_id = g.list_rows[v.list_index].id;
                return;
            }
            const auto selectable = selectable_rows(g.detail_rows);
            if (selectable.empty()) {
                // Plain text, such as a conversation, scrolls instead.
                const int max_top = std::max(0, g.detail_lines - g.detail_height);
                int top = v.detail_top == INT_MAX ? max_top : v.detail_top;
                top = std::clamp(top + delta, 0, max_top);
                v.detail_top = top == max_top ? INT_MAX : top;
                return;
            }
            auto it = std::find(selectable.begin(), selectable.end(), v.detail_index);
            int at = it == selectable.end() ? 0 : static_cast<int>(it - selectable.begin());
            at = std::clamp(at + delta, 0, static_cast<int>(selectable.size()) - 1);
            v.detail_index = selectable[at];
            v.detail_id = g.detail_rows[v.detail_index].id;
        }

        const Row* current_row() {
            View& v = tab().view;
            if (g.focus == Focus::List)
                return v.list_index < (int)g.list_rows.size() && g.list_rows[v.list_index].selectable
                           ? &g.list_rows[v.list_index]
                           : nullptr;
            return v.detail_index < (int)g.detail_rows.size() && g.detail_rows[v.detail_index].selectable
                       ? &g.detail_rows[v.detail_index]
                       : nullptr;
        }

        void switch_tab(size_t index) {
            if (index >= g.tabs.size() || !g.tabs[index].page->shown() || index == g.current)
                return;
            tab().page->set_visible(false);
            g.current = index;
            g.focus = Focus::List;
            tab().view.reported_id = "\x01";
            tab().page->set_visible(true);
        }

        void cycle_tab(int delta) {
            const int n = static_cast<int>(g.tabs.size());
            int index = static_cast<int>(g.current);
            for (int i = 0; i < n; ++i) {
                index = (index + delta + n) % n;
                if (g.tabs[index].page->shown()) {
                    switch_tab(index);
                    return;
                }
            }
        }

        std::vector<Candidate> complete_path(const std::string& partial) {
            namespace fs = std::filesystem;
            std::vector<Candidate> out;
            std::string expanded = partial;
            if (expanded.rfind("~/", 0) == 0)
                if (const char* home = getenv("HOME"))
                    expanded = std::string(home) + expanded.substr(1);
            const size_t slash = expanded.rfind('/');
            const std::string dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : expanded.substr(0, slash));
            const std::string stem = slash == std::string::npos ? expanded : expanded.substr(slash + 1);
            const std::string shown_dir = slash == std::string::npos ? "" : partial.substr(0, partial.rfind('/') + 1);
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                const std::string name = entry.path().filename().string();
                if (name.rfind(stem, 0) != 0 || (name[0] == '.' && stem.empty()))
                    continue;
                const bool directory = entry.is_directory(ec);
                out.push_back({shown_dir + name + (directory ? "/" : ""), shown_dir + name + (directory ? "/" : "")});
                if (out.size() >= 200)
                    break;
            }
            std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.value < b.value; });
            return out;
        }

        // Completes the last word of an ex command that takes paths.
        std::vector<Candidate> complete_command(const std::string& text) {
            const size_t space = text.find(' ');
            if (space == std::string::npos)
                return {};
            const std::string verb = text.substr(0, space);
            const size_t last = text.rfind(' ');
            const std::string head = text.substr(0, last + 1);
            const std::string word = text.substr(last + 1);
            std::vector<Candidate> out;
            if (verb == "send" || verb == "s") {
                for (auto& c : complete_path(word))
                    out.push_back({c.label, head + c.value});
            } else if (verb == "msg" || verb == "dial") {
                for (auto& c : contacts::complete(text.substr(space + 1), verb == "dial"))
                    out.push_back({c.label, verb + " " + c.value});
            }
            return out;
        }

        void refresh_candidates() {
            g.candidate = -1;
            if (g.mode == Mode::Prompt && g.completer)
                g.candidates = g.completer(g.line.text());
            else if (g.mode == Mode::Command && !g.searching)
                g.candidates = complete_command(g.line.text());
            else
                g.candidates.clear();
        }

        void run_command(const std::string& line) {
            std::string text = line;
            while (!text.empty() && text.back() == ' ')
                text.pop_back();
            if (text.empty())
                return;
            const size_t space = text.find(' ');
            const std::string name = text.substr(0, space);
            const std::string args = space == std::string::npos ? "" : text.substr(space + 1);
            if (name == "q" || name == "q!" || name == "qa" || name == "qa!" || name == "quit" || name == "x" ||
                name == "wq") {
                g.quit = true;
                return;
            }
            if (name == "help" || name == "h") {
                g.mode = Mode::Help;
                return;
            }
            for (size_t i = 0; i < g.tabs.size(); ++i) {
                if (name == g.tabs[i].name) {
                    switch_tab(i);
                    return;
                }
            }
            if (!name.empty() && std::all_of(name.begin(), name.end(), ::isdigit)) {
                switch_tab(static_cast<size_t>(std::stoi(name)) - 1);
                return;
            }
            if (tab().page->command(name, args))
                return;
            for (auto& t : g.tabs)
                if (&t != &tab() && t.page->command(name, args))
                    return;
            g.message = tr_format(_("Not a command: {}"), name);
            g.message_is_error = true;
        }

        void next_confirm() {
            if (g.confirm_queue.empty()) {
                if (g.mode == Mode::Confirm)
                    g.mode = Mode::Normal;
                return;
            }
            auto [question, yes, no] = std::move(g.confirm_queue.front());
            g.confirm_queue.erase(g.confirm_queue.begin());
            g.confirm_question = std::move(question);
            g.confirm_yes = std::move(yes);
            g.confirm_no = std::move(no);
            g.mode = Mode::Confirm;
        }

        void half_page(int sign) {
            const int rows = std::max(1, (g.focus == Focus::List ? g.list_height : g.detail_height) / 2);
            move_cursor(sign * rows);
        }

        void normal_action(const std::string& action) {
            View& v = tab().view;
            if (action == "down")
                move_cursor(1);
            else if (action == "up")
                move_cursor(-1);
            else if (action == "top")
                move_cursor(-100000);
            else if (action == "bottom")
                move_cursor(100000);
            else if (action == "half_down")
                half_page(1);
            else if (action == "half_up")
                half_page(-1);
            else if (action == "page_down")
                half_page(2);
            else if (action == "page_up")
                half_page(-2);
            else if (action == "focus_list")
                g.focus = Focus::List;
            else if (action == "focus_detail" || (action == "toggle_focus" && g.focus == Focus::List)) {
                if (g.focus == Focus::List && !v.list_id.empty())
                    tab().page->open(v.list_id);
                g.focus = Focus::Detail;
            } else if (action == "toggle_focus")
                g.focus = Focus::List;
            else if (action == "next_tab")
                cycle_tab(1);
            else if (action == "prev_tab")
                cycle_tab(-1);
            else if (action.rfind("tab", 0) == 0)
                switch_tab(static_cast<size_t>(action[3] - '1'));
            else if (action == "activate") {
                if (g.focus == Focus::List) {
                    if (!v.list_id.empty()) {
                        tab().page->open(v.list_id);
                        g.focus = Focus::Detail;
                    }
                } else if (const Row* row = current_row(); row && row->activate) {
                    const auto run = row->activate;
                    run();
                }
            } else if (action == "toggle") {
                if (!tab().page->action("toggle"))
                    normal_action("activate");
            } else if (action == "search") {
                g.mode = Mode::Command;
                g.searching = true;
                g.focus = Focus::List;
                g.line.set(v.filter);
                g.candidates.clear();
            } else if (action == "command") {
                g.mode = Mode::Command;
                g.searching = false;
                g.line.clear();
                g.candidates.clear();
            } else if (action == "escape") {
                v.filter.clear();
                g.message.clear();
            } else if (action == "help") {
                g.mode = Mode::Help;
            } else if (action == "insert") {
                if (Composer* composer = tab().page->composer()) {
                    if (composer->enabled) {
                        g.mode = Mode::Insert;
                        g.focus = Focus::Detail;
                    } else if (!composer->notice.empty()) {
                        app::status(composer->notice);
                    }
                }
            } else if (action == "yank") {
                const Row* row = current_row();
                if (row && !row->yank.empty())
                    app::yank(row->yank);
            } else if (action == "quit") {
                g.quit = true;
            } else if (action == "interrupt") {
                app::status(_("Type :q and press Enter to quit"));
            } else if (action == "redraw") {
                clearok(stdscr, TRUE);
            } else {
                tab().page->action(action);
            }
        }

        void handle_key(const std::string& key) {
            if (key == "<Resize>")
                return;
            switch (g.mode) {
            case Mode::Help:
                g.mode = Mode::Normal;
                return;
            case Mode::Confirm: {
                const auto yes = std::move(g.confirm_yes);
                const auto no = std::move(g.confirm_no);
                g.mode = Mode::Normal;
                if (key == "y" || key == "Y") {
                    if (yes)
                        yes();
                } else if (no) {
                    no();
                }
                if (g.mode == Mode::Normal)
                    next_confirm();
                return;
            }
            case Mode::Insert: {
                Composer* composer = tab().page->composer();
                if (!composer || key == "<Esc>") {
                    g.mode = Mode::Normal;
                    return;
                }
                if (key == "<CR>") {
                    tab().page->submit();
                    return;
                }
                if (!composer->enabled)
                    return;
                if (key == "<C-j>")
                    composer->edit.insert(L'\n');
                else if (!composer->edit.key(key)) {
                    const std::wstring wide = widen(key);
                    if (key == "<Space>")
                        composer->edit.insert(L' ');
                    else if (key.front() != '<' || key.size() == 1)
                        for (wchar_t c : wide)
                            composer->edit.insert(c);
                    else
                        return;
                }
                if (composer->notice_is_error) {
                    composer->notice.clear();
                    composer->notice_is_error = false;
                }
                return;
            }
            case Mode::Command:
            case Mode::Prompt: {
                if (key == "<Esc>" || (key == "<BS>" && g.line.empty())) {
                    if (g.mode == Mode::Command && g.searching && key == "<Esc>")
                        tab().view.filter.clear();
                    g.mode = Mode::Normal;
                    g.candidates.clear();
                    return;
                }
                if (key == "<Tab>" || key == "<S-Tab>" || key == "<C-n>" || key == "<C-p>") {
                    if (g.candidates.empty())
                        refresh_candidates();
                    if (g.candidates.empty())
                        return;
                    const int n = static_cast<int>(g.candidates.size());
                    const int step = (key == "<S-Tab>" || key == "<C-p>") ? -1 : 1;
                    g.candidate = g.candidate < 0 ? (step > 0 ? 0 : n - 1) : (g.candidate + step + n) % n;
                    g.line.set(g.candidates[g.candidate].value);
                    return;
                }
                if (key == "<CR>") {
                    const std::string text = g.line.text();
                    const Mode mode = g.mode;
                    g.mode = Mode::Normal;
                    g.candidates.clear();
                    if (mode == Mode::Command && g.searching) {
                        tab().view.filter = text;
                    } else if (mode == Mode::Command) {
                        run_command(text);
                    } else if (g.prompt_done) {
                        const auto done = std::move(g.prompt_done);
                        done(text);
                    }
                    return;
                }
                if (!g.line.key(key)) {
                    if (key == "<Space>")
                        g.line.insert(L' ');
                    else if (key.front() != '<' || key.size() == 1)
                        for (wchar_t c : widen(key))
                            g.line.insert(c);
                    else
                        return;
                }
                if (g.mode == Mode::Command && g.searching)
                    tab().view.filter = g.line.text();
                refresh_candidates();
                return;
            }
            case Mode::Normal:
                break;
            }
            const std::string action = g.keys.feed(key);
            if (!action.empty())
                normal_action(action);
        }

        void bind_keys() {
            Keymap& k = g.keys;
            for (const char* key : {"j", "<Down>"})
                k.bind({key}, "down");
            for (const char* key : {"k", "<Up>"})
                k.bind({key}, "up");
            k.bind({"g", "g"}, "top");
            k.bind({"<Home>"}, "top");
            k.bind({"G"}, "bottom");
            k.bind({"<End>"}, "bottom");
            k.bind({"<C-d>"}, "half_down");
            k.bind({"<C-u>"}, "half_up");
            k.bind({"<C-f>"}, "page_down");
            k.bind({"<PageDown>"}, "page_down");
            k.bind({"<C-b>"}, "page_up");
            k.bind({"<PageUp>"}, "page_up");
            k.bind({"h"}, "focus_list");
            k.bind({"<Left>"}, "focus_list");
            k.bind({"l"}, "focus_detail");
            k.bind({"<Right>"}, "focus_detail");
            k.bind({"<C-w>", "h"}, "focus_list");
            k.bind({"<C-w>", "l"}, "focus_detail");
            k.bind({"<C-w>", "w"}, "toggle_focus");
            k.bind({"<C-w>", "<C-w>"}, "toggle_focus");
            k.bind({"<Tab>"}, "toggle_focus");
            k.bind({"g", "t"}, "next_tab");
            k.bind({"g", "T"}, "prev_tab");
            for (char c = '1'; c <= '6'; ++c)
                k.bind({std::string(1, c)}, std::string("tab") + c);
            k.bind({"<CR>"}, "activate");
            k.bind({"<Space>"}, "toggle");
            k.bind({"/"}, "search");
            k.bind({":"}, "command");
            k.bind({"<Esc>"}, "escape");
            k.bind({"?"}, "help");
            k.bind({"i"}, "insert");
            k.bind({"a"}, "insert");
            k.bind({"A"}, "insert");
            k.bind({"o"}, "open");
            k.bind({"d", "d"}, "delete");
            k.bind({"y", "y"}, "yank");
            k.bind({"r"}, "refresh");
            k.bind({"Z", "Z"}, "quit");
            k.bind({"Z", "Q"}, "quit");
            k.bind({"<C-c>"}, "interrupt");
            k.bind({"<C-l>"}, "redraw");
        }

        void read_keys() {
            wint_t ch;
            int kind;
            while ((kind = get_wch(&ch)) != ERR) {
                const std::string token = key_token(kind, ch);
                if (!token.empty())
                    handle_key(token);
            }
        }

        void usage() {
            std::printf("%s\n",
                        _("Usage: tether-tui [--devices|--messages|--notifications|--contacts|--calls|--settings] "
                          "[--thread KEY]"));
        }

    } // namespace

    namespace app {

        bool send(const json& message) {
            if (g.daemon_fd < 0)
                return false;

            const std::string payload = message.dump() + "\n";
            size_t written = 0;
            int stalls = 0;
            while (written < payload.size()) {
                const ssize_t n = ::send(g.daemon_fd, payload.data() + written, payload.size() - written, MSG_NOSIGNAL);
                if (n > 0) {
                    written += static_cast<size_t>(n);
                    continue;
                }
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && ++stalls < 200) {
                    usleep(1000);
                    continue;
                }
                return false;
            }
            return true;
        }

        bool connected() { return g.daemon_fd >= 0; }

        void status(const std::string& text) {
            g.message = text;
            g.message_is_error = false;
        }

        void confirm(const std::string& question, std::function<void()> yes, std::function<void()> no) {
            g.confirm_queue.emplace_back(question, std::move(yes), std::move(no));
            if (g.mode != Mode::Confirm)
                next_confirm();
        }

        void prompt(const std::string& label,
                    std::function<void(const std::string&)> done,
                    Completer completer,
                    const std::string& initial) {
            g.mode = Mode::Prompt;
            g.searching = false;
            g.prompt_label = label;
            g.prompt_done = std::move(done);
            g.completer = std::move(completer);
            g.line.set(initial);
            refresh_candidates();
        }

        int after(int seconds, std::function<void()> run) {
            const int id = g.next_timer++;
            g.timers.push_back({id, Clock::now() + std::chrono::seconds(seconds), std::move(run)});
            return id;
        }

        void cancel(int& id) {
            if (id == 0)
                return;
            g.timers.erase(std::remove_if(g.timers.begin(), g.timers.end(), [&](const Timer& t) { return t.id == id; }),
                           g.timers.end());
            id = 0;
        }

        void show_page(const std::string& name) {
            for (size_t i = 0; i < g.tabs.size(); ++i)
                if (name == g.tabs[i].name)
                    switch_tab(i);
        }

        void select_row(const std::string& id) {
            tab().view.list_id = id;
            tab().view.reported_id = id;
        }

        void pin_detail() { tab().view.detail_top = INT_MAX; }

        void focus_detail() { g.focus = Focus::Detail; }
        void focus_list() { g.focus = Focus::List; }

        void insert_mode() {
            if (Composer* composer = tab().page->composer(); composer && composer->enabled) {
                g.mode = Mode::Insert;
                g.focus = Focus::Detail;
            }
        }

        void yank(const std::string& text) {
            const std::string encoded =
                tether::base64_encode(reinterpret_cast<const unsigned char*>(text.data()), text.size());
            std::fprintf(stdout, "\033]52;c;%s\a", encoded.c_str());
            std::fflush(stdout);
            status(_("Copied to the clipboard."));
        }

        void set_route(Route route, bool ok, const std::string& detail) {
            g.routes[static_cast<int>(route)] = {ok, detail};
        }

        void set_unread(int unread) { g.unread = unread; }

        void set_airpods(const std::string& name, const std::string& battery) {
            g.airpods_name = name;
            g.airpods_battery = battery;
        }

    } // namespace app

} // namespace tether::tui

int main(int argc, char** argv) {
    using namespace tether::tui;
    tether::init_locale();

    std::string start_page = "devices";
    std::string thread;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--thread" && i + 1 < argc) {
            thread = argv[++i];
        } else if (arg.rfind("--thread=", 0) == 0) {
            thread = arg.substr(9);
        } else if (arg == "--devices" || arg == "--messages" || arg == "--notifications" || arg == "--contacts" ||
                   arg == "--calls" || arg == "--settings") {
            start_page = arg.substr(2);
        } else if (arg == "--version") {
            std::printf("tether-tui %s\n", TETHER_VERSION);
            return 0;
        } else {
            usage();
            return arg == "--help" || arg == "-h" ? 0 : 2;
        }
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::fprintf(stderr, "%s\n", _("tether-tui needs a terminal."));
        return 1;
    }

    tether::Crypto::instance().init();

    g.tabs.push_back({"devices", make_devices_page(), {}});
    g.tabs.push_back({"messages", make_messages_page(), {}});
    g.tabs.push_back({"notifications", make_notifications_page(), {}});
    g.tabs.push_back({"contacts", make_contacts_page(), {}});
    g.tabs.push_back({"calls", make_calls_page(), {}});
    g.tabs.push_back({"settings", make_settings_page(), {}});
    bind_keys();

    // Logs would scribble over the screen.
    if (!std::getenv("TETHER_TUI_STDERR"))
        std::freopen("/dev/null", "w", stderr);

    std::signal(SIGPIPE, SIG_IGN);
    for (int sig : {SIGTERM, SIGHUP}) {
        struct sigaction action{};
        action.sa_handler = [](int) { g_signalled = 1; };
        sigaction(sig, &action, nullptr);
    }

    set_escdelay(25);
    initscr();
    raw();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(P_HEADING, COLOR_CYAN, -1);
        init_pair(P_MUTED, -1, -1);
        init_pair(P_GOOD, COLOR_GREEN, -1);
        init_pair(P_BAD, COLOR_RED, -1);
        init_pair(P_ACCENT, COLOR_YELLOW, -1);
        init_pair(P_OUTGOING, COLOR_BLUE, -1);
        init_pair(P_BAR, COLOR_BLACK, COLOR_WHITE);
    }

    app::set_route(app::Route::WiFi, false, _("Waiting for the Tether daemon."));
    app::set_route(app::Route::Bluetooth, false, _("Waiting for the Tether daemon."));
    connect_daemon();
    app::show_page(start_page);
    if (!thread.empty()) {
        app::show_page("messages");
        messages_open_thread(thread);
    }

    while (!g.quit && !g_signalled) {
        draw();
        pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {g.daemon_fd, POLLIN, 0}};
        const int nfds = g.daemon_fd >= 0 ? 2 : 1;
        const int ready = poll(fds, nfds, poll_timeout());
        if (ready < 0 && errno != EINTR)
            break;
        if (nfds == 2 && fds[1].revents)
            read_daemon();
        read_keys();
        run_timers();
    }

    endwin();
    close_daemon();
    return 0;
}
