#include <gtest/gtest.h>
#include <tether/bluetooth/contacts.hpp>
#include <tether/bluetooth/messages.hpp>
#include <tether/bluetooth/vcard.hpp>

using namespace tether::bluetooth;

TEST(VCard, ParsesNameTelAndEmail) {
    auto cards = parse_vcards(
        "BEGIN:VCARD\r\n"
        "VERSION:3.0\r\n"
        "FN:Ada Lovelace\r\n"
        "TEL;TYPE=CELL:+15551234567\r\n"
        "EMAIL;TYPE=INTERNET:ada@example.com\r\n"
        "END:VCARD\r\n");

    ASSERT_EQ(cards.size(), 1u);
    EXPECT_EQ(cards[0].name, "Ada Lovelace");
    ASSERT_EQ(cards[0].tels.size(), 1u);
    EXPECT_EQ(cards[0].tels[0], "+15551234567");
    ASSERT_EQ(cards[0].emails.size(), 1u);
    EXPECT_EQ(cards[0].emails[0], "ada@example.com");
}

// A contact reachable at several numbers is the normal case, not an edge case:
// keeping only the first would silently lose the address a thread is keyed on.
TEST(VCard, KeepsEveryTelAndEmail) {
    auto cards = parse_vcards(
        "BEGIN:VCARD\n"
        "FN:Grace Hopper\n"
        "TEL;TYPE=CELL:+15550000001\n"
        "TEL;TYPE=HOME:+15550000002\n"
        "EMAIL:grace@example.com\n"
        "EMAIL:grace@navy.example\n"
        "END:VCARD\n");

    ASSERT_EQ(cards.size(), 1u);
    EXPECT_EQ(cards[0].tels.size(), 2u);
    EXPECT_EQ(cards[0].emails.size(), 2u);
}

TEST(VCard, ParsesMultipleCards) {
    auto cards = parse_vcards(
        "BEGIN:VCARD\nFN:One\nTEL:+1555000001\nEND:VCARD\n"
        "BEGIN:VCARD\nFN:Two\nTEL:+1555000002\nEND:VCARD\n");

    ASSERT_EQ(cards.size(), 2u);
    EXPECT_EQ(cards[0].name, "One");
    EXPECT_EQ(cards[1].name, "Two");
}

// RFC 2425 folding splits long values across lines; without unfolding, a long
// name or address is silently truncated at the fold.
TEST(VCard, UnfoldsContinuationLines) {
    auto cards = parse_vcards(
        "BEGIN:VCARD\r\n"
        "FN:A Very Long Contact\r\n"
        "  Name Indeed\r\n"
        "TEL:+1555\r\n"
        " 0000001\r\n"
        "END:VCARD\r\n");

    ASSERT_EQ(cards.size(), 1u);
    EXPECT_EQ(cards[0].name, "A Very Long Contact Name Indeed");
    ASSERT_EQ(cards[0].tels.size(), 1u);
    EXPECT_EQ(cards[0].tels[0], "+15550000001");
}

TEST(VCard, FallsBackToStructuredName) {
    auto cards = parse_vcards("BEGIN:VCARD\nN:Lovelace;Ada;;;\nTEL:+1555\nEND:VCARD\n");

    ASSERT_EQ(cards.size(), 1u);
    EXPECT_EQ(cards[0].name, "Ada Lovelace");
}

TEST(VCard, PrefersFormattedNameOverStructured) {
    auto cards = parse_vcards("BEGIN:VCARD\nN:Lovelace;Ada;;;\nFN:Countess Ada\nTEL:+1555\nEND:VCARD\n");

    ASSERT_EQ(cards.size(), 1u);
    EXPECT_EQ(cards[0].name, "Countess Ada");
}

TEST(VCard, UnescapesValues) {
    auto cards = parse_vcards("BEGIN:VCARD\nFN:Smith\\, Alice\nTEL:+1555\nEND:VCARD\n");

    ASSERT_EQ(cards.size(), 1u);
    EXPECT_EQ(cards[0].name, "Smith, Alice");
}

