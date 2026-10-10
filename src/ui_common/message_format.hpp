#pragma once

#include <cstdint>
#include <string>

namespace tether::ui {

    // Pango markup for a message body, with any URLs turned into links. The
    // whole body is markup-escaped first, so the result is safe.
    std::string linkify_markup(const std::string& body);

    // How old a conversation's last message is.
    std::string format_thread_time(int64_t epoch, int64_t now);

    // Heading for a day's worth of messages: "Today", "Yesterday", or the date.
    std::string format_day_heading(int64_t epoch, int64_t now);

    // Whether two instants fall on the same local calendar day.
    bool same_local_day(int64_t a, int64_t b);

} // namespace tether::ui
