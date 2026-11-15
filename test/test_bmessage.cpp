#include <gtest/gtest.h>
#include <tether/bluetooth/bmessage.hpp>
#include <tether/bluetooth/map_session.hpp>
#include <tether/bluetooth/messages.hpp>

using namespace tether::bluetooth;

namespace {

    // Shape observed from the iPhone: originator vCard outside BENV, recipient
    // inside it, UTF-8 body in BBODY/MSG, TYPE always SMS_GSM.
    constexpr const char* INCOMING_TEL =
        "BEGIN:BMSG\r\n"
        "VERSION:1.0\r\n"
        "STATUS:UNREAD\r\n"
        "TYPE:SMS_GSM\r\n"
        "FOLDER:telecom/msg/inbox\r\n"
        "BEGIN:VCARD\r\n"
        "VERSION:2.1\r\n"
        "N:Doe;Jane\r\n"
        "TEL:+1 (555) 123-4567\r\n"
        "END:VCARD\r\n"
        "BEGIN:BENV\r\n"
        "BEGIN:VCARD\r\n"
        "VERSION:2.1\r\n"
        "TEL:+15559998888\r\n"
        "END:VCARD\r\n"
        "BEGIN:BBODY\r\n"
        "CHARSET:UTF-8\r\n"
        "LENGTH:42\r\n"
        "BEGIN:MSG\r\n"
        "Running late\r\n"
        "END:MSG\r\n"
        "END:BBODY\r\n"
        "END:BENV\r\n"
        "END:BMSG\r\n";

    // Apple-ID destination: no TEL at all, only EMAIL.
    constexpr const char* INCOMING_EMAIL =
        "BEGIN:BMSG\n"
        "VERSION:1.0\n"
        "STATUS:READ\n"
        "TYPE:SMS_GSM\n"
        "FOLDER:telecom/msg/inbox\n"
        "BEGIN:VCARD\n"
        "FN:Sam Smith\n"
        "EMAIL:Sam.Smith@Example.COM\n"
        "END:VCARD\n"
        "BEGIN:BENV\n"
        "BEGIN:BBODY\n"
        "BEGIN:MSG\n"
        "sent from an apple id\n"
        "END:MSG\n"
        "END:BBODY\n"
        "END:BENV\n"
        "END:BMSG\n";

} // namespace

TEST(BMessageParse, ParsesTelephoneOriginator) {
    BMessage m = parse_bmessage(INCOMING_TEL);
    ASSERT_TRUE(m.valid);
    EXPECT_FALSE(m.read);
    EXPECT_EQ(m.type, "SMS_GSM");
    EXPECT_EQ(m.folder, "telecom/msg/inbox");
    EXPECT_EQ(m.originator.tel, "+1 (555) 123-4567");
    EXPECT_EQ(m.originator.name, "Jane Doe");
    EXPECT_EQ(m.body, "Running late");
    ASSERT_EQ(m.recipients.size(), 1u);
    EXPECT_EQ(m.recipients[0].tel, "+15559998888");
}

TEST(BMessageParse, ParsesAppleIdOriginator) {
    BMessage m = parse_bmessage(INCOMING_EMAIL);
    ASSERT_TRUE(m.valid);
    EXPECT_TRUE(m.read);
    EXPECT_TRUE(m.originator.tel.empty());
    EXPECT_EQ(m.originator.email, "Sam.Smith@Example.COM");
    EXPECT_EQ(m.originator.name, "Sam Smith");
    EXPECT_EQ(m.body, "sent from an apple id");
}

TEST(BMessageParse, KeepsMultilineAndUtf8Bodies) {
    const std::string text = "BEGIN:BMSG\r\nSTATUS:UNREAD\r\n"
                             "BEGIN:VCARD\r\nTEL:+15551112222\r\nEND:VCARD\r\n"
                             "BEGIN:BENV\r\nBEGIN:BBODY\r\nBEGIN:MSG\r\n"
                             "line one\r\nline two\r\n\xC3\xA9\xC3\xA0 \xE2\x9C\x93 \xF0\x9F\x8E\x89\r\n"
                             "END:MSG\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n";
    BMessage m = parse_bmessage(text);
    ASSERT_TRUE(m.valid);
    EXPECT_EQ(m.body, "line one\nline two\n\xC3\xA9\xC3\xA0 \xE2\x9C\x93 \xF0\x9F\x8E\x89");
}

