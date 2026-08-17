#include <gtest/gtest.h>
#include <tether/bluetooth/groups.hpp>

using namespace tether::bluetooth;

namespace {

    ContactStore contacts_with(const std::string& vcards) {
        ContactStore store;
        store.set(parse_vcards(vcards));
        return store;
    }

    MessageNotification notification(const std::string& sender,
                                     const std::string& subtitle,
                                     const std::string& body,
                                     int64_t received) {
        return MessageNotification{sender, subtitle, body, received};
    }

} // namespace

// --- Parsing the side channel -------------------------------------------

TEST(Groups, ParsesAnUnnamedGroupSubtitle) {
    GroupInfo info;
    ASSERT_TRUE(parse_group_notification("Alice", "To you & Bob", info));

    EXPECT_TRUE(info.is_group);
    EXPECT_FALSE(info.named);
    EXPECT_EQ(info.sender, "Alice");
    ASSERT_EQ(info.participants.size(), 1u);
    EXPECT_EQ(info.participants[0], "Bob") << "the user is a member but never a reply recipient";
}

TEST(Groups, ParsesLongerParticipantLists) {
    GroupInfo info;
    ASSERT_TRUE(parse_group_notification("Alice", "To you, Bob & Carol", info));
    ASSERT_EQ(info.participants.size(), 2u);
    EXPECT_EQ(info.participants[0], "Bob");
    EXPECT_EQ(info.participants[1], "Carol");

    ASSERT_TRUE(parse_group_notification("Alice", "To Bob, Carol & you", info));
    ASSERT_EQ(info.participants.size(), 2u);
}

TEST(Groups, ParsesANamedGroup) {
    GroupInfo info;
    ASSERT_TRUE(parse_group_notification("Alice", "Weekend Trip", info));

    EXPECT_TRUE(info.named);
    EXPECT_EQ(info.name, "Weekend Trip");
    EXPECT_TRUE(info.participants.empty()) << "a group name carries no member list";
}

TEST(Groups, TreatsAOneToOneNotificationAsNotAGroup) {
    GroupInfo info;
    EXPECT_FALSE(parse_group_notification("Alice", "", info));
    EXPECT_FALSE(parse_group_notification("Alice", "To you", info)) << "only the user means no other members";
}

// iOS supplies a name but no conversation identifier, so two groups sharing a
// name collapse into one local thread. That is a documented limitation, and the
// key has to make it visible rather than pretend otherwise.
TEST(Groups, NamedGroupsWithTheSameNameShareAKey) {
    GroupInfo first;
    GroupInfo second;
    ASSERT_TRUE(parse_group_notification("Alice", "Weekend Trip", first));
    ASSERT_TRUE(parse_group_notification("Bob", "weekend   trip", second));

    EXPECT_EQ(group_thread_key(first), group_thread_key(second));
}

TEST(Groups, UnnamedGroupKeyDoesNotDependOnMemberOrder) {
    GroupInfo first;
    GroupInfo second;
    ASSERT_TRUE(parse_group_notification("Alice", "To you, Bob & Carol", first));
    ASSERT_TRUE(parse_group_notification("Carol", "To Bob, you & Alice", second));

    EXPECT_NE(group_thread_key(first), group_thread_key(second))
        << "different member sets are different conversations";

    GroupInfo reordered;
    ASSERT_TRUE(parse_group_notification("Alice", "To Carol, you & Bob", reordered));
    EXPECT_EQ(group_thread_key(first), group_thread_key(reordered));
}

// --- Correlation ---------------------------------------------------------

TEST(Groups, CorrelatesAMessageToItsNotification) {
    GroupCorrelator correlator;
    correlator.observe(notification("Alice", "To you & Bob", "dinner at seven", 100));

    GroupInfo info;
    EXPECT_EQ(correlator.correlate("dinner at seven", 105, info), Correlation::Matched);
    EXPECT_EQ(info.sender, "Alice");
}

// ANCS bodies are capped at what was requested, so a longer MAP body matches
// the notification as a prefix.
TEST(Groups, MatchesALongerBodyByItsPrefix) {
    GroupCorrelator correlator;
    correlator.observe(notification("Alice", "To you & Bob", "the first part of a long", 100));

    GroupInfo info;
    EXPECT_EQ(correlator.correlate("the first part of a long message that kept going", 101, info),
              Correlation::Matched);
}

