#include <gtest/gtest.h>
#include <tether/bluetooth/ancs/notifications.hpp>
#include <tether/bluetooth/ancs/sequencer.hpp>

using namespace tether::bluetooth::ancs;

namespace {

    struct Harness {
        std::vector<std::vector<uint8_t>> writes;
        std::vector<std::pair<Request, Response>> completions;
        std::vector<std::pair<Request, std::string>> failures;
        bool write_succeeds = true;

        ControlPointSequencer make() {
            return ControlPointSequencer([this](const std::vector<uint8_t>& payload) {
                                             writes.push_back(payload);
                                             return write_succeeds;
                                         },
                                         [this](const Request& request, const Response& response) {
                                             completions.emplace_back(request, response);
                                         },
                                         [this](const Request& request, const std::string& reason) {
                                             failures.emplace_back(request, reason);
                                         });
        }
    };

    std::vector<uint8_t> notification_response(uint32_t uid, const std::string& app) {
        std::vector<uint8_t> out{0x00,
                                 static_cast<uint8_t>(uid & 0xff),
                                 static_cast<uint8_t>((uid >> 8) & 0xff),
                                 static_cast<uint8_t>((uid >> 16) & 0xff),
                                 static_cast<uint8_t>((uid >> 24) & 0xff)};
        out.push_back(0);
        out.push_back(static_cast<uint8_t>(app.size() & 0xff));
        out.push_back(static_cast<uint8_t>((app.size() >> 8) & 0xff));
        out.insert(out.end(), app.begin(), app.end());
        return out;
    }

    SourceEvent event_for(uint32_t uid, EventId event = EventId::Added, uint8_t flags = 0) {
        SourceEvent e;
        e.uid = uid;
        e.event = event;
        e.flags = flags;
        return e;
    }

} // namespace

// --- Request encoding ----------------------------------------------------

TEST(AncsSequencer, EncodesANotificationRequest) {
    Request request = build_notification_request(0x01020304, {NotificationAttributeId::AppIdentifier,
                                                              NotificationAttributeId::Message});

    ASSERT_GE(request.payload.size(), 8u);
    EXPECT_EQ(request.payload[0], 0x00);
    EXPECT_EQ(request.payload[1], 0x04) << "the UID is little-endian";
    EXPECT_EQ(request.payload[4], 0x01);
    EXPECT_EQ(request.payload[5], 0x00) << "AppIdentifier takes no length";
    EXPECT_EQ(request.payload[6], 0x03) << "Message follows immediately";
    // Message is length-capped, and the cap is what Phase 9 correlates against.
    EXPECT_EQ(request.payload[7] | (request.payload[8] << 8), MAX_BODY_LENGTH);
    EXPECT_EQ(request.attribute_ids, (std::vector<uint8_t>{0, 3}));
}

TEST(AncsSequencer, EncodesAnAppRequest) {
    Request request = build_app_request("com.apple.MobileSMS");

    EXPECT_EQ(request.payload[0], 0x01);
    EXPECT_EQ(request.payload[request.payload.size() - 2], 0x00) << "the identifier is NUL-terminated";
    EXPECT_EQ(request.payload.back(), 0x00) << "DisplayName is the requested attribute";
}

TEST(AncsSequencer, EncodesAnActionRequest) {
    Request request = build_action_request(0x11223344, ActionId::Negative);

    EXPECT_EQ(request.payload[0], 0x02);
    EXPECT_EQ(request.payload.back(), 0x01);
    EXPECT_TRUE(request.attribute_ids.empty()) << "an action expects no response";
}

// --- Serialization -------------------------------------------------------

// Responses carry no request identifier, so two in flight cannot be told apart
// and one notification's attributes would be attached to another.
TEST(AncsSequencer, KeepsExactlyOneRequestInFlight) {
    Harness harness;
    auto sequencer = harness.make();

    sequencer.submit(build_notification_request(1, {NotificationAttributeId::AppIdentifier}));
    sequencer.submit(build_notification_request(2, {NotificationAttributeId::AppIdentifier}));
    sequencer.submit(build_notification_request(3, {NotificationAttributeId::AppIdentifier}));
    sequencer.tick(0);

    EXPECT_EQ(harness.writes.size(), 1u);
    EXPECT_EQ(sequencer.queued(), 2u);
    EXPECT_TRUE(sequencer.in_flight());

    auto bytes = notification_response(1, "com.example.one");
    sequencer.on_data_source(bytes.data(), bytes.size(), 1);

    EXPECT_EQ(harness.completions.size(), 1u);
    EXPECT_EQ(harness.completions[0].first.uid, 1u);
    EXPECT_EQ(harness.writes.size(), 2u) << "the next request starts as soon as the first completes";
}

