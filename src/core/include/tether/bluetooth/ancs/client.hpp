#pragma once

#include "tether/bluetooth/ancs/notifications.hpp"

#include <functional>
#include <memory>
#include <string>

namespace tether::bluetooth {
    class BluezMonitor;
}

namespace tether::bluetooth::ancs {

    // How often to retry discovery, subscription and the readiness probe.
    inline constexpr int ANCS_RETRY_SECONDS = 5;

    // How often a live subscription is re-confirmed against BlueZ.
    inline constexpr int ANCS_VERIFY_SECONDS = 30;

    // Defined in client.cpp.
    struct AncsClientState;

    // GATT client for Apple's notification service.
    //
    // Everything here is driven from one thread by tick(). BlueZ delivers
    // characteristic values on the GLib thread, which only buffers the bytes;
    // the sequencer and all D-Bus writes stay on the caller's thread, so no ANCS
    // state is touched from two places at once.
    class AncsClient {
    public:
        using NotificationFn = std::function<void(const Notification&)>;
        using WithdrawFn = std::function<void(uint32_t uid)>;
        // Reported when readiness changes; `reason` is shown to the user.
        using StatusFn = std::function<void(bool ready, const std::string& reason)>;

        AncsClient(BluezMonitor& monitor, NotificationFn on_notification, WithdrawFn on_withdraw, StatusFn on_status);
        ~AncsClient();

        AncsClient(const AncsClient&) = delete;
        AncsClient& operator=(const AncsClient&) = delete;

        // The BlueZ object path of the connected iPhone, or empty when LE drops.
        void set_device(const std::string& device_path);

        // Whether to ask for notification contents or only which app sent them.
        void set_content_enabled(bool enabled);

        // Ends a live subscription so the next tick resubscribes on whatever link
        // is up then. No-op when not subscribed.
        void drop_session(const std::string& reason);

        void tick(int64_t now);

        // Processes what has arrived since the last tick (replies, notifications, queued actions) and sends the
        // next request, without the rest of tick(). Same thread as tick().
        void pump(int64_t now);

        // Called from any thread when something arrives that pump() should process. Set before the first tick.
        void set_inbox_wake(std::function<void()> wake);

        bool ready() const;
        const std::string& status_reason() const;

        // Queues a notification's positive or negative action. ANCS offers only these two.
        bool perform_action(uint32_t uid, ActionId action);

        // Newest first. Safe from any thread.
        std::vector<Notification> recent_notifications(size_t limit = 50) const;

    private:
        std::unique_ptr<AncsClientState> state_;
    };

} // namespace tether::bluetooth::ancs
