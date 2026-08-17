#pragma once

#include <string>
#include <vector>

namespace tether::bluetooth {

    // One contact as PBAP delivers it.
    //
    // Name is display data. The tel and email lists are the identity keys, and a contact routinely has several of each.
    struct VCard {
        std::string name;
        std::vector<std::string> tels;
        std::vector<std::string> emails;

        bool empty() const { return name.empty() && tels.empty() && emails.empty(); }
    };

    // Parses a vCard 3.0 document containing any number of cards.
    std::vector<VCard> parse_vcards(const std::string& text);

    // Undoes RFC 2425 line folding: a line beginning with a space or tab
    // continues the previous one. Also normalizes CRLF to LF.
    std::vector<std::string> vcard_unfold(const std::string& text);

    // "TEL;TYPE=CELL:+15551234567" -> name "TEL", value "+15551234567".
    // Parameters after ';' are discarded; only the property name matters.
    bool vcard_split_property(const std::string& line, std::string& name, std::string& value);

    // vCard N is structured "Family;Given;Middle;Prefix;Suffix".
    std::string vcard_name_from_n(const std::string& value);

    // vCard 3.0 escapes \, \; \n and \\ in values.
    std::string vcard_unescape(const std::string& value);

} // namespace tether::bluetooth
