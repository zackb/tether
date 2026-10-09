#include "scoped_env.hpp"

#include <gtest/gtest.h>
#include <tether/bluetooth/bmessage.hpp>
#include <tether/bluetooth/contacts.hpp>
#include <tether/bluetooth/messages.hpp>
#include <tether/bluetooth/vcard.hpp>
#include <tether/secret_store.hpp>

#include <filesystem>
#include <fstream>

using namespace tether;
using namespace tether::bluetooth;

namespace {

    ContactStore three_contacts() {
        ContactStore store;
        store.set(parse_vcards("BEGIN:VCARD\nFN:Ada Lovelace\nTEL:+1 (555) 123-4567\nEND:VCARD\n"
                               "BEGIN:VCARD\nFN:Grace Hopper\nEMAIL:grace@example.com\nEND:VCARD\n"
                               "BEGIN:VCARD\nFN:Alan Turing\nTEL:+15559876543\nEMAIL:alan@example.com\nEND:VCARD\n"));
        return store;
    }

    std::vector<std::string> names_of(const std::vector<VCard>& cards) {
        std::vector<std::string> out;
        for (const auto& card : cards)
            out.push_back(card.name);
        return out;
    }

    // A scratch HOME, so the store key and both cache paths are throwaway.
    class ScopedStore {
    public:
        explicit ScopedStore(const std::string& name, Retention mode = Retention::Encrypted)
            : dir_(std::filesystem::temp_directory_path() / ("tether-contacts-" + name)), home_("HOME", dir_) {
            std::filesystem::remove_all(dir_);
            std::filesystem::create_directories(dir_);
            secret::reset_for_test();
            secret::set_retention(mode);
        }

        ~ScopedStore() {
            secret::reset_for_test();
            std::error_code ec;
            std::filesystem::remove_all(dir_, ec);
        }

        std::filesystem::path store() const { return dir_ / ".local" / "share" / "tether"; }

    private:
        std::filesystem::path dir_;
        tether::testing::ScopedEnv home_;
    };

} // namespace