TEST(AncsSequencer, MatchesEachResponseToItsOwnRequest) {
    Harness harness;
    auto sequencer = harness.make();

    sequencer.submit(build_notification_request(11, {NotificationAttributeId::AppIdentifier}));
    sequencer.submit(build_notification_request(22, {NotificationAttributeId::AppIdentifier}));
    sequencer.tick(0);

    auto first = notification_response(11, "com.example.first");
    sequencer.on_data_source(first.data(), first.size(), 1);
    auto second = notification_response(22, "com.example.second");
    sequencer.on_data_source(second.data(), second.size(), 2);

    ASSERT_EQ(harness.completions.size(), 2u);
    EXPECT_EQ(harness.completions[0].first.uid, 11u);
    EXPECT_EQ(harness.completions[0].second.attribute(0), "com.example.first");
    EXPECT_EQ(harness.completions[1].first.uid, 22u);
    EXPECT_EQ(harness.completions[1].second.attribute(0), "com.example.second");
}

// An action gets no Data Source response at all, so waiting for one would stall
// every queued request behind it for the full timeout.
TEST(AncsSequencer, DoesNotWaitForAResponseToAnAction) {
    Harness harness;
    auto sequencer = harness.make();

    sequencer.submit(build_action_request(5, ActionId::Positive));
    sequencer.submit(build_notification_request(6, {NotificationAttributeId::AppIdentifier}));
    sequencer.tick(0);

    EXPECT_EQ(harness.writes.size(), 2u);
    EXPECT_EQ(harness.completions.size(), 1u);
    EXPECT_TRUE(sequencer.in_flight()) << "the notification request is the one now waiting";
}

TEST(AncsSequencer, TimesOutARequestThatIsNeverAnswered) {
    Harness harness;
    auto sequencer = harness.make();

    sequencer.submit(build_notification_request(1, {NotificationAttributeId::AppIdentifier}));
    sequencer.submit(build_notification_request(2, {NotificationAttributeId::AppIdentifier}));
    sequencer.tick(0);
    ASSERT_EQ(harness.writes.size(), 1u);

    sequencer.tick(REQUEST_TIMEOUT_SECONDS - 1);
    EXPECT_TRUE(harness.failures.empty()) << "not yet due";

    sequencer.tick(REQUEST_TIMEOUT_SECONDS);
    ASSERT_EQ(harness.failures.size(), 1u);
    EXPECT_EQ(harness.failures[0].first.uid, 1u);
    EXPECT_EQ(harness.writes.size(), 2u) << "the queue must not stay wedged behind a lost response";
}

TEST(AncsSequencer, FailsARequestWhoseWriteIsRefused) {
    Harness harness;
    harness.write_succeeds = false;
    auto sequencer = harness.make();

    sequencer.submit(build_notification_request(1, {NotificationAttributeId::AppIdentifier}));
    sequencer.tick(0);

    EXPECT_EQ(harness.failures.size(), 1u);
    EXPECT_FALSE(sequencer.in_flight());
}

TEST(AncsSequencer, FailsTheRequestWhenItsResponseIsMalformed) {
    Harness harness;
    auto sequencer = harness.make();

    sequencer.submit(build_notification_request(1, {NotificationAttributeId::AppIdentifier}));
    sequencer.tick(0);

    std::vector<uint8_t> garbage{0x09, 0x09, 0x09};
    sequencer.on_data_source(garbage.data(), garbage.size(), 1);

    EXPECT_EQ(harness.failures.size(), 1u);
    EXPECT_TRUE(harness.completions.empty());
}

