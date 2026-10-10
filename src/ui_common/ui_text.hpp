#pragma once

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Display rules shared by every front end. No toolkit headers.
namespace tether::ui {

    // Normalized case- and accent-insensitive form of a string.
    std::string fold(const std::string& text);

    // Upper-cased first letters of the first and last words; empty for a blank name.
    std::string initials(const std::string& name);

    // A string member, or the fallback when it is missing or not a string.
    std::string json_string(const nlohmann::json& j, const char* key, const std::string& fallback = "");

    // "L 82%   R 79%   Case 45%", omitting whatever is not reporting, or the reason
    // there are no levels at all. Empty while the channel is still opening.
    // "spoken" spells the earbuds out for screen readers instead of "L"/"R".
    std::string airpods_status_text(const nlohmann::json& airpods, bool spoken = false);

    // The daemon's sentence for why the Bluetooth route is not fully up.
    std::string connection_reason(const nlohmann::json& connection);

    // The phone's own words for a call state, kept short enough for a row.
    const char* call_state_text(const std::string& state);

    // What HFP reports about the phone's cellular link. "spoken" replaces the
    // bar glyphs and bare percentage with words for screen readers.
    std::string network_text(const nlohmann::json& calls, bool spoken = false);

    // Time of day for today's calls, the date as well for older ones.
    std::string format_call_time(int64_t epoch);

    // Clock time beside a message.
    std::string format_clock(int64_t epoch);

    // Contact addresses arrive namespaced like thread keys ("tel:…"); this is
    // the bare address for display.
    std::string display_address(const std::string& key);

    // One sendable address from the phonebook, ready for completion.
    struct ContactEntry {
        std::string display; // "Name · address", or the address alone
        std::string address; // what goes in the entry
        std::string search;  // folded haystack, including the normalized form
        bool is_tel = false;
    };

    // Flattens a bt_contacts payload into de-duplicated entries, and fills
    // names with the phonebook name for each thread key.
    std::vector<ContactEntry> contact_entries(const nlohmann::json& contacts,
                                              std::map<std::string, std::string>& names);

} // namespace tether::ui
