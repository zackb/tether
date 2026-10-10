#include "message_format.hpp"

#include <ctime>
#include <glib.h>
#include <regex>
#include <tether/i18n.hpp>

namespace tether::ui {

    namespace {

        std::string escape(const std::string& text) {
            gchar* escaped = g_markup_escape_text(text.c_str(), -1);
            std::string out = escaped ? escaped : "";
            g_free(escaped);
            return out;
        }

        size_t trim_trailing(const std::string& url) {
            size_t end = url.size();
            while (end > 0) {
                const char c = url[end - 1];
                if (c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':')
                    --end;
                else if (c == ')' && url.find('(', 0) >= end)
                    --end;
                else
                    break;
            }
            return end;
        }

        std::string localized_day(const std::tm& tm, const char* format) {
            char buffer[64];
            std::strftime(buffer, sizeof(buffer), format, &tm);
            return buffer;
        }

        // Whole days apart in local time, so "yesterday" means the calendar day
        // before rather than 24 hours ago.
        int days_between(const std::tm& then, const std::tm& now) {
            std::tm a = then;
            std::tm b = now;
            a.tm_hour = a.tm_min = a.tm_sec = 0;
            b.tm_hour = b.tm_min = b.tm_sec = 0;
            a.tm_isdst = b.tm_isdst = -1;
            const std::time_t start = std::mktime(&a);
            const std::time_t end = std::mktime(&b);
            if (start == static_cast<std::time_t>(-1) || end == static_cast<std::time_t>(-1))
                return 0;
            return static_cast<int>((end - start) / 86400);
        }

    } // namespace

    std::string linkify_markup(const std::string& body) {
        static const std::regex pattern(R"((https?://|www\.)[^\s<>"']+)", std::regex::icase);

        std::string out;
        auto begin = std::sregex_iterator(body.begin(), body.end(), pattern);
        auto end = std::sregex_iterator();
        size_t cursor = 0;

        for (auto it = begin; it != end; ++it) {
            const std::smatch& match = *it;
            std::string url = match.str();
            const size_t keep = trim_trailing(url);
            if (keep == 0)
                continue;
            url.resize(keep);

            const size_t at = static_cast<size_t>(match.position());
            out += escape(body.substr(cursor, at - cursor));
            // bare www. host is not a URI
            const std::string href = (url.compare(0, 4, "www.") == 0) ? "http://" + url : url;
            out += "<a href=\"" + escape(href) + "\">" + escape(url) + "</a>";
            cursor = at + keep;
        }
        out += escape(body.substr(cursor));
        return out;
    }

    std::string format_thread_time(int64_t epoch, int64_t now) {
        if (epoch <= 0)
            return "";

        std::time_t when = static_cast<std::time_t>(epoch);
        std::time_t today = static_cast<std::time_t>(now);
        std::tm when_tm{};
        std::tm today_tm{};
        localtime_r(&when, &when_tm);
        localtime_r(&today, &today_tm);

        const int days = days_between(when_tm, today_tm);
        if (days <= 0)
            // TRANSLATORS: strftime clock time on today's conversations, e.g. "%I:%M %p" for 12-hour.
            // xgettext:no-c-format
            return localized_day(when_tm, _("%H:%M"));
        if (days == 1)
            return _("Yesterday");
        if (days < 7)
            return localized_day(when_tm, "%a");
        // TRANSLATORS: strftime short date for older conversations; reorder to the local
        // convention, e.g. "%-d %b".
        // xgettext:no-c-format
        return localized_day(when_tm, _("%b %-d"));
    }

    std::string format_day_heading(int64_t epoch, int64_t now) {
        if (epoch <= 0)
            return "";

        std::time_t when = static_cast<std::time_t>(epoch);
        std::time_t today = static_cast<std::time_t>(now);
        std::tm when_tm{};
        std::tm today_tm{};
        localtime_r(&when, &when_tm);
        localtime_r(&today, &today_tm);

        const int days = days_between(when_tm, today_tm);
        if (days <= 0)
            return _("Today");
        if (days == 1)
            return _("Yesterday");
        if (days < 7)
            return localized_day(when_tm, "%A");
        // TRANSLATORS: strftime date heading above older messages, e.g. "%A %-d %B".
        // xgettext:no-c-format
        return localized_day(when_tm, _("%A, %b %-d"));
    }

    bool same_local_day(int64_t a, int64_t b) {
        std::time_t first = static_cast<std::time_t>(a);
        std::time_t second = static_cast<std::time_t>(b);
        std::tm first_tm{};
        std::tm second_tm{};
        localtime_r(&first, &first_tm);
        localtime_r(&second, &second_tm);
        return first_tm.tm_year == second_tm.tm_year && first_tm.tm_yday == second_tm.tm_yday;
    }

} // namespace tether::ui