// A body line that looks like a structural token is byte-stuffed by the sender.
// Failing to unstuff would corrupt the message; failing to stop at the real
// END:MSG would swallow the rest of the container.
TEST(BMessageParse, UnstuffsBodyLinesThatLookStructural) {
    const std::string text = "BEGIN:BMSG\r\nSTATUS:UNREAD\r\n"
                             "BEGIN:VCARD\r\nTEL:+15551112222\r\nEND:VCARD\r\n"
                             "BEGIN:BENV\r\nBEGIN:BBODY\r\nBEGIN:MSG\r\n"
                             " END:MSG\r\n"
                             " BEGIN:VCARD\r\n"
                             "real text\r\n"
                             "END:MSG\r\nEND:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n";
    BMessage m = parse_bmessage(text);
    ASSERT_TRUE(m.valid);
    EXPECT_EQ(m.body, "END:MSG\nBEGIN:VCARD\nreal text");
    // The stuffed vCard line must not have been read as a real party.
    EXPECT_TRUE(m.recipients.empty());
}

TEST(BMessageParse, AcceptsLfOnlyAndIgnoresUnknownProperties) {
    const std::string text = "BEGIN:BMSG\nX-WEIRD:whatever\nSTATUS:READ\n"
                             "BEGIN:VCARD\nTEL;TYPE=CELL:+15551112222\nX-OTHER:x\nEND:VCARD\n"
                             "BEGIN:BENV\nBEGIN:BBODY\nBEGIN:MSG\nhi\nEND:MSG\nEND:BBODY\nEND:BENV\nEND:BMSG";
    BMessage m = parse_bmessage(text);
    ASSERT_TRUE(m.valid);
    EXPECT_EQ(m.originator.tel, "+15551112222") << "vCard parameters must not break property parsing";
    EXPECT_EQ(m.body, "hi");
}

TEST(BMessageParse, RejectsMalformedInputWithoutThrowing) {
    EXPECT_FALSE(parse_bmessage("").valid);
    EXPECT_FALSE(parse_bmessage("not a bmessage at all").valid);
    EXPECT_FALSE(parse_bmessage("STATUS:READ\nBEGIN:MSG\nbody\n").valid);

    // Truncated mid-container: must parse what it can and not crash.
    BMessage truncated = parse_bmessage("BEGIN:BMSG\nSTATUS:UNREAD\nBEGIN:VCARD\nTEL:+1555");
    EXPECT_TRUE(truncated.valid);
    EXPECT_TRUE(truncated.body.empty());
}

// --- Identity ----------------------------------------------------------

TEST(AddressIdentity, NormalizesPhoneFormatting) {
    EXPECT_EQ(normalize_phone("+1 (555) 123-4567"), "+15551234567");
    EXPECT_EQ(normalize_phone("+1-555-123-4567"), "+15551234567");
    EXPECT_EQ(normalize_phone("555.123.4567"), "5551234567");
    EXPECT_EQ(normalize_phone(""), "");
}

// Guessing a country code could route a reply to a different person, so a
// national number must not silently become an international one.
TEST(AddressIdentity, DoesNotInventCountryCodes) {
    EXPECT_NE(normalize_phone("5551234567"), normalize_phone("+15551234567"));
}

TEST(AddressIdentity, EmailIsCaseInsensitive) {
    EXPECT_EQ(normalize_email("  Sam.Smith@Example.COM "), "sam.smith@example.com");
}

