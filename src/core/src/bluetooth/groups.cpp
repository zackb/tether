#include "tether/bluetooth/groups.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <pwd.h>
#include <unistd.h>

namespace tether::bluetooth {

    namespace {

        std::string home_dir() {
            if (const char* home = getenv("HOME"); home && *home)
                return home;
            if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir)
                return pw->pw_dir;
            return {};
        }

        std::string trim(const std::string& text) {
            size_t begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return {};
            size_t end = text.find_last_not_of(" \t\r\n");
            return text.substr(begin, end - begin + 1);
        }

        std::string lower(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return text;
        }

        // Collapses case and whitespace so a group name is a stable key.
        std::string normalize_name(const std::string& text) {
            std::string out;
            bool space = false;
            for (char c : lower(trim(text))) {
                if (std::isspace(static_cast<unsigned char>(c))) {
                    space = true;
                    continue;
                }
                if (space && !out.empty())
                    out += '-';
                space = false;
                out += c;
            }
            return out;
        }

        // Splits "you, Alice & Bob" on commas and ampersands.
        std::vector<std::string> split_participants(const std::string& text) {
            std::vector<std::string> parts;
            std::string current;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == ',' || text[i] == '&') {
                    parts.push_back(trim(current));
                    current.clear();
                    continue;
                }
                current += text[i];
            }
            parts.push_back(trim(current));

            std::vector<std::string> out;
            for (auto& part : parts) {
                if (part.empty())
                    continue;
                // The user is always a member and is never a reply recipient.
                if (lower(part) == "you")
                    continue;
                out.push_back(std::move(part));
            }
            return out;
        }

    } // namespace

    bool parse_group_notification(const std::string& title, const std::string& subtitle, GroupInfo& out) {
        const std::string clean_subtitle = trim(subtitle);
        if (clean_subtitle.empty())
            return false;

        out = GroupInfo{};
        out.sender = trim(title);
        out.is_group = true;

        // "To you & Alice" marks an unnamed group. Anything else is a name.
        const std::string lowered = lower(clean_subtitle);
        if (lowered.rfind("to ", 0) == 0) {
            out.named = false;
            out.participants = split_participants(clean_subtitle.substr(3));
            // "To Alice" with nobody else is a one-to-one conversation.
            if (out.participants.empty())
                return false;
            return true;
        }

        out.named = true;
        out.name = clean_subtitle;
        return true;
    }

    std::string group_thread_key(const GroupInfo& info) {
        if (!info.is_group)
            return {};
        if (info.named)
            return "group:name:" + normalize_name(info.name);

        std::vector<std::string> slugs;
        slugs.reserve(info.participants.size());
        for (const auto& participant : info.participants)
            slugs.push_back(normalize_name(participant));
        std::sort(slugs.begin(), slugs.end());

        std::string key = "group:members:";
        for (size_t i = 0; i < slugs.size(); ++i) {
            if (i)
                key += "+";
            key += slugs[i];
        }
        return key;
    }

    void GroupCorrelator::observe(const MessageNotification& notification) { recent_.push_back(notification); }

    void GroupCorrelator::expire(int64_t now) {
        std::erase_if(
            recent_, [&](const MessageNotification& n) { return now - n.received > GROUP_CORRELATION_WINDOW_SECONDS; });
    }

    void GroupCorrelator::clear() { recent_.clear(); }

    Correlation GroupCorrelator::correlate(const std::string& body, int64_t now, GroupInfo& out) const {
        const MessageNotification* match = nullptr;
        size_t matches = 0;

        for (const auto& candidate : recent_) {
            if (now - candidate.received > GROUP_CORRELATION_WINDOW_SECONDS)
                continue;
            if (candidate.body.empty())
                continue;
            // ANCS bodies are capped at what was requested, so a longer MAP body
            // matches the notification as a prefix rather than exactly.
            if (body.rfind(candidate.body, 0) != 0)
                continue;

            ++matches;
            match = &candidate;
        }

        if (matches == 0)
            return Correlation::NoMatch;

        // two notifications with same text cannot be told apart
        if (matches > 1)
            return Correlation::Ambiguous;

        GroupInfo info;
        if (!parse_group_notification(match->sender, match->subtitle, info))
            return Correlation::NoMatch;
        out = info;
        return Correlation::Matched;
    }

    const char* to_string(ReplyEligibility eligibility) {
        switch (eligibility) {
        case ReplyEligibility::Allowed:
            return "allowed";
        case ReplyEligibility::Disabled:
            return "disabled";
        case ReplyEligibility::NeedsRoster:
            return "needs-roster";
        case ReplyEligibility::Unresolved:
            return "unresolved";
        case ReplyEligibility::RouteInvalidated:
            return "route-invalidated";
        }
        return "unknown";
    }

    void GroupRoster::set(const std::string& thread_key, std::vector<std::string> addresses) {
        rosters_[thread_key] = std::move(addresses);
    }

    const std::vector<std::string>* GroupRoster::find(const std::string& thread_key) const {
        auto it = rosters_.find(thread_key);
        return it == rosters_.end() ? nullptr : &it->second;
    }

    std::string GroupRoster::serialize() const {
        nlohmann::json j;
        for (const auto& [key, addresses] : rosters_)
            j[key] = addresses;
        return j.dump(2);
    }

    GroupRoster GroupRoster::deserialize(const std::string& text) {
        GroupRoster roster;
        try {
            auto j = nlohmann::json::parse(text);
            if (!j.is_object())
                return roster;
            for (auto& [key, value] : j.items()) {
                if (value.is_array())
                    roster.set(key, value.get<std::vector<std::string>>());
            }
        } catch (const std::exception&) {
            // A corrupt roster must leave groups read-only rather than routing a
            // reply to whatever survived parsing.
            return GroupRoster{};
        }
        return roster;
    }

    std::string roster_path() {
        std::string home = home_dir();
        if (home.empty())
            return {};
        return (std::filesystem::path(home) / ".config" / "tether" / "groups.json").string();
    }

    GroupRoster load_rosters() {
        std::string path = roster_path();
        if (path.empty())
            return {};
        std::ifstream in(path);
        if (!in.is_open())
            return {};
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return GroupRoster::deserialize(text);
    }

    bool save_rosters(const GroupRoster& roster) {
        std::string path = roster_path();
        if (path.empty())
            return false;

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

        std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out.is_open())
                return false;
            out << roster.serialize();
            if (!out)
                return false;
        }
        std::filesystem::permissions(tmp,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace,
                                     ec);
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
        return true;
    }

    ReplyEligibility resolve_group_recipients(const GroupInfo& info,
                                              const std::string& thread_key,
                                              const ContactStore& contacts,
                                              const GroupRoster& rosters,
                                              bool group_replies_enabled,
                                              std::vector<Recipient>& out,
                                              std::string& reason) {
        out.clear();
        if (!group_replies_enabled) {
            reason = "Group replies are turned off.";
            return ReplyEligibility::Disabled;
        }

        // user-supplied roster is authoritative for either kind of group
        if (const std::vector<std::string>* roster = rosters.find(thread_key)) {
            for (const auto& address : *roster) {
                Recipient recipient;
                std::string err;
                if (!recipient_from_thread_key(address, recipient, err)) {
                    reason = "The roster for this group contains an unusable address: " + err;
                    out.clear();
                    return ReplyEligibility::Unresolved;
                }
                out.push_back(recipient);
            }
            if (out.empty()) {
                reason = "The roster for this group is empty.";
                return ReplyEligibility::NeedsRoster;
            }
            return ReplyEligibility::Allowed;
        }

        // thread with no correlated notification behind it
        if (!info.is_group) {
            reason = "This conversation has not been identified as a group yet, so there is nobody to reply to.";
            return ReplyEligibility::NeedsRoster;
        }

        if (info.named) {
            reason = "This group has a name but no member list. Add its members before replying.";
            return ReplyEligibility::NeedsRoster;
        }

        for (const auto& participant : info.participants) {
            std::vector<std::string> addresses = contacts.addresses_for_name(participant);
            if (addresses.empty()) {
                reason = "\"" + participant + "\" is not in your contacts.";
                out.clear();
                return ReplyEligibility::Unresolved;
            }
            if (addresses.size() > 1) {
                reason = "\"" + participant + "\" matches more than one contact address.";
                out.clear();
                return ReplyEligibility::Unresolved;
            }

            Recipient recipient;
            std::string err;
            if (!recipient_from_thread_key(addresses.front(), recipient, err)) {
                reason = "\"" + participant + "\" resolved to an unusable address: " + err;
                out.clear();
                return ReplyEligibility::Unresolved;
            }
            out.push_back(recipient);
        }

        if (out.empty()) {
            reason = "No recipients could be resolved for this group.";
            return ReplyEligibility::Unresolved;
        }
        return ReplyEligibility::Allowed;
    }

    bool sender_invalidates_route(const std::string& sender_address, const std::vector<Recipient>& recipients) {
        if (sender_address.empty() || recipients.empty())
            return false;

        Recipient sender;
        std::string err;
        if (!recipient_from_thread_key(sender_address, sender, err))
            return true;

        for (const auto& recipient : recipients) {
            if (recipient.kind == sender.kind && recipient.address == sender.address)
                return false;
        }
        return true;
    }

} // namespace tether::bluetooth
