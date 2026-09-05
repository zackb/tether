#include "tether/bluetooth/ancs/notifications.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace tether::bluetooth::ancs {

    namespace {
        constexpr size_t MAX_RETAINED = 200;

        // Generic stand-ins for an app whose own logo is unknown or absent from
        // the theme. Two per category, because Breeze and Adwaita disagree on
        // several of these names and only one of them has to land.
        std::vector<const char*> category_icons(CategoryId category) {
            switch (category) {
            case CategoryId::IncomingCall:
                return {"call-start", "call-incoming"};
            case CategoryId::MissedCall:
                return {"call-stop", "call-missed"};
            case CategoryId::Voicemail:
                return {"media-playback-start", "audio-input-microphone"};
            case CategoryId::Social:
                return {"internet-chat", "im-user"};
            case CategoryId::Schedule:
                return {"office-calendar", "x-office-calendar"};
            case CategoryId::Email:
                return {"mail-unread", "mail-message-new"};
            case CategoryId::News:
                return {"application-rss+xml", "internet-news-reader"};
            case CategoryId::HealthAndFitness:
                return {"applications-health", "preferences-desktop-personal"};
            case CategoryId::BusinessAndFinance:
                return {"wallet-open", "office-chart-line"};
            case CategoryId::Location:
                return {"mark-location", "find-location"};
            case CategoryId::Entertainment:
                return {"applications-multimedia", "multimedia-player"};
            case CategoryId::Other:
                break;
            }
            return {"phone-symbolic", "phone"};
        }
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
        // sorted by the delivery time, not arrival: backlog replay is not ordered.
        std::vector<Notification> out;
        out.reserve(notifications_.size());
        for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
            auto found = notifications_.find(*it);
            if (found != notifications_.end())
                out.push_back(found->second);
        }
        std::stable_sort(out.begin(), out.end(), [](const Notification& a, const Notification& b) {
            return a.received > b.received;
        });
        if (out.size() > limit)
            out.resize(limit);
        return out;
    }

    void NotificationRegistry::rename_app(const std::string& app_id, const std::string& name) {
        if (app_id.empty() || name.empty())
            return;
        for (auto& [uid, notification] : notifications_) {
            if (notification.app_id == app_id)
                notification.app_name = name;
        }
    }

    std::string derive_app_name(const std::string& app_id) {
        const size_t dot = app_id.rfind('.');
        if (dot == std::string::npos || dot + 1 == app_id.size())
            return app_id;
        std::string name = app_id.substr(dot + 1);
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        return name;
    }

    bool should_show_desktop_popup(const Notification& notification) { return notification.app_id != APP_ID_MESSAGES; }

    std::vector<std::string> icon_candidates(const Notification& notification) {
        std::vector<std::string> out;

        // Brand logos, named the way icon themes that carry them do (Papirus is
        // the common one; Breeze and Adwaita ship none of these, which is why
        // the category names below have to stand on their own).
        static const std::map<std::string, const char*> by_app_id{
            {"com.amazon.Amazon", "amazon"},
            {"com.apple.MobileAddressBook", "x-office-address-book"},
            {"com.apple.MobileSMS", "internet-chat"},
            {"com.apple.Music", "multimedia-player"},
            {"com.apple.facetime", "camera-web"},
            {"com.apple.mobilecal", "office-calendar"},
            {"com.apple.mobilemail", "mail-unread"},
            {"com.apple.mobilenotes", "accessories-text-editor"},
            {"com.apple.mobilephone", "call-start"},
            {"com.apple.mobileslideshow", "multimedia-photo-viewer"},
            {"com.apple.news", "application-rss+xml"},
            {"com.apple.podcasts", "podcasts"},
            {"com.apple.reminders", "view-task"},
            {"com.atebits.Tweetie2", "twitter"},
            {"com.burbn.instagram", "instagram"},
            {"com.facebook.Facebook", "facebook"},
            {"com.facebook.Messenger", "messenger"},
            {"com.github.stormbreaker.prod", "github"},
            {"com.google.Gmail", "gmail"},
            {"com.google.Maps", "google-maps"},
            {"com.google.ios.youtube", "youtube"},
            {"com.hammerandchisel.discord", "discord"},
            {"com.linkedin.LinkedIn", "linkedin"},
            {"com.microsoft.Office.Outlook", "ms-outlook"},
            {"com.reddit.Reddit", "reddit"},
            {"com.spotify.client", "spotify"},
            {"com.tinyspeck.chatlyio", "slack"},
            {"com.toyopagroup.picaboo", "snapchat"},
            {"com.ubercab.UberClient", "uber"},
            {"com.zhiliaoapp.musically", "tiktok"},
            {"net.whatsapp.WhatsApp", "whatsapp"},
            {"org.telegram.messenger", "telegram"},
            {"org.whispersystems.signal", "signal-desktop"},
            {"ph.telegra.Telegraph", "telegram"},
            {"us.zoom.videomeetings", "zoom"},
        };

        if (auto found = by_app_id.find(notification.app_id); found != by_app_id.end())
            out.emplace_back(found->second);

        for (const char* name : category_icons(notification.category))
            out.emplace_back(name);

        out.emplace_back("tether");
        return out;
    }

    LaunchTarget launch_target(const std::string& app_id) {
        static const std::map<std::string, LaunchTarget> by_app_id{
            {"com.apple.mobilemail", {"mailto", {}}},
            {"com.facebook.Messenger", {"", {"caprine", "com.sindresorhus.Caprine"}}},
            {"com.google.Gmail", {"mailto", {}}},
            {"com.hammerandchisel.discord", {"discord", {"discord", "com.discordapp.Discord"}}},
            {"com.microsoft.Office.Outlook", {"mailto", {}}},
            {"com.spotify.client", {"spotify", {"spotify", "com.spotify.Client"}}},
            {"com.tinyspeck.chatlyio", {"slack", {"slack", "com.slack.Slack"}}},
            {"net.whatsapp.WhatsApp",
             {"whatsapp",
              {"com.rtosta.zapzap",
               "io.github.mimbrero.WhatsAppDesktop",
               "com.github.eneshecan.WhatsAppForLinux",
               "whatsapp-for-linux"}}},
            {"org.telegram.messenger", {"tg", {"org.telegram.desktop", "telegramdesktop", "telegram-desktop"}}},
            {"org.whispersystems.signal", {"sgnl", {"signal-desktop", "signal", "org.signal.Signal"}}},
            {"ph.telegra.Telegraph", {"tg", {"org.telegram.desktop", "telegramdesktop", "telegram-desktop"}}},
            {"us.zoom.videomeetings", {"zoommtg", {"Zoom", "us.zoom.Zoom"}}},
        };

        if (auto found = by_app_id.find(app_id); found != by_app_id.end())
            return found->second;
        return {};
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