TEST(VCard, IgnoresMalformedInput) {
    EXPECT_TRUE(parse_vcards("").empty());
    EXPECT_TRUE(parse_vcards("not a vcard at all").empty());
    EXPECT_TRUE(parse_vcards("BEGIN:VCARD\nEND:VCARD\n").empty()) << "an empty card carries nothing to store";
}

// A card cut off mid-transfer should still yield what it had rather than
// swallowing the card that follows it.
TEST(VCard, RecoversFromUnterminatedCard) {
    auto cards = parse_vcards(
        "BEGIN:VCARD\nFN:First\nTEL:+1555000001\n"
        "BEGIN:VCARD\nFN:Second\nTEL:+1555000002\nEND:VCARD\n");

    ASSERT_EQ(cards.size(), 2u);
    EXPECT_EQ(cards[0].name, "First");
    EXPECT_EQ(cards[1].name, "Second");
}

TEST(ContactStore, ResolvesNamesByTelAndEmail) {
    ContactStore store;
    store.set(parse_vcards("BEGIN:VCARD\nFN:Ada\nTEL:+1 (555) 123-4567\nEMAIL:Ada@Example.COM\nEND:VCARD\n"));

    EXPECT_EQ(store.name_for("tel:+15551234567"), "Ada");
    EXPECT_EQ(store.name_for("email:ada@example.com"), "Ada");
    EXPECT_EQ(store.name_for("tel:+15559999999"), "");
}

// Tether never invents a country code, so the two forms stay distinct keys. A
// contact stored one way must not silently label the other.
TEST(ContactStore, DoesNotEquateNationalAndE164Forms) {
    ContactStore store;
    store.set(parse_vcards("BEGIN:VCARD\nFN:Ada\nTEL:+15551234567\nEND:VCARD\n"));

    EXPECT_EQ(store.name_for("tel:+15551234567"), "Ada");
    EXPECT_EQ(store.name_for("tel:5551234567"), "");
}

TEST(ContactStore, SurvivesJsonRoundTrip) {
    ContactStore store;
    store.set(parse_vcards("BEGIN:VCARD\nFN:Ada\nTEL:+15551234567\nEMAIL:ada@example.com\nEND:VCARD\n"));

    ContactStore restored = deserialize_contacts(serialize_contacts(store));
    EXPECT_EQ(restored.size(), store.size());
    EXPECT_EQ(restored.name_for("tel:+15551234567"), "Ada");
    EXPECT_EQ(restored.name_for("email:ada@example.com"), "Ada");
}

TEST(ContactStore, CorruptCacheYieldsEmptyStore) {
    EXPECT_EQ(deserialize_contacts("{not json").size(), 0u);
    EXPECT_EQ(deserialize_contacts("").size(), 0u);
}

// A contact with no name still contributes nothing to display, but must not
// index an empty label over a valid one.
TEST(ContactStore, SkipsUnnamedContacts) {
    ContactStore store;
    store.set(parse_vcards("BEGIN:VCARD\nTEL:+15551234567\nEND:VCARD\n"));

    EXPECT_EQ(store.name_for("tel:+15551234567"), "");
}

// A re-listing after reconnect carries the same message under a new object path.
// The store must adopt the new path, or acting on the message uses a dead one.
TEST(MessageStore, RefreshesObjectPathOnRelist) {
    MessageStore store;
    Message first;
    first.handle = "42";
    first.thread_key = "tel:+15551234567";
    first.object_path = "/org/bluez/obex/client/session5/message42";
    EXPECT_TRUE(store.add(first));

    Message again = first;
    again.object_path = "/org/bluez/obex/client/session11/message42";
    EXPECT_FALSE(store.add(again)) << "same handle is still a duplicate";

    ASSERT_NE(store.find("42"), nullptr);
    EXPECT_EQ(store.find("42")->object_path, "/org/bluez/obex/client/session11/message42");
}
