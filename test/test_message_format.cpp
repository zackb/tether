#include "../src/ui_common/message_format.hpp"

#include <glib.h>
#include <gtest/gtest.h>

using tether::ui::format_day_heading;
using tether::ui::format_thread_time;
using tether::ui::linkify_markup;
using tether::ui::same_local_day;

namespace {

    // Fixed local time so the day boundaries are the ones under test rather than
    // whatever the build host happens to be set to.
    struct FixedZone : ::testing::Test {
        void SetUp() override {
            setenv("TZ", "UTC", 1);
            tzset();
        }
    };

    // 2026-08-25 12:00:00 UTC, a Tuesday.
    constexpr int64_t NOW = 1787659200;
    constexpr int64_t DAY = 86400;

} // namespace

TEST(LinkifyMarkup, PlainTextIsEscapedAndUnlinked) {
    EXPECT_EQ(linkify_markup("a < b & c"), "a &lt; b &amp; c");
}

TEST(LinkifyMarkup, BareUrlBecomesALink) {
    EXPECT_EQ(linkify_markup("see https://example.com now"),
              "see <a href=\"https://example.com\">https://example.com</a> now");
}

TEST(LinkifyMarkup, TrailingSentencePunctuationStaysOutsideTheLink) {
    EXPECT_EQ(linkify_markup("go to https://example.com/x."),
              "go to <a href=\"https://example.com/x\">https://example.com/x</a>.");
}

TEST(LinkifyMarkup, ClosingParenBelongingToTheUrlIsKept) {
    EXPECT_EQ(linkify_markup("https://example.com/a_(b)"),
              "<a href=\"https://example.com/a_(b)\">https://example.com/a_(b)</a>");
}

TEST(LinkifyMarkup, WwwHostGetsAScheme) {
    EXPECT_EQ(linkify_markup("www.example.com"), "<a href=\"http://www.example.com\">www.example.com</a>");
}

TEST(LinkifyMarkup, QueryStringIsEscapedInBothHrefAndText) {
    EXPECT_EQ(linkify_markup("https://example.com/?a=1&b=2"),
              "<a href=\"https://example.com/?a=1&amp;b=2\">https://example.com/?a=1&amp;b=2</a>");
}

TEST(LinkifyMarkup, MultipleUrlsInOneBody) {
    EXPECT_EQ(linkify_markup("http://a.test and http://b.test"),
              "<a href=\"http://a.test\">http://a.test</a> and <a href=\"http://b.test\">http://b.test</a>");
}

TEST_F(FixedZone, TodayIsAClockTime) { EXPECT_EQ(format_thread_time(NOW - 3600, NOW), "11:00"); }

TEST_F(FixedZone, EarlierTodayStaysAClockTime) {
    // 00:30 the same calendar day, which is more than 24h from nothing but is
    // still "today" and must not read as Yesterday.
    EXPECT_EQ(format_thread_time(NOW - (11 * 3600) - 1800, NOW), "00:30");
}

TEST_F(FixedZone, PreviousCalendarDayIsYesterday) { EXPECT_EQ(format_thread_time(NOW - DAY, NOW), "Yesterday"); }

TEST_F(FixedZone, WithinTheWeekIsAWeekday) {
    EXPECT_EQ(format_thread_time(NOW - (3 * DAY), NOW), "Sat");
    EXPECT_EQ(format_thread_time(NOW - (6 * DAY), NOW), "Wed");
}

TEST_F(FixedZone, BeyondAWeekIsADate) { EXPECT_EQ(format_thread_time(NOW - (7 * DAY), NOW), "Aug 18"); }

TEST_F(FixedZone, MissingTimestampIsEmpty) { EXPECT_EQ(format_thread_time(0, NOW), ""); }

TEST_F(FixedZone, DayHeadingNamesTodayAndYesterday) {
    EXPECT_EQ(format_day_heading(NOW, NOW), "Today");
    EXPECT_EQ(format_day_heading(NOW - DAY, NOW), "Yesterday");
}

TEST_F(FixedZone, DayHeadingWithinTheWeekIsTheWeekdayName) {
    EXPECT_EQ(format_day_heading(NOW - (3 * DAY), NOW), "Saturday");
}

TEST_F(FixedZone, DayHeadingBeyondAWeekCarriesTheDate) {
    EXPECT_EQ(format_day_heading(NOW - (7 * DAY), NOW), "Tuesday, Aug 18");
}

TEST_F(FixedZone, SameLocalDayIgnoresTimeOfDayButNotTheDateBoundary) {
    EXPECT_TRUE(same_local_day(NOW, NOW - (11 * 3600)));
    EXPECT_FALSE(same_local_day(NOW, NOW - (13 * 3600)));
}

// Whatever a sender types, the result has to be markup GTK will accept -
// invalid markup renders the bubble empty instead of showing the message.
TEST(LinkifyMarkup, AlwaysProducesParseableMarkup) {
    const char* bodies[] = {
        "",
        "plain text",
        "<b>not bold</b>",
        "5 < 6 && 7 > 3",
        "quote \" and apostrophe '",
        "<a href='http://evil'>click</a>",
        "https://example.com/<script>",
        "http://example.com/?q=<>&x=\"y\"",
        "&amp; already escaped",
        "www.example.com</a><b>",
        "http://",
        "www.",
        "trailing )))",
        "https://example.com/a(b)c).",
        "ünïcödé https://exämple.com/ß",
        "\xf0\x9f\x98\x80 emoji https://example.com",
        "newline\nhttp://example.com\ttab",
    };

    for (const char* body : bodies) {
        const std::string markup = linkify_markup(body);
        // The same well-formedness gtk_label_set_markup requires, checked with
        // a root element the way Pango wraps it.
        const std::string document = "<markup>" + markup + "</markup>";
        GMarkupParser parser{};
        GMarkupParseContext* context = g_markup_parse_context_new(&parser, GMarkupParseFlags(0), nullptr, nullptr);
        GError* error = nullptr;
        const gboolean ok = g_markup_parse_context_parse(context, document.c_str(), -1, &error) &&
                            g_markup_parse_context_end_parse(context, &error);
        EXPECT_TRUE(ok) << "body: " << body << "\nmarkup: " << markup
                        << "\nerror: " << (error ? error->message : "?");
        g_clear_error(&error);
        g_markup_parse_context_free(context);
    }
}