TEST(Groups, IgnoresNotificationsOutsideTheWindow) {
    GroupCorrelator correlator;
    correlator.observe(notification("Alice", "To you & Bob", "hello", 100));

    GroupInfo info;
    EXPECT_EQ(correlator.correlate("hello", 100 + GROUP_CORRELATION_WINDOW_SECONDS + 1, info), Correlation::NoMatch);
}

// Two notifications carrying the same text cannot be told apart, and guessing
// would aim a reply at the wrong people.
TEST(Groups, RefusesAmbiguousRepeatedBodies) {
    GroupCorrelator correlator;
    correlator.observe(notification("Alice", "To you & Bob", "ok", 100));
    correlator.observe(notification("Carol", "Weekend Trip", "ok", 101));

    GroupInfo info;
    EXPECT_EQ(correlator.correlate("ok", 102, info), Correlation::Ambiguous);
}

TEST(Groups, ReportsNoMatchForAnUnrelatedMessage) {
    GroupCorrelator correlator;
    correlator.observe(notification("Alice", "To you & Bob", "hello", 100));

    GroupInfo info;
    EXPECT_EQ(correlator.correlate("something else entirely", 101, info), Correlation::NoMatch);
}

TEST(Groups, ExpiryDropsStaleNotifications) {
    GroupCorrelator correlator;
    correlator.observe(notification("Alice", "To you & Bob", "hello", 100));
    correlator.expire(100 + GROUP_CORRELATION_WINDOW_SECONDS + 1);
    EXPECT_EQ(correlator.size(), 0u);
}

// --- Reply routing -------------------------------------------------------

TEST(Groups, ResolvesAnUnnamedGroupWhenEveryNameIsUnambiguous) {
    auto contacts = contacts_with(
        "BEGIN:VCARD\nFN:Bob\nTEL:+15550000002\nEND:VCARD\n"
        "BEGIN:VCARD\nFN:Carol\nTEL:+15550000003\nEND:VCARD\n");

    GroupInfo info;
    ASSERT_TRUE(parse_group_notification("Alice", "To you, Bob & Carol", info));

    std::vector<Recipient> recipients;
    std::string reason;
    EXPECT_EQ(resolve_group_recipients(info, group_thread_key(info), contacts, GroupRoster{}, true, recipients, reason),
              ReplyEligibility::Allowed)
        << reason;
    EXPECT_EQ(recipients.size(), 2u);
}

// A name matching two contacts must never be resolved by picking one: the
// message would go to a stranger.
TEST(Groups, RefusesAnAmbiguousParticipantName) {
    auto contacts = contacts_with(
        "BEGIN:VCARD\nFN:Bob\nTEL:+15550000002\nEND:VCARD\n"
        "BEGIN:VCARD\nFN:Bob\nTEL:+15559999999\nEND:VCARD\n");

    GroupInfo info;
    ASSERT_TRUE(parse_group_notification("Alice", "To you & Bob", info));

    std::vector<Recipient> recipients;
    std::string reason;
    EXPECT_EQ(resolve_group_recipients(info, group_thread_key(info), contacts, GroupRoster{}, true, recipients, reason),
              ReplyEligibility::Unresolved);
    EXPECT_TRUE(recipients.empty()) << "a refused resolution must leave no partial recipient set";
}

TEST(Groups, RefusesAnUnknownParticipantName) {
    auto contacts = contacts_with("BEGIN:VCARD\nFN:Bob\nTEL:+15550000002\nEND:VCARD\n");

    GroupInfo info;
    ASSERT_TRUE(parse_group_notification("Alice", "To you, Bob & Dave", info));

    std::vector<Recipient> recipients;
    std::string reason;
    EXPECT_EQ(resolve_group_recipients(info, group_thread_key(info), contacts, GroupRoster{}, true, recipients, reason),
              ReplyEligibility::Unresolved);
    EXPECT_TRUE(recipients.empty());
}

// A named group identifies the conversation but gives no roster, so it stays
// read-only until the user provides one.
TEST(Groups, NamedGroupsAreReadOnlyWithoutARoster) {
    auto contacts = contacts_with("BEGIN:VCARD\nFN:Bob\nTEL:+15550000002\nEND:VCARD\n");

    GroupInfo info;
    ASSERT_TRUE(parse_group_notification("Alice", "Weekend Trip", info));

    std::vector<Recipient> recipients;
    std::string reason;
    EXPECT_EQ(resolve_group_recipients(info, group_thread_key(info), contacts, GroupRoster{}, true, recipients, reason),
              ReplyEligibility::NeedsRoster);
    EXPECT_TRUE(recipients.empty());
}