TEST(VCard, ParsesNameTelAndEmail) {
    auto cards = parse_vcards("BEGIN:VCARD\r\n"
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
    auto cards = parse_vcards("BEGIN:VCARD\n"
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
    auto cards = parse_vcards("BEGIN:VCARD\nFN:One\nTEL:+1555000001\nEND:VCARD\n"
                              "BEGIN:VCARD\nFN:Two\nTEL:+1555000002\nEND:VCARD\n");

    ASSERT_EQ(cards.size(), 2u);
    EXPECT_EQ(cards[0].name, "One");
    EXPECT_EQ(cards[1].name, "Two");
}

// RFC 2425 folding splits long values across lines; without unfolding, a long
// name or address is silently truncated at the fold.
TEST(VCard, UnfoldsContinuationLines) {
    auto cards = parse_vcards("BEGIN:VCARD\r\n"
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
    auto cards = parse_vcards("BEGIN:VCARD\nFN:First\nTEL:+1555000001\n"
                              "BEGIN:VCARD\nFN:Second\nTEL:+1555000002\nEND:VCARD\n");

    ASSERT_EQ(cards.size(), 2u);
    EXPECT_EQ(cards[0].name, "First");
    EXPECT_EQ(cards[1].name, "Second");
}

// iOS sends the call type as a bare parameter, as seen on an iPhone's cch.
TEST(VCard, ParsesCallHistoryEntry) {
    auto cards = parse_vcards("BEGIN:VCARD\nVERSION:3.0\nFN:\nN:\nTEL;TYPE=CELL:+15551234567\n"
                              "X-IRMC-CALL-DATETIME;MISSED:20261007T111657\nEND:VCARD\n"
                              "BEGIN:VCARD\nFN:Ada\nX-IRMC-CALL-DATETIME;TYPE=DIALED:20261005T092933\nEND:VCARD\n");

    ASSERT_EQ(cards.size(), 2u);
    EXPECT_EQ(cards[0].call_type, "MISSED");
    EXPECT_EQ(cards[0].call_time, "20261007T111657");
    EXPECT_EQ(cards[0].tels, std::vector<std::string>{"+15551234567"});
    EXPECT_EQ(cards[1].call_type, "DIALED");
}

// A withheld caller has no number and no name, only the time.
TEST(VCard, KeepsCallEntryWithNoNumber) {
    auto cards = parse_vcards("BEGIN:VCARD\nX-IRMC-CALL-DATETIME;RECEIVED:20261005T122204\nEND:VCARD\n");

    ASSERT_EQ(cards.size(), 1u);
    EXPECT_TRUE(cards[0].tels.empty());
    EXPECT_EQ(cards[0].call_type, "RECEIVED");
}

TEST(ContactStore, ResolvesNamesByTelAndEmail) {
    ContactStore store;
    store.set(parse_vcards("BEGIN:VCARD\nFN:Ada\nTEL:+1 (555) 123-4567\nEMAIL:Ada@Example.COM\nEND:VCARD\n"));

    EXPECT_EQ(store.name_for("tel:+15551234567"), "Ada");
    EXPECT_EQ(store.name_for("email:ada@example.com"), "Ada");
    EXPECT_EQ(store.name_for("tel:+15559999999"), "");
}

// Most contacts are saved without a country code while the phone keys threads
// in E.164, so a strict match leaves the majority of conversations labelled with
// a bare number. A display name is not routing -- the thread key is untouched
// and a reply still goes to the address on the thread -- so the two forms are
// allowed to meet here, and only here.
TEST(ContactStore, MatchesNationalAndE164FormsForDisplay) {
    ContactStore store;
    store.set(parse_vcards("BEGIN:VCARD\nFN:Ada\nTEL:+15551234567\nEND:VCARD\n"
                           "BEGIN:VCARD\nFN:Grace\nTEL:5039998888\nEND:VCARD\n"));

    EXPECT_EQ(store.name_for("tel:+15551234567"), "Ada");
    EXPECT_EQ(store.name_for("tel:5551234567"), "Ada") << "a contact saved in E.164 must label a national thread";
    EXPECT_EQ(store.name_for("tel:+15039998888"), "Grace") << "and the reverse, which is the common case";

    EXPECT_EQ(store.name_for("tel:+15550000000"), "") << "an unknown number stays unlabelled";
}

// Two people cannot be told apart by the fallback alone, and labelling a thread
// with the wrong person's name is worse than labelling it with a number.
TEST(ContactStore, RefusesToGuessWhenTwoContactsShareASuffix) {
    ContactStore store;
    store.set(parse_vcards("BEGIN:VCARD\nFN:Ada\nTEL:+15551234567\nEND:VCARD\n"
                           "BEGIN:VCARD\nFN:Grace\nTEL:+445551234567\nEND:VCARD\n"));

    EXPECT_EQ(store.name_for("tel:+15551234567"), "Ada") << "an exact key still wins outright";
    EXPECT_EQ(store.name_for("tel:5551234567"), "") << "an ambiguous suffix resolves to no name";
}

// Shortcodes and extensions are too short to carry a country code, so they match
// exactly or not at all.
TEST(ContactStore, DoesNotSuffixMatchShortNumbers) {
    ContactStore store;
    store.set(parse_vcards("BEGIN:VCARD\nFN:Alerts\nTEL:4125\nEND:VCARD\n"));

    EXPECT_EQ(store.name_for("tel:4125"), "Alerts");
    EXPECT_EQ(store.name_for("tel:+15554125"), "");
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

TEST(ContactSearch, MatchesOnPartOfAName) {
    EXPECT_EQ(names_of(three_contacts().search("lace", 10)), (std::vector<std::string>{"Ada Lovelace"}));
    EXPECT_EQ(names_of(three_contacts().search("hopper", 10)), (std::vector<std::string>{"Grace Hopper"}));
}

TEST(ContactSearch, IsCaseInsensitive) {
    EXPECT_EQ(names_of(three_contacts().search("ADA", 10)), (std::vector<std::string>{"Ada Lovelace"}));
}

TEST(ContactSearch, MatchesANumberTypedWithoutItsFormatting) {
    // The contact is stored as "+1 (555) 123-4567"; nobody types it that way.
    EXPECT_EQ(names_of(three_contacts().search("5551234567", 10)), (std::vector<std::string>{"Ada Lovelace"}));
}

TEST(ContactSearch, MatchesAnEmailAddress) {
    EXPECT_EQ(names_of(three_contacts().search("grace@", 10)), (std::vector<std::string>{"Grace Hopper"}));
}

TEST(ContactSearch, AnEmptyQueryReturnsEverythingSortedByName) {
    EXPECT_EQ(names_of(three_contacts().search("", 10)),
              (std::vector<std::string>{"Ada Lovelace", "Alan Turing", "Grace Hopper"}));
}

TEST(ContactSearch, HonoursTheLimit) { EXPECT_EQ(three_contacts().search("", 2).size(), 2u); }

TEST(ContactSearch, NoMatchIsEmptyRatherThanEverything) { EXPECT_TRUE(three_contacts().search("nobody", 10).empty()); }

TEST(ContactStore, EncryptedCacheLeavesNothingReadable) {
    ScopedStore scoped("encrypted");
    ASSERT_TRUE(save_contacts(three_contacts()));

    EXPECT_FALSE(std::filesystem::exists(scoped.store() / "contacts.json"))
        << "the plaintext name must stay empty, or an older tetherd would read the phonebook";
    const auto sealed = scoped.store() / "contacts.json.enc";
    ASSERT_TRUE(std::filesystem::exists(sealed));

    std::ifstream in(sealed);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text.find("Lovelace"), std::string::npos) << "a contact name is on disk in the clear";
    EXPECT_EQ(text.find("5551234567"), std::string::npos) << "a phone number is on disk in the clear";

    EXPECT_EQ(load_contacts().name_for("tel:+15551234567"), "Ada Lovelace");
}

TEST(ContactStore, MigratesAPlaintextCacheOnLoad) {
    ScopedStore scoped("migrate");
    {
        secret::set_retention(Retention::Plaintext);
        ASSERT_TRUE(save_contacts(three_contacts()));
        ASSERT_TRUE(std::filesystem::exists(scoped.store() / "contacts.json"));
    }

    secret::set_retention(Retention::Encrypted);
    EXPECT_EQ(load_contacts().name_for("tel:+15551234567"), "Ada Lovelace") << "migration lost the phonebook";
    EXPECT_FALSE(std::filesystem::exists(scoped.store() / "contacts.json"))
        << "the plaintext copy outlived the migration";
    EXPECT_TRUE(std::filesystem::exists(scoped.store() / "contacts.json.enc"));
}

// A crash between the rename and the unlink leaves both files, and the plaintext
// phonebook used to stay on disk from then on.
TEST(ContactStore, RemovesASourceLeftBehindByAnInterruptedMigration) {
    ScopedStore scoped("migrate-interrupted");
    ASSERT_TRUE(save_contacts(three_contacts()));
    ASSERT_TRUE(std::filesystem::exists(scoped.store() / "contacts.json.enc"));

    // The plaintext cache the interrupted migration never unlinked.
    {
        secret::set_retention(Retention::Plaintext);
        ASSERT_TRUE(save_contacts(three_contacts()));
        ASSERT_TRUE(std::filesystem::exists(scoped.store() / "contacts.json"));
    }

    secret::set_retention(Retention::Encrypted);
    EXPECT_EQ(load_contacts().name_for("tel:+15551234567"), "Ada Lovelace") << "the sealed cache was disturbed";
    EXPECT_FALSE(std::filesystem::exists(scoped.store() / "contacts.json")) << "the plaintext copy is still on disk";
}

TEST(ContactStore, RetentionNoneWritesNothing) {
    ScopedStore scoped("none", Retention::None);
    EXPECT_FALSE(save_contacts(three_contacts()));
    EXPECT_FALSE(std::filesystem::exists(scoped.store() / "contacts.json"));
    EXPECT_FALSE(std::filesystem::exists(scoped.store() / "contacts.json.enc"));
    EXPECT_EQ(load_contacts().size(), 0u);
}

// Overwriting a good cache with an empty one would cost every display name
// until the next PBAP pull, which can be a reconnect away.
TEST(ContactStore, AnUnreadableKeyDoesNotClobberTheCache) {
    ScopedStore scoped("clobber");
    ASSERT_TRUE(save_contacts(three_contacts()));
    const auto sealed = scoped.store() / "contacts.json.enc";
    std::ifstream in(sealed);
    const std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    {
        // A different HOME is a different key: what a locked wallet looks like.
        const auto elsewhere = std::filesystem::temp_directory_path() / "tether-contacts-clobber-other";
        std::filesystem::create_directories(elsewhere);
        tether::testing::ScopedEnv home("HOME", elsewhere);
        secret::reset_for_test();
        load_contacts();
        std::filesystem::remove_all(elsewhere);
    }

    secret::reset_for_test();
    std::ifstream after_in(sealed);
    const std::string after((std::istreambuf_iterator<char>(after_in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(after, before) << "the cache was rewritten by a daemon that could not read it";
}

// bt_list_contacts emits addresses already namespaced, and the composer turns
// one back into a recipient to fill its completion. recipient_from_input takes a
// *bare* address and rejects the "tel:" prefix as a vCard delimiter, so feeding
// it these silently empties the popup instead of erroring.
TEST(ContactSearch, NamespacedAddressesParseBackToTheSameThreadKey) {
    for (const std::string key : {"tel:+15551234567", "email:ada@example.com"}) {
        Recipient recipient;
        std::string err;
        ASSERT_TRUE(recipient_from_thread_key(key, recipient, err)) << key << ": " << err;
        EXPECT_EQ(thread_key_for(recipient), key);

        // The form the daemon does not send, spelled out so the reason this
        // helper is the right one does not have to be rediscovered.
        Recipient rejected;
        std::string reject_err;
        EXPECT_FALSE(recipient_from_input(key, rejected, reject_err))
            << "recipient_from_input accepted a namespaced address; the two helpers are no longer distinct";
    }
}

// Every address bt_list_contacts is built from has to survive that round trip,
// or the contact silently vanishes from the picker.
TEST(ContactSearch, EveryEmittedAddressIsUsableAsARecipient) {
    for (const auto& card : three_contacts().search("", 10)) {
        for (const auto& tel : card.tels) {
            const std::string key = "tel:" + normalize_phone(tel);
            Recipient recipient;
            std::string err;
            EXPECT_TRUE(recipient_from_thread_key(key, recipient, err)) << key << ": " << err;
        }
        for (const auto& email : card.emails) {
            const std::string key = "email:" + normalize_email(email);
            Recipient recipient;
            std::string err;
            EXPECT_TRUE(recipient_from_thread_key(key, recipient, err)) << key << ": " << err;
        }
    }
}

// The interrupted-migration rule keeps the destination and drops the source. The
// test above writes the same phonebook to both paths, so it cannot tell which one
// survived; this one gives them different contents and pins the documented rule.
TEST(ContactStore, AnInterruptedMigrationKeepsTheDestinationNotTheSource) {
    ScopedStore scoped("migrate-destination-wins");

    ASSERT_TRUE(save_contacts(three_contacts()));
    ASSERT_TRUE(std::filesystem::exists(scoped.store() / "contacts.json.enc"));

    // A stale plaintext cache holding somebody the sealed one has never heard of.
    {
        secret::set_retention(Retention::Plaintext);
        ContactStore stale;
        stale.set(parse_vcards("BEGIN:VCARD\nFN:Stale Contact\nTEL:+15550000009\nEND:VCARD\n"));
        ASSERT_TRUE(save_contacts(stale));
    }

    secret::set_retention(Retention::Encrypted);
    const ContactStore loaded = load_contacts();
    EXPECT_EQ(loaded.name_for("tel:+15551234567"), "Ada Lovelace") << "the sealed cache was replaced by the source";
    EXPECT_EQ(loaded.name_for("tel:+15550000009"), "") << "the stale source won over the destination";
    EXPECT_FALSE(std::filesystem::exists(scoped.store() / "contacts.json")) << "the plaintext copy is still on disk";
}

// iOS sends fav cards in a different number format than pb.
TEST(ContactSearch, ListsFavoritesFirstAndNeverCutsThemOff) {
    auto cards = parse_vcards("BEGIN:VCARD\nFN:Ada Lovelace\nTEL:+1 (555) 123-4567\nEND:VCARD\n"
                              "BEGIN:VCARD\nFN:Grace Hopper\nEMAIL:grace@example.com\nEND:VCARD\n"
                              "BEGIN:VCARD\nFN:Alan Turing\nTEL:+15559876543\nEND:VCARD\n");
    mark_favorites(cards, parse_vcards("BEGIN:VCARD\nFN:Alan\nTEL:+1 555 987 6543\nEND:VCARD\n"));
    ContactStore store;
    store.set(std::move(cards));

    EXPECT_EQ(names_of(store.search("", 10)),
              (std::vector<std::string>{"Alan Turing", "Ada Lovelace", "Grace Hopper"}));
    EXPECT_EQ(names_of(store.search("", 1)), (std::vector<std::string>{"Alan Turing"}));
}

TEST(ContactStore, FavoriteSurvivesJsonRoundTrip) {
    auto cards = parse_vcards("BEGIN:VCARD\nFN:Ada\nTEL:+15551234567\nEND:VCARD\n");
    mark_favorites(cards, cards);
    ContactStore store;
    store.set(std::move(cards));

    ScopedStore scope("favorite-roundtrip");
    ASSERT_TRUE(deserialize_contacts(serialize_contacts(store)).contacts().at(0).favorite);
}