TEST(AncsSequencer, DropsEverythingOnReset) {
    Harness harness;
    auto sequencer = harness.make();

    sequencer.submit(build_notification_request(1, {NotificationAttributeId::AppIdentifier}));
    sequencer.submit(build_notification_request(2, {NotificationAttributeId::AppIdentifier}));
    sequencer.tick(0);
    sequencer.reset();

    EXPECT_FALSE(sequencer.in_flight());
    EXPECT_EQ(sequencer.queued(), 0u);

    // A response from the dead session must not be applied to anything.
    auto bytes = notification_response(1, "com.example.one");
    sequencer.on_data_source(bytes.data(), bytes.size(), 1);
    EXPECT_TRUE(harness.completions.empty());
}

TEST(AncsSequencer, BoundsTheQueue) {
    Harness harness;
    auto sequencer = harness.make();

    size_t accepted = 0;
    for (size_t i = 0; i < MAX_QUEUED_REQUESTS + 10; ++i) {
        if (sequencer.submit(build_notification_request(static_cast<uint32_t>(i),
                                                        {NotificationAttributeId::AppIdentifier})))
            ++accepted;
    }
    EXPECT_EQ(accepted, MAX_QUEUED_REQUESTS) << "a notification burst must not grow the daemon without bound";
}

// --- Delivery rules ------------------------------------------------------

// iOS replays its whole backlog whenever the subscription comes up. Delivering
// it would turn every reconnect into a burst of desktop popups.
TEST(AncsNotifications, SkipsPreExistingOnFirstSubscribe) {
    NotificationRegistry registry;
    auto pre_existing = event_for(1, EventId::Added, FlagPreExisting);

    EXPECT_EQ(registry.classify(pre_existing, true), Decision::Ignore);
    EXPECT_EQ(registry.classify(pre_existing, false), Decision::Fetch)
        << "after the initial sync a pre-existing flag is not a reason to hide it";
}

TEST(AncsNotifications, DedupesRepeatedAddedEvents) {
    NotificationRegistry registry;
    auto added = event_for(7);

    EXPECT_EQ(registry.classify(added, false), Decision::Fetch);
    registry.remember(added);
    EXPECT_EQ(registry.classify(added, false), Decision::Ignore) << "a replay of the same UID is not news";
}

// A modification is a genuine update — a call that became a missed call, a
// message that was edited — so dedupe must not swallow it.
TEST(AncsNotifications, NeverSuppressesAModification) {
    NotificationRegistry registry;
    auto added = event_for(7);
    registry.remember(added);

    EXPECT_EQ(registry.classify(event_for(7, EventId::Modified), false), Decision::Fetch);
}

TEST(AncsNotifications, WithdrawsRemovedNotifications) {
    NotificationRegistry registry;
    auto added = event_for(7);
    registry.remember(added);

    EXPECT_EQ(registry.classify(event_for(7, EventId::Removed), false), Decision::Withdraw);

    registry.forget(7);
    EXPECT_EQ(registry.classify(added, false), Decision::Fetch) << "the UID is free to be seen again";
}

// MAP already delivers these with read state that stays in sync, so an ANCS
// popup would be the second copy of the same message.
TEST(AncsNotifications, SuppressesDesktopPopupsForMessages) {
    Notification messages;
    messages.app_id = "com.apple.MobileSMS";
    EXPECT_FALSE(should_show_desktop_popup(messages));

    Notification other;
    other.app_id = "com.example.chat";
    EXPECT_TRUE(should_show_desktop_popup(other));

    Notification silent;
    silent.app_id = "com.example.chat";
    silent.silent = true;
    EXPECT_FALSE(should_show_desktop_popup(silent)) << "the phone was told to stay quiet about this one";
}

TEST(AncsNotifications, KeepsRecentNotificationsNewestFirst) {
    NotificationRegistry registry;
    for (uint32_t i = 1; i <= 3; ++i) {
        Notification n;
        n.uid = i;
        n.app_id = "com.example.app";
        registry.store(n);
    }

    auto recent = registry.recent();
    ASSERT_EQ(recent.size(), 3u);
    EXPECT_EQ(recent[0].uid, 3u);
    EXPECT_EQ(recent[2].uid, 1u);
}

TEST(AncsNotifications, ClearingForgetsEverything) {
    NotificationRegistry registry;
    Notification n;
    n.uid = 1;
    registry.store(n);
    registry.remember(event_for(1));

    registry.clear();
    EXPECT_EQ(registry.size(), 0u);
    EXPECT_EQ(registry.classify(event_for(1), false), Decision::Fetch);
}