TEST(Groups, ARosterMakesANamedGroupRepliable) {
    GroupInfo info;
    ASSERT_TRUE(parse_group_notification("Alice", "Weekend Trip", info));
    const std::string key = group_thread_key(info);

    GroupRoster roster;
    roster.set(key, {"tel:+15550000002", "email:carol@example.com"});

    std::vector<Recipient> recipients;
    std::string reason;
    EXPECT_EQ(resolve_group_recipients(info, key, ContactStore{}, roster, true, recipients, reason),
              ReplyEligibility::Allowed)
        << reason;
    ASSERT_EQ(recipients.size(), 2u);
    EXPECT_EQ(recipients[0].kind, RecipientKind::Tel);
    EXPECT_EQ(recipients[1].kind, RecipientKind::Email);
}

// The roster is user-supplied text, and it lands in a bMessage as structure.
TEST(Groups, RefusesARosterEntryThatCouldInjectARecipient) {
    GroupInfo info;
    ASSERT_TRUE(parse_group_notification("Alice", "Weekend Trip", info));
    const std::string key = group_thread_key(info);

    GroupRoster roster;
    roster.set(key, {"tel:+1555\r\nTEL:+15559999999"});

    std::vector<Recipient> recipients;
    std::string reason;
    EXPECT_EQ(resolve_group_recipients(info, key, ContactStore{}, roster, true, recipients, reason),
              ReplyEligibility::Unresolved);
    EXPECT_TRUE(recipients.empty());
}

// Group membership lives only in memory, so after a restart a group thread has
// no known members until the phone sends another message. Replying then would
// be routing to nobody, and must not silently succeed.
TEST(Groups, AnUnidentifiedThreadCannotBeRepliedTo) {
    std::vector<Recipient> recipients;
    std::string reason;
    EXPECT_EQ(resolve_group_recipients(
                  GroupInfo{}, "group:name:weekend-trip", ContactStore{}, GroupRoster{}, true, recipients, reason),
              ReplyEligibility::NeedsRoster);
    EXPECT_TRUE(recipients.empty());
    EXPECT_FALSE(reason.empty());
}

TEST(Groups, RepliesAreOffUntilEnabled) {
    GroupInfo info;
    ASSERT_TRUE(parse_group_notification("Alice", "To you & Bob", info));

    std::vector<Recipient> recipients;
    std::string reason;
    EXPECT_EQ(
        resolve_group_recipients(info, group_thread_key(info), ContactStore{}, GroupRoster{}, false, recipients, reason),
        ReplyEligibility::Disabled);
}

// iOS reports nothing when a member is silently added, so an unfamiliar sender
// is the only evidence that a roster has gone stale.
TEST(Groups, AnUnknownSenderInvalidatesTheRoute) {
    std::vector<Recipient> recipients{{RecipientKind::Tel, "+15550000002"},
                                      {RecipientKind::Tel, "+15550000003"}};

    EXPECT_FALSE(sender_invalidates_route("tel:+15550000002", recipients));
    EXPECT_TRUE(sender_invalidates_route("tel:+15557654321", recipients));
    EXPECT_TRUE(sender_invalidates_route("email:stranger@example.com", recipients));
}

TEST(Groups, RosterSurvivesARoundTrip) {
    GroupRoster roster;
    roster.set("group:name:weekend-trip", {"tel:+15550000002"});

    GroupRoster restored = GroupRoster::deserialize(roster.serialize());
    const auto* addresses = restored.find("group:name:weekend-trip");
    ASSERT_NE(addresses, nullptr);
    ASSERT_EQ(addresses->size(), 1u);
    EXPECT_EQ((*addresses)[0], "tel:+15550000002");
}

// A corrupt roster must leave groups read-only rather than routing a reply to
// whatever happened to survive parsing.
TEST(Groups, CorruptRosterResolvesToNothing) {
    EXPECT_TRUE(GroupRoster::deserialize("{not json").empty());
    EXPECT_TRUE(GroupRoster::deserialize("[]").empty());
    EXPECT_TRUE(GroupRoster::deserialize("").empty());
}
