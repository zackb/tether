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
        // Already on the phone when the subscription came up.
        bool pre_existing = false;
        uint64_t session = 0;

        bool operator==(const Notification&) const = default;
    };

    // Whether a Source event should be resolved and shown.
    enum class Decision {
        Fetch,   // new or genuinely updated: ask for its attributes
        Ignore,  // a replay, or something deliberately not mirrored
        Withdraw // removed on the phone: drop the desktop copy
    };

    // Holds new notifications until their app's display name arrives or WAIT_SECONDS pass.
    class HeldPopups {
    public:
        static constexpr int64_t WAIT_SECONDS = 2;

        void hold(Notification notification, int64_t now);

        // The notifications held for `app_id`, renamed to `name`, oldest first.
        std::vector<Notification> release_app(const std::string& app_id, const std::string& name);

        // The notifications that have waited long enough, oldest first.
        std::vector<Notification> release_expired(int64_t now);

        // Everything held, oldest first (the session is ending).
        std::vector<Notification> release_all();

        bool empty() const { return held_.empty(); }

    private:
        struct Held {
            Notification notification;
            int64_t deadline;
        };
        std::vector<Held> held_;
    };

    // Applies the delivery rules that keep the desktop from being flooded.
    //
    // iOS replays every existing notification whenever the subscription comes
    // up, so the same notification arrives again after each reconnect. Dedupe is
    // on the notification UID, but a Modified event is a genuine update the user
    // should see, so it is never suppressed by having seen the UID before.
    class NotificationRegistry {
    public:
        Decision classify(const SourceEvent& event) const;

        void remember(const SourceEvent& event);
        void forget(uint32_t uid);
        void clear();
        void begin_session();
        uint64_t session() const { return session_; }

        void store(const Notification& notification);
        // Applies a display name resolved after the notifications were stored.
        void rename_app(const std::string& app_id, const std::string& name);
        const Notification* find(uint32_t uid) const;
        // Newest first.
        std::vector<Notification> recent(size_t limit = 50) const;

        size_t size() const { return notifications_.size(); }

    private:
        std::map<uint32_t, int64_t> seen_;
        uint64_t session_ = 0;
        std::map<uint32_t, Notification> notifications_;
        std::vector<uint32_t> order_;
    };

    // "com.burbn.instagram" -> "Instagram". Stands in until the phone answers
    // with the app's real display name, and when it never does.
    std::string derive_app_name(const std::string& app_id);

    // Messages arrive over MAP already, with read state that stays in sync. The
    // ANCS copy is kept for correlation but must never raise a second popup.
    // Everything else is shown, silent included: iOS sets FlagSilent whenever the
    // phone is muted or in a Focus mode, which means "make no noise", not "hide".
    bool should_show_desktop_popup(const Notification& notification);

    // The order to ask the phone about events that arrived together. A session starts with iOS replaying all of
    // Notification Center, more entries than the client queues, so the ones fetched should be the newest. Within a
    // session UIDs grow, so the newest additions are last in the replay: everything that is not an addition keeps
    // its place at the front, then additions newest first.
    std::vector<SourceEvent> fetch_order(std::vector<SourceEvent> events);

    // Freedesktop icon names to try for a notification, best first: the app's
    // own logo, then something generic for its category.
    std::vector<std::string> icon_candidates(const Notification& notification);

    // How to find the Linux counterpart of an iPhone app.
    struct LaunchTarget {
        // No "://".
        std::string uri_scheme;
        // No ".desktop" suffix.
        std::vector<std::string> desktop_ids;

        bool empty() const { return uri_scheme.empty() && desktop_ids.empty(); }
    };

    LaunchTarget launch_target(const std::string& app_id);

    nlohmann::json to_json(const Notification& notification);

} // namespace tether::bluetooth::ancs
