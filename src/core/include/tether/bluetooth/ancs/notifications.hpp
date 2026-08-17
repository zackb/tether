#pragma once

#include "tether/bluetooth/ancs/protocol.hpp"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace tether::bluetooth::ancs {

    // A mirrored notification, as far as it has been resolved.
    struct Notification {
        uint32_t uid = 0;
        std::string app_id;
        // The app's display name once resolved; falls back to the identifier.
        std::string app_name;
        std::string title;
        std::string subtitle;
        std::string body;
        CategoryId category = CategoryId::Other;
        int64_t received = 0;
        bool silent = false;
        bool has_positive_action = false;
        bool has_negative_action = false;

        bool operator==(const Notification&) const = default;
    };

    // Whether a Source event should be resolved and shown.
    enum class Decision {
        Fetch,   // new or genuinely updated: ask for its attributes
        Ignore,  // a replay, or something deliberately not mirrored
        Withdraw // removed on the phone: drop the desktop copy
    };

    // Applies the delivery rules that keep the desktop from being flooded.
    //
    // iOS replays every existing notification whenever the subscription comes
    // up, so the same notification arrives again after each reconnect. Dedupe is
    // on the notification UID, but a Modified event is a genuine update the user
    // should see, so it is never suppressed by having seen the UID before.
    class NotificationRegistry {
    public:
        // `initial` marks the first subscription of a session, where
        // pre-existing notifications are the phone's backlog rather than news.
        Decision classify(const SourceEvent& event, bool initial) const;

        void remember(const SourceEvent& event);
        void forget(uint32_t uid);
        void clear();

        void store(const Notification& notification);
        const Notification* find(uint32_t uid) const;
        // Newest first.
        std::vector<Notification> recent(size_t limit = 50) const;

        size_t size() const { return notifications_.size(); }

    private:
        // UIDs seen this session, to recognize a replay.
        std::map<uint32_t, int64_t> seen_;
        std::map<uint32_t, Notification> notifications_;
        std::vector<uint32_t> order_;
    };

    // Messages arrive over MAP already, with read state that stays in sync. The
    // ANCS copy is kept for correlation but must never raise a second popup.
    bool should_show_desktop_popup(const Notification& notification);

    nlohmann::json to_json(const Notification& notification);

} // namespace tether::bluetooth::ancs