TEST(AddressIdentity, ThreadKeyPrefersTelAndNamespacesTypes) {
    VCardParty both;
    both.tel = "+1 555 123 4567";
    both.email = "a@b.com";
    EXPECT_EQ(thread_key_for(both), "tel:+15551234567");

    VCardParty email_only;
    email_only.email = "A@B.com";
    EXPECT_EQ(thread_key_for(email_only), "email:a@b.com");

    // A phone number and an email must never collide.
    EXPECT_NE(thread_key_for(both), thread_key_for(email_only));

    EXPECT_TRUE(thread_key_for(VCardParty{}).empty());
}

// Providers send OTP SMS from an alphanumeric sender ID. Keyed on the id itself:
// normalizing it as a phone number leaves nothing, and the message is dropped.
TEST(AddressIdentity, AlphanumericSenderKeepsItsOwnKey) {
    VCardParty sender;
    sender.tel = "MMA";
    EXPECT_EQ(thread_key_for(sender), "sender:mma");

    VCardParty spelled_differently;
    spelled_differently.tel = "  mma ";
    EXPECT_EQ(thread_key_for(spelled_differently), thread_key_for(sender));

    // Digits inside a sender id are not a phone number, and must not be keyed as
    // one: "tel:123" belongs to whoever really has that number.
    VCardParty mixed;
    mixed.tel = "MMA-123";
    EXPECT_EQ(thread_key_for(mixed), "sender:mma-123");

    VCardParty number;
    number.tel = "+15551234567";
    EXPECT_EQ(thread_key_for(number), "tel:+15551234567");

    // Control characters never reach a key; the id around them survives.
    VCardParty noisy;
    noisy.tel = "M\x01MA";
    EXPECT_EQ(thread_key_for(noisy), "sender:mma");

    VCardParty junk;
    junk.tel = "\x01\x02";
    EXPECT_TRUE(thread_key_for(junk).empty());
}

// The display name is not identity; two contacts sharing a name must stay apart.
TEST(AddressIdentity, NameDoesNotAffectThreadKey) {
    VCardParty a;
    a.name = "Sam";
    a.tel = "+15551110000";
    VCardParty b;
    b.name = "Sam";
    b.tel = "+15552220000";
    EXPECT_NE(thread_key_for(a), thread_key_for(b));
}

TEST(Timestamps, ParsesMapBasicIso) {
    EXPECT_GT(parse_map_timestamp("20260816T024500"), 0);
    EXPECT_EQ(parse_map_timestamp("20260816T024500Z"), 1786848300); // 2026-08-16T02:45:00Z
    EXPECT_EQ(parse_map_timestamp(""), 0);
    EXPECT_EQ(parse_map_timestamp("garbage"), 0);
    EXPECT_EQ(parse_map_timestamp("20261399T999999"), 0);
}

// --- Store -------------------------------------------------------------

namespace {

    Message make(const std::string& handle, const std::string& thread, int64_t ts, bool read = false,
                 bool outgoing = false) {
        Message m;
        m.handle = handle;
        m.thread_key = thread;
        m.peer_address = thread;
        m.body = "body " + handle;
        m.timestamp = ts;
        m.read = read;
        m.outgoing = outgoing;
        return m;
    }

} // namespace

// MNS events and ListMessages polling both deliver the same message, and the
// phone replays messages after a reconnect.
TEST(MessageStore, DedupesByHandle) {
    MessageStore store;
    EXPECT_TRUE(store.add(make("h1", "tel:+1555", 100)));
    EXPECT_FALSE(store.add(make("h1", "tel:+1555", 100)));
    EXPECT_EQ(store.size(), 1u);
}

TEST(MessageStore, RejectsMessagesWithoutHandle) {
    MessageStore store;
    EXPECT_FALSE(store.add(make("", "tel:+1555", 100)));
    EXPECT_EQ(store.size(), 0u);
}

TEST(MessageStore, ReportsTheNewestTimestampHeld) {
    MessageStore store;
    EXPECT_FALSE(store.newest_timestamp().has_value());
    store.add(make("a", "tel:+1111", 300));
    store.add(make("b", "tel:+2222", 100));
    EXPECT_EQ(store.newest_timestamp(), 300);
}

