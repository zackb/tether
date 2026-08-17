#pragma once

#include "tether/bluetooth/bmessage.hpp"
#include "tether/bluetooth/contacts.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tether::bluetooth {

    inline constexpr int GROUP_CORRELATION_WINDOW_SECONDS = 30;

    // What the Messages notification revealed about a conversation.
    struct GroupInfo {
        bool is_group = false;
        // when the group has a name
        bool named = false;
        std::string name;
        // display names parsed from an unnamed group's subtitle, excluding the user.
        std::vector<std::string> participants;
        // Display only. Never part of thread identity or reply routing.
        std::string sender;

        bool operator==(const GroupInfo&) const = default;
    };

    // Parses a Messages notification into group information.
    //
    // An unnamed group's subtitle is like "To you & Alice", with further
    // names separated by commas or ampersands. Anything else is treated as a
    // group name. Returns false when the notification describes a one-to-one
    // conversation, which has no subtitle of either.
    bool parse_group_notification(const std::string& title, const std::string& subtitle, GroupInfo& out);

    // The local thread key for a group.
    std::string group_thread_key(const GroupInfo& info);

    // One observed Apple Messages notification
    struct MessageNotification {
        std::string sender;
        std::string subtitle;
        std::string body;
        int64_t received = 0;
    };

    enum class Correlation {
        Matched,
        NoMatch,
        Ambiguous,
    };

    // Matches arriving MAP messages against recent Messages notifications.
    class GroupCorrelator {
    public:
        void observe(const MessageNotification& notification);
        void expire(int64_t now);
        void clear();

        Correlation correlate(const std::string& body, int64_t now, GroupInfo& out) const;

        size_t size() const { return recent_.size(); }

    private:
        std::vector<MessageNotification> recent_;
    };

    // Why a group cannot be replied to
    enum class ReplyEligibility {
        Allowed,
        // Group messaging is switched off.
        Disabled,
        // A named group with no user-supplied roster.
        NeedsRoster,
        // A participant name matched no contact, or more than one.
        Unresolved,
        // A sender appeared who is not in the known member set.
        RouteInvalidated,
    };

    const char* to_string(ReplyEligibility eligibility);

    // Rosters the user supplies for named groups, keyed by thread key.
    class GroupRoster {
    public:
        void set(const std::string& thread_key, std::vector<std::string> addresses);
        const std::vector<std::string>* find(const std::string& thread_key) const;
        bool empty() const { return rosters_.empty(); }

        std::string serialize() const;
        static GroupRoster deserialize(const std::string& text);

    private:
        std::map<std::string, std::vector<std::string>> rosters_;
    };

    std::string roster_path();
    GroupRoster load_rosters();
    bool save_rosters(const GroupRoster& roster);

    // Resolves who a group reply would actually go to.
    // Every participant name must resolve to exactly one address. A name that
    // matches several contacts is refused rather than guessed.
    ReplyEligibility resolve_group_recipients(const GroupInfo& info,
                                              const std::string& thread_key,
                                              const ContactStore& contacts,
                                              const GroupRoster& rosters,
                                              bool group_replies_enabled,
                                              std::vector<Recipient>& out,
                                              std::string& reason);

    // True when `sender` is not among the addresses a reply would be sent to.
    bool sender_invalidates_route(const std::string& sender_address, const std::vector<Recipient>& recipients);

} // namespace tether::bluetooth
