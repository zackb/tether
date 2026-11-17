#include <gtest/gtest.h>
#include <tether/bluetooth/catch_up.hpp>

using namespace tether::bluetooth;

namespace {

    Message message(const std::string& handle, const std::string& thread_key, int64_t timestamp) {
        Message m;
        m.handle = handle;
        m.thread_key = thread_key;
        m.timestamp = timestamp;
        return m;
    }

    std::vector<std::string> handles(const std::vector<Message>& messages) {
        std::vector<std::string> out;
        for (const auto& m : messages)
            out.push_back(m.handle);
        return out;
    }

} // namespace

TEST(CatchUp, NothingMissedAnnouncesNothing) {
    const auto plan = plan_catch_up({}, 0);
    ASSERT_TRUE(std::holds_alternative<CatchUpEach>(plan));
    EXPECT_TRUE(std::get<CatchUpEach>(plan).messages.empty());
}

TEST(CatchUp, AFewMessagesPopUpEachOldestFirst) {
    const auto plan = plan_catch_up(
        {
            message("c", "tel:+15035550103", 300),
            message("a", "tel:+15035550101", 100),
            message("b", "tel:+15035550102", 200),
        },
        0);
    ASSERT_TRUE(std::holds_alternative<CatchUpEach>(plan));
    EXPECT_EQ(handles(std::get<CatchUpEach>(plan).messages), (std::vector<std::string>{"a", "b", "c"}));
}

TEST(CatchUp, ExactlyTheLimitStillPopsUpEach) {
    std::vector<Message> missed;
    for (size_t i = 0; i < CATCH_UP_POPUP_MAX; ++i)
        missed.push_back(message(std::to_string(i), "tel:+15035550101", static_cast<int64_t>(i)));
    EXPECT_TRUE(std::holds_alternative<CatchUpEach>(plan_catch_up(missed, -1)));
}

TEST(CatchUp, OverTheLimitBecomesOneSummary) {
    std::vector<Message> missed;
    for (size_t i = 0; i <= CATCH_UP_POPUP_MAX; ++i)
        missed.push_back(message(std::to_string(i), "tel:+15035550101", static_cast<int64_t>(i)));
    const auto plan = plan_catch_up(missed, -1);
    ASSERT_TRUE(std::holds_alternative<CatchUpSummary>(plan));
    EXPECT_EQ(std::get<CatchUpSummary>(plan).count, CATCH_UP_POPUP_MAX + 1);
}

TEST(CatchUp, SummaryNamesEachConversationOnceNewestFirst) {
    const auto plan = plan_catch_up(
        {
            message("1", "tel:+15035550101", 100),
            message("2", "tel:+15035550102", 200),
            message("3", "tel:+15035550101", 300),
            message("4", "group:name:hiking", 400),
            message("5", "tel:+15035550102", 500),
        },
        0,
        2);
    ASSERT_TRUE(std::holds_alternative<CatchUpSummary>(plan));
    const auto& summary = std::get<CatchUpSummary>(plan);
    EXPECT_EQ(summary.count, 5u);
    // Each thread is represented by its newest message.
    EXPECT_EQ(handles(summary.threads), (std::vector<std::string>{"5", "4", "3"}));
}

TEST(CatchUp, OnlyMessagesNewerThanTheKnownHistoryCount) {
    const auto plan = plan_catch_up(
        {
            message("old", "tel:+15035550101", 100),
            message("same", "tel:+15035550101", 200),
            message("new", "tel:+15035550102", 300),
        },
        200);
    ASSERT_TRUE(std::holds_alternative<CatchUpEach>(plan));
    EXPECT_EQ(handles(std::get<CatchUpEach>(plan).messages), (std::vector<std::string>{"new"}));
}

// A first sync, or a journal that could not be read, has no history to compare
// against, and the phone's whole unread inbox must not be announced as new.
TEST(CatchUp, NoKnownHistoryAnnouncesNothing) {
    const auto plan = plan_catch_up({message("a", "tel:+15035550101", 100)}, std::nullopt);
    ASSERT_TRUE(std::holds_alternative<CatchUpEach>(plan));
    EXPECT_TRUE(std::get<CatchUpEach>(plan).messages.empty());
}

TEST(CatchUp, TheThresholdCountsOnlyNewMessages) {
    std::vector<Message> missed;
    for (int i = 0; i < 10; ++i)
        missed.push_back(message(std::to_string(i), "tel:+15035550101", i));
    // Seven are already known, which leaves exactly the limit.
    EXPECT_TRUE(std::holds_alternative<CatchUpEach>(plan_catch_up(missed, 6)));
}