TEST(MessageStore, GroupsThreadsNewestFirst) {
    MessageStore store;
    store.add(make("a", "tel:+1111", 100));
    store.add(make("b", "tel:+2222", 300));
    store.add(make("c", "tel:+1111", 200));

    auto threads = store.threads();
    ASSERT_EQ(threads.size(), 2u);
    EXPECT_EQ(threads[0].key, "tel:+2222");
    EXPECT_EQ(threads[1].key, "tel:+1111");
    EXPECT_EQ(threads[1].count, 2);
    EXPECT_EQ(threads[1].last_body, "body c") << "preview must come from the newest message";
}

TEST(MessageStore, CountsUnreadIncomingOnly) {
    MessageStore store;
    store.add(make("a", "tel:+1111", 100, /*read=*/false));
    store.add(make("b", "tel:+1111", 200, /*read=*/true));
    store.add(make("c", "tel:+1111", 300, /*read=*/false, /*outgoing=*/true));

    auto threads = store.threads();
    ASSERT_EQ(threads.size(), 1u);
    EXPECT_EQ(threads[0].unread, 1) << "an unread outgoing message is not unread mail";
}

TEST(MessageStore, MarkReadIsIdempotentAndReportsChange) {
    MessageStore store;
    store.add(make("a", "tel:+1111", 100, /*read=*/false));

    EXPECT_TRUE(store.set_read("a", true));
    EXPECT_FALSE(store.set_read("a", true)) << "an echo of our own change must not loop";
    EXPECT_TRUE(store.set_read("a", false));
    EXPECT_FALSE(store.set_read("unknown", true));
}

TEST(MessageStore, MessagesAreOldestFirstAndLimited) {
    MessageStore store;
    for (int i = 0; i < 10; ++i)
        store.add(make("h" + std::to_string(i), "tel:+1111", 100 + i));

    auto all = store.messages("tel:+1111");
    ASSERT_EQ(all.size(), 10u);
    EXPECT_EQ(all.front().handle, "h0");
    EXPECT_EQ(all.back().handle, "h9");

    auto recent = store.messages("tel:+1111", 3);
    ASSERT_EQ(recent.size(), 3u);
    EXPECT_EQ(recent.front().handle, "h7") << "limit must keep the newest, in oldest-first order";
    EXPECT_EQ(recent.back().handle, "h9");
}

TEST(MessageStore, EvictsOldestBeyondCap) {
    MessageStore store(3);
    for (int i = 0; i < 5; ++i)
        store.add(make("h" + std::to_string(i), "tel:+1111", 100 + i));

    EXPECT_EQ(store.size(), 3u);
    EXPECT_EQ(store.find("h0"), nullptr);
    EXPECT_NE(store.find("h4"), nullptr);
}

TEST(MessageStore, BuildsMessageFromIncomingBMessage) {
    Message m = message_from_bmessage(parse_bmessage(INCOMING_TEL), "0x0001", 12345);
    EXPECT_EQ(m.handle, "0x0001");
    EXPECT_EQ(m.thread_key, "tel:+15551234567") << "keyed on the originator for incoming mail";
    EXPECT_EQ(m.peer_name, "Jane Doe");
    EXPECT_FALSE(m.outgoing);
    EXPECT_FALSE(m.read);
    EXPECT_EQ(m.timestamp, 12345);
}

// For a message we sent, the conversation is the recipient, not ourselves.
TEST(MessageStore, OutgoingMessageIsKeyedOnRecipient) {
    const std::string sent = "BEGIN:BMSG\nSTATUS:READ\nFOLDER:telecom/msg/sent\n"
                             "BEGIN:VCARD\nEND:VCARD\n"
                             "BEGIN:BENV\nBEGIN:VCARD\nTEL:+15557778888\nEND:VCARD\n"
                             "BEGIN:BBODY\nBEGIN:MSG\nyo\nEND:MSG\nEND:BBODY\nEND:BENV\nEND:BMSG\n";
    Message m = message_from_bmessage(parse_bmessage(sent), "0x0002", 999);
    EXPECT_TRUE(m.outgoing);
    EXPECT_EQ(m.thread_key, "tel:+15557778888");
}

