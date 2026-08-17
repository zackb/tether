#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tether::bluetooth {

    // One party in a bMessage, decoded from its embedded vCard.
    //
    // A telephone number in TEL and an AppleID destination in EMAIL;
    // both must be saved, because contacts that exist only as an AppleID have
    // no phone number at all.
    struct VCardParty {
        // Display only.
        std::string name;
        std::string tel;
        std::string email;

        bool empty() const { return tel.empty() && email.empty(); }
        bool operator==(const VCardParty&) const = default;
    };

    struct BMessage {
        bool valid = false;
        // READ or UNREAD as reported by the phone.
        bool read = false;
        // Always SMS_GSM for both SMS and iMessage.
        std::string type;
        std::string folder;
        VCardParty originator;
        std::vector<VCardParty> recipients;
        std::string body;

        bool operator==(const BMessage&) const = default;
    };

    // Tolerant parser: accepts CRLF or LF, missing optional fields, and unknown
    // properties. Returns valid=false rather than throwing on malformed input.
    // This is untrusted data arriving from a phone.
    BMessage parse_bmessage(const std::string& text);

    // Addresses are identity; names are display. These normalizations decide
    // which messages group into one conversation, so they must be stable and
    // must not merge two different people.

    // Strips formatting from a phone number, keeping a leading '+'. Does not
    // invent a country code: "555-1234" stays national, because guessing one
    // could route a reply to a different person.
    std::string normalize_phone(const std::string& raw);

    // Lowercases and trims an email/AppleID address.
    std::string normalize_email(const std::string& raw);

    // The stable key for one conversation.
    std::string thread_key_for(const VCardParty& party);

    // Parses MAP's basic-ISO timestamp (optionally with a trailing offset) into a Unix epoch.
    // Returns 0 when unparseable.
    int64_t parse_map_timestamp(const std::string& text);

    enum class RecipientKind { Tel, Email };

    struct Recipient {
        RecipientKind kind = RecipientKind::Tel;
        std::string address;

        bool operator==(const Recipient&) const = default;
    };

    // Returns false with err_out set when the address must not be used.
    bool validate_recipient(const Recipient& recipient, std::string& err_out);

    // Parses a namespaced thread key ("tel:+15551234567", "email:a@b.com") into a
    // validated recipient. Returns false for unknown prefixes.
    bool recipient_from_thread_key(const std::string& thread_key, Recipient& out, std::string& err_out);

    // MAP byte-stuffing: a body line that would otherwise a structural
    // token is prefixed with a space. Mirrors what the parser undoes, so a body
    // does a round trip unchanged.
    std::string stuff_body(const std::string& body);

    // Builds a MAP 1.4 bMessage for PushMessage. Recipients must already be
    // validated
    //
    // TYPE is always SMS_GSM, including for AppleID recipients: iOS decides
    // between SMS and iMessage itself, and nothing here can force or observe
    // that choice.
    std::string build_bmessage(const std::vector<Recipient>& recipients, const std::string& body);

} // namespace tether::bluetooth
