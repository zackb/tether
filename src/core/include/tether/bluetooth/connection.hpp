#pragma once

#include "tether/bluetooth/ancs/notifications.hpp"
#include "tether/bluetooth/bearer_supervisor.hpp"
#include "tether/bluetooth/groups.hpp"
#include "tether/bluetooth/messages.hpp"
#include "tether/bluetooth/profile_supervisor.hpp"
#include "tether/bluetooth/telephony.hpp"
#include "tether/secret_store.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace tether::bluetooth {

    class BluezMonitor;

    struct ConnectionState;

    // Drives the bearer and profile supervisors against the real BlueZ and obexd,
    // on its own thread, for the daemon's lifetime.
    class ConnectionManager {
    public:
        // Reports a combined status whenever it changes.
        using StatusFn = std::function<void(const nlohmann::json&)>;
        // Called once per newly seen message. `backfill` marks the first listing
        // after a MAP session opens (phone's existing inbox), so they belong in the UI but must not each do desktop
        // notification.
        using MessageFn = std::function<void(const Message& message, bool backfill)>;
        // Called once per mirrored notification, and when one is dismissed on
        // the phone.
        using NotificationFn = std::function<void(const ancs::Notification&)>;
        using WithdrawFn = std::function<void(uint32_t uid)>;
        // Called whenever the call list or the telephony service's availability changes.
        using CallsFn = std::function<void(const nlohmann::json& calls)>;

        ConnectionManager(BluezMonitor& monitor, StatusFn on_change, MessageFn on_message = {});

        // Enables notification mirroring. Without a handler the ANCS client is
        // never started, so a caller that cannot show notifications does not pay
        // for a GATT session it will not use.
        void set_notification_handlers(NotificationFn on_notification, WithdrawFn on_withdraw);

        // Turns notification mirroring on or off in place, on the next supervisor tick.
        void set_ancs_enabled(bool enabled);

        // Whether mirrored notifications carry their title and body, or only
        // which app sent them. Takes effect on the next notification.
        void set_ancs_content_enabled(bool enabled);

        // Whether the session locks when the phone drops out of range.
        void set_lock_on_away(bool enabled, int grace_seconds);

        // Invokes a mirrored notification's action.
        bool perform_notification_action(uint32_t uid, ancs::ActionId action);

        // Enables call control.
        void set_call_handler(CallsFn on_calls);
        void set_calls_enabled(bool enabled);

        // Publishes the call list now if it changed, instead of on the next supervisor tick.
        void refresh_calls();

        bool dial(const std::string& number, std::string& err);
        bool call_action(const std::string& path, const std::string& action, std::string& err);
        bool call_tones(const std::string& tones, std::string& err);
        nlohmann::json calls() const;
        // Whether the phone has Hands-Free up.
        bool calls_available() const;

        // The phone's recent calls, newest first: number, name, type
        void set_call_history_handler(CallsFn on_history);
        nlohmann::json call_history() const;

        nlohmann::json notifications(size_t limit = 50) const;
        ~ConnectionManager();

        ConnectionManager(const ConnectionManager&) = delete;
        ConnectionManager& operator=(const ConnectionManager&) = delete;

        // address is the bonded iPhone; empty disables supervision until set.
        bool start(const std::string& address, bool ancs_enabled);
        void stop();

        // `ancs_enabled` is the user's preference. Whether the controller can
        // actually carry ANCS is decided here rather than by the caller.
        void set_device(const std::string& address, bool ancs_enabled);

        nlohmann::json status() const;

    private:
        bool start_locked(const std::string& address, bool ancs_enabled);
        void stop_locked();

        std::unique_ptr<ConnectionState> state_;
    };

    nlohmann::json to_json(const BearerStatus& bearer, const ProfileStatus& profiles);

    // Conversation history for the selected iPhone.
    MessageStore& message_store();
    std::mutex& message_store_mutex();

    // Marks a message as read locally and on the phone (when MAP is up)
    // Returns false only when the handle is unknown.
    bool mark_message_read(const std::string& handle, bool read, std::string& err_out, bool* synced_out = nullptr);

    // Marks a batch read off the calling thread.
    void mark_messages_read_async(std::vector<std::string> handles, bool read);

    // Sends a reply to a one-to-one conversation. Returns false with err_out set
    // when MAP is down, the thread cannot be replied to, or the phone refused.
    bool send_message(const std::string& thread_key, const std::string& body, Message& sent_out, std::string& err_out);

    // Feeds the group correlator with an Apple Messages notification.
    void observe_message_notification(const std::string& sender,
                                      const std::string& subtitle,
                                      const std::string& body,
                                      int64_t now);

    // Why a group thread can or cannot be replied to, for the UI to show.
    ReplyEligibility group_reply_status(const std::string& thread_key, std::string& reason);

    // Closes the message journal and drops the in-memory history.
    void discard_retained_messages();

    // Follows a retention mode change on disk.
    void move_retained_store(Retention previous, Retention next);

    void set_group_replies_enabled(bool enabled);
    void reload_group_rosters();

    extern ConnectionManager* g_bt_connections;

} // namespace tether::bluetooth