// --- MAP listings ------------------------------------------------------
//
// ListMessages alone carries enough to display a conversation, so the listing
// path must produce the same identity as a parsed bMessage would.

TEST(MapListing, BuildsMessageWithoutBMessageFetch) {
    MapListing listing;
    listing.handle = "/org/bluez/obex/client/session1/message0";
    listing.subject = "see you there";
    listing.sender_address = "+1 (555) 123-4567";
    listing.sender_name = "Jane Doe";
    listing.timestamp = "20260816T024500Z";
    listing.type = "sms-gsm";
    listing.folder = "inbox";
    listing.read = false;

    Message m = message_from_listing(listing);
    // The handle is the stable id, not the session-scoped object path.
    EXPECT_EQ(m.handle, "0");
    EXPECT_EQ(m.object_path, listing.handle);
    EXPECT_EQ(m.body, "see you there");
    EXPECT_EQ(m.thread_key, "tel:+15551234567");
    EXPECT_EQ(m.peer_address, "+15551234567");
    EXPECT_EQ(m.peer_name, "Jane Doe");
    EXPECT_EQ(m.timestamp, 1786848300);
    EXPECT_FALSE(m.read);
    EXPECT_FALSE(m.outgoing);
}

// An Apple-ID sender arrives in the same field as a phone number, with '@' the
// only discriminator available.
TEST(MapListing, RoutesAppleIdSenderToEmailIdentity) {
    MapListing listing;
    listing.handle = "h";
    listing.sender_address = "Sam@Example.com";
    Message m = message_from_listing(listing);
    EXPECT_EQ(m.thread_key, "email:sam@example.com");
}

// A listing and a full bMessage for the same conversation must land in one
// thread, or history would split depending on how it was fetched.
TEST(MapListing, AgreesWithBMessageThreadKey) {
    MapListing listing;
    listing.handle = "h";
    listing.sender_address = "+15551234567";
    const Message from_listing = message_from_listing(listing);
    const Message from_bmessage = message_from_bmessage(parse_bmessage(INCOMING_TEL), "h2", 1);
    EXPECT_EQ(from_listing.thread_key, from_bmessage.thread_key);
}

TEST(MapListing, SentFlagMarksOutgoing) {
    MapListing listing;
    listing.handle = "h";
    listing.sender_address = "+15551234567";
    listing.sent = true;
    EXPECT_TRUE(message_from_listing(listing).outgoing);
}

// obexd's object path embeds the OBEX session number, which changes on every
// reconnect. Keying a message on the whole path makes the same message look new
// after each one, duplicating history without bound.
TEST(MapHandle, StripsTheSessionScopedPrefix) {
    EXPECT_EQ(map_handle_from_path("/org/bluez/obex/client/session5/message1086086778701665313"),
              "1086086778701665313");
    EXPECT_EQ(map_handle_from_path("/org/bluez/obex/client/session11/message1086086778701665313"),
              "1086086778701665313")
        << "a different session must yield the same handle";
}

TEST(MapHandle, PassesThroughUnexpectedShapes) {
    EXPECT_EQ(map_handle_from_path("12345"), "12345");
    EXPECT_EQ(map_handle_from_path(""), "");
}

TEST(MapListingConversion, KeepsTheObjectPathForActingOnTheMessage) {
    MapListing listing;
    listing.handle = "/org/bluez/obex/client/session5/message42";
    listing.sender_address = "+15551234567";
    listing.subject = "hi";

    Message message = message_from_listing(listing);
    EXPECT_EQ(message.handle, "42");
    EXPECT_EQ(message.object_path, "/org/bluez/obex/client/session5/message42");
}

namespace {

