#include "tether/bluetooth/ancs/notifications.hpp"

#include <algorithm>

namespace tether::bluetooth::ancs {

    namespace {
        constexpr size_t MAX_RETAINED = 200;
    } // namespace

    Decision NotificationRegistry::classify(const SourceEvent& event, bool initial) const {
        if (event.event == EventId::Removed)
            return Decision::Withdraw;

        if (initial && event.pre_existing())
            return Decision::Ignore;

        if (event.event == EventId::Modified)
            return Decision::Fetch;

        return seen_.count(event.uid) ? Decision::Ignore : Decision::Fetch;
    }

    void NotificationRegistry::remember(const SourceEvent& event) { seen_[event.uid] = 0; }

    void NotificationRegistry::forget(uint32_t uid) {
        seen_.erase(uid);
        notifications_.erase(uid);
        order_.erase(std::remove(order_.begin(), order_.end(), uid), order_.end());
    }

    void NotificationRegistry::clear() {
        seen_.clear();
        notifications_.clear();
        order_.clear();
    }

    void NotificationRegistry::store(const Notification& notification) {
        if (!notifications_.count(notification.uid))
            order_.push_back(notification.uid);
        notifications_[notification.uid] = notification;

        while (order_.size() > MAX_RETAINED) {
            const uint32_t oldest = order_.front();
            order_.erase(order_.begin());
            notifications_.erase(oldest);
        }
    }

    const Notification* NotificationRegistry::find(uint32_t uid) const {
        auto it = notifications_.find(uid);
        return it == notifications_.end() ? nullptr : &it->second;
    }

    std::vector<Notification> NotificationRegistry::recent(size_t limit) const {
        std::vector<Notification> out;
        for (auto it = order_.rbegin(); it != order_.rend() && out.size() < limit; ++it) {
            auto found = notifications_.find(*it);
            if (found != notifications_.end())
                out.push_back(found->second);
        }
        return out;
    }

    bool should_show_desktop_popup(const Notification& notification) {
        if (notification.app_id == APP_ID_MESSAGES)
            return false;
        return !notification.silent;
    }

    nlohmann::json to_json(const Notification& notification) {
        return {
            {"uid", notification.uid},
            {"app_id", notification.app_id},
            {"app_name", notification.app_name},
            {"title", notification.title},
            {"subtitle", notification.subtitle},
            {"body", notification.body},
            {"category", static_cast<int>(notification.category)},
            {"timestamp", notification.received},
            {"silent", notification.silent},
            {"positive_action", notification.has_positive_action},
            {"negative_action", notification.has_negative_action},
        };
    }

} // namespace tether::bluetooth::ancs