    MapListing listing_for(const std::string& path, bool sent, const std::string& folder) {
        MapListing listing;
        listing.handle = path;
        listing.subject = "see you at six";
        listing.sender_name = "Alice";
        listing.sender_address = "+15551110000";
        listing.recipient_name = "Bob";
        listing.recipient_address = "+15552220000";
        listing.timestamp = "20260817T190000";
        listing.sent = sent;
        listing.folder = folder;
        return listing;
    }

} // namespace

// A received message is identified by whoever sent it.
TEST(MapListing, IncomingIsKeyedOnTheSender) {
    const Message message = message_from_listing(listing_for("/obex/session1/message1", false, "inbox"));
    EXPECT_FALSE(message.outgoing);
    EXPECT_EQ(message.thread_key, "tel:+15551110000");
    EXPECT_EQ(message.peer_name, "Alice");
}

// A message the phone sent names the user as its sender. Keying on that would
// collapse every outgoing message into one thread under the user's own number,
// and would aim a reply there too.
TEST(MapListing, SentIsKeyedOnTheRecipient) {
    const Message message = message_from_listing(listing_for("/obex/session1/message2", true, "sent"));
    EXPECT_TRUE(message.outgoing);
    EXPECT_EQ(message.thread_key, "tel:+15552220000");
    EXPECT_EQ(message.peer_name, "Bob");
}

// Not every phone sets the Sent flag on what it serves from the sent folder.
TEST(MapListing, SentFolderIsOutgoingWithoutTheFlag) {
    const Message message = message_from_listing(listing_for("/obex/session1/message3", false, "telecom/msg/sent"));
    EXPECT_TRUE(message.outgoing);
    EXPECT_EQ(message.thread_key, "tel:+15552220000");
}

// An address we do not have is not an address to guess at: an empty thread key
// is the signal for the caller to drop the message rather than misfile it.
TEST(MapListing, SentWithoutARecipientHasNoThread) {
    MapListing listing = listing_for("/obex/session1/message4", true, "sent");
    listing.recipient_address.clear();
    listing.recipient_name.clear();

    const Message message = message_from_listing(listing);
    EXPECT_TRUE(message.outgoing);
    EXPECT_TRUE(message.thread_key.empty());
}

TEST(MapListing, EmailRecipientIsKeyedAsAnAppleId) {
    MapListing listing = listing_for("/obex/session1/message5", true, "sent");
    listing.recipient_address = "Someone@Example.COM";

    const Message message = message_from_listing(listing);
    EXPECT_EQ(message.thread_key, "email:someone@example.com");
}

namespace {

    Message local_send(const std::string& thread, const std::string& body, int64_t ts) {
        Message m;
        m.handle = std::string(LOCAL_HANDLE_PREFIX) + std::to_string(ts);
        m.thread_key = thread;
        m.peer_address = thread;
        m.body = body;
        m.timestamp = ts;
        m.outgoing = true;
        m.read = true;
        m.folder = "outbox";
        return m;
    }

    Message phone_copy(const std::string& handle, const std::string& thread, const std::string& body, int64_t ts) {
        Message m = local_send(thread, body, ts);
        m.handle = handle;
        m.folder = "sent";
        return m;
    }

} // namespace

// A reply sent from Tether is shown immediately under a local handle. Once the
// phone lists its own copy from the sent folder, the two must collapse into one
// message rather than showing the reply twice.
TEST(MessageStore, PhoneCopySupersedesTheLocalEcho) {
    MessageStore store;
    ASSERT_TRUE(store.add(local_send("tel:+1111", "on my way", 1000)));
    ASSERT_TRUE(store.add(phone_copy("m9", "tel:+1111", "on my way", 1005)));

    EXPECT_EQ(store.size(), 1u);
    auto messages = store.messages("tel:+1111");
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].handle, "m9") << "the phone's copy is the one with a usable handle";
}

// A MAP listing reports the body with its line breaks flattened to spaces, so
// the phone's copy of a multi-line reply never matches the local echo byte for
// byte. Comparing the text alone is what keeps it from showing up twice.
TEST(MessageStore, MultiLineEchoSurvivesAFlattenedPhoneCopy) {
    MessageStore store;
    ASSERT_TRUE(store.add(local_send("tel:+1111", "on my way\nsee you soon", 1000)));
    ASSERT_TRUE(store.add(phone_copy("m9", "tel:+1111", "on my way see you soon", 1005)));

    EXPECT_EQ(store.size(), 1u);
    auto messages = store.messages("tel:+1111");
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].handle, "m9");
}

TEST(MessageStore, UnrelatedOutgoingMessagesAreNotCollapsed) {
    MessageStore store;
    store.add(local_send("tel:+1111", "on my way", 1000));

    // Same text, different conversation.
    store.add(phone_copy("m1", "tel:+2222", "on my way", 1005));
    // Same conversation, different text.
    store.add(phone_copy("m2", "tel:+1111", "running late", 1005));
    // Same text and conversation, but far too long afterwards to be the echo.
    store.add(phone_copy("m3", "tel:+1111", "on my way", 1000 + LOCAL_ECHO_WINDOW_SECONDS + 1));

    EXPECT_EQ(store.size(), 4u) << "a distinct message was mistaken for a duplicate";
}

// The journal replays the placeholder and the phone's copy in the order they
// were written, so a restart must collapse them the same way.
TEST(MessageStore, EchoIsCollapsedOnReplayToo) {
    MessageStore store;
    store.add(local_send("tel:+1111", "on my way", 1000));
    store.add(phone_copy("m9", "tel:+1111", "on my way", 1005));
    store.add(phone_copy("m9", "tel:+1111", "on my way", 1005));

    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(store.threads().size(), 1u);
}

// iOS names the same peer "+14255550123" in the sent folder and "14255550123"
// in the inbox listing, which used to file one person under two conversations.
TEST(MessageStore, OneNumberSpelledTwoWaysIsOneThread) {
    for (bool plus_first : {true, false}) {
        MessageStore store;
        const std::string plus = "tel:+14255550123";
        const std::string bare = "tel:14255550123";
        store.add(make("a", plus_first ? plus : bare, 100));
        store.add(make("b", plus_first ? bare : plus, 200));

        auto threads = store.threads();
        ASSERT_EQ(threads.size(), 1u);
        EXPECT_EQ(threads[0].key, plus) << "the reply address must keep its E.164 form";
        EXPECT_EQ(threads[0].count, 2);

        // Either spelling still resolves the conversation, so a client holding
        // the older key is not left with an empty thread.
        EXPECT_EQ(store.messages(plus).size(), 2u);
        EXPECT_EQ(store.messages(bare).size(), 2u);
    }
}

// A number with its country code and the same number without one are the same
// peer; keying them apart splits the conversation the moment the phone changes
// how it reports the address.
TEST(MessageStore, NationalAndInternationalFormsShareAThread) {
    MessageStore store;
    store.add(make("a", "tel:4255550123", 100));
    store.add(make("b", "tel:+14255550123", 200));

    auto threads = store.threads();
    ASSERT_EQ(threads.size(), 1u);
    EXPECT_EQ(threads[0].key, "tel:+14255550123");
}

// Outside NANP the national form carries a trunk 0 the international one drops.
TEST(MessageStore, TrunkPrefixNationalFormSharesAThread) {
    MessageStore store;
    store.add(make("a", "tel:+61412345678", 100));
    store.add(make("b", "tel:0412345678", 200));

    auto threads = store.threads();
    ASSERT_EQ(threads.size(), 1u);
    EXPECT_EQ(threads[0].key, "tel:+61412345678");
}

// The local echo is keyed on whatever spelling the UI held, the phone's copy on
// the sent folder's. Different spellings must not keep them apart, or the reply
// shows twice.
TEST(MessageStore, EchoCollapsesAcrossAddressSpellings) {
    MessageStore store;
    ASSERT_TRUE(store.add(local_send("tel:14255550123", "on my way", 1000)));
    ASSERT_TRUE(store.add(phone_copy("m9", "tel:+14255550123", "on my way", 1005)));

    EXPECT_EQ(store.size(), 1u);
    ASSERT_EQ(store.threads().size(), 1u);
    EXPECT_EQ(store.messages("tel:+14255550123").size(), 1u);
}

// Short numbers have no country code to discount, so every digit still counts.
TEST(MessageStore, ShortNumbersStaySeparate) {
    MessageStore store;
    store.add(make("a", "tel:1111", 100));
    store.add(make("b", "tel:2222", 200));
    EXPECT_EQ(store.threads().size(), 2u);
}

// Captured from the phone's own sent folder: iOS writes the originator's number
// scheme-tagged. The tag is transport framing, not part of the address, and
// leaving it on would key the conversation under an address nobody has.
TEST(BMessageParse, StripsTheSchemeTagIosPutsOnAddresses) {
    const BMessage m = parse_bmessage("BEGIN:BMSG\r\n"
                                      "VERSION:1.0\r\n"
                                      "STATUS:READ\r\n"
                                      "TYPE:SMS_GSM\r\n"
                                      "FOLDER:telecom/msg/sent\r\n"
                                      "BEGIN:VCARD\r\nVERSION:2.1\r\nTEL:7:+15033201884\r\nEND:VCARD\r\n"
                                      "BEGIN:BENV\r\n"
                                      "BEGIN:VCARD\r\nVERSION:2.1\r\nEMAIL:7:cece.blair@icloud.com\r\nEND:VCARD\r\n"
                                      "BEGIN:BBODY\r\nCHARSET:UTF-8\r\nLENGTH:26\r\n"
                                      "BEGIN:MSG\r\ntest\r\nEND:MSG\r\n"
                                      "END:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n");

    ASSERT_TRUE(m.valid);
    EXPECT_EQ(m.originator.tel, "+15033201884");
    ASSERT_EQ(m.recipients.size(), 1u);
    EXPECT_EQ(m.recipients.front().email, "cece.blair@icloud.com");
    EXPECT_EQ(thread_key_for(m.recipients.front()), "email:cece.blair@icloud.com");
}

// An untagged address is the common case and must survive untouched.
TEST(BMessageParse, LeavesAnUntaggedAddressAlone) {
    const BMessage m = parse_bmessage("BEGIN:BMSG\r\nVERSION:1.0\r\nTYPE:SMS_GSM\r\n"
                                      "BEGIN:VCARD\r\nVERSION:2.1\r\nEMAIL:ab@icloud.com\r\nEND:VCARD\r\n"
                                      "END:BMSG\r\n");
    ASSERT_TRUE(m.valid);
    EXPECT_EQ(m.originator.email, "ab@icloud.com");
}

namespace {

    // Same message in each of the three line endings a phone might use, with a
    // two-line body.
    std::string two_line_bmessage(const std::string& eol) {
        const std::string lines[] = {"BEGIN:BMSG",
                                     "VERSION:1.0",
                                     "STATUS:UNREAD",
                                     "TYPE:SMS_GSM",
                                     "FOLDER:telecom/msg/inbox",
                                     "BEGIN:VCARD",
                                     "VERSION:2.1",
                                     "TEL:+15551234567",
                                     "END:VCARD",
                                     "BEGIN:BENV",
                                     "BEGIN:BBODY",
                                     "CHARSET:UTF-8",
                                     "BEGIN:MSG",
                                     "first line",
                                     "second line",
                                     "END:MSG",
                                     "END:BBODY",
                                     "END:BENV",
                                     "END:BMSG"};
        std::string out;
        for (const auto& line : lines)
            out += line + eol;
        return out;
    }

} // namespace

// The body is what the conversation shows, so its line breaks have to survive
// the parser whichever ending the phone wrote them with.
TEST(BMessage, MultiLineBodyKeepsItsLineBreaks) {
    for (const std::string& eol : {std::string("\r\n"), std::string("\n"), std::string("\r")}) {
        const BMessage parsed = parse_bmessage(two_line_bmessage(eol));
        ASSERT_TRUE(parsed.valid);
        EXPECT_EQ(parsed.body, "first line\nsecond line");
    }
}
