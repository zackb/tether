#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tether {

    // A mirrored notification as the desktop should present it.
    struct NotificationSpec {
        // The originating iPhone app. Shown as the popup's application name
        std::string app_name;
        std::string summary;
        std::string body;
        // Freedesktop icon names, best first.
        std::vector<std::string> icons;
        bool quiet = false;
        std::string reply_thread;
        // OTP found in this notification, or empty. Non-empty adds a copy action.
        std::string otp_code;
        // Originating iPhone bundle id.
        std::string app_id;
        // MAP handle to mark read when the code is copied. Empty for
        // notifications with no message behind them.
        std::string read_handle;
        // iPhone notification UID, or -1. Set, the popup closes on withdraw().
        int64_t ancs_uid = -1;
        // The iPhone can clear it, so dismissing the popup clears it there too.
        bool clearable = false;
    };

    class DesktopNotifier {
    public:
        DesktopNotifier();
        ~DesktopNotifier();

        DesktopNotifier(const DesktopNotifier&) = delete;
        DesktopNotifier& operator=(const DesktopNotifier&) = delete;

        bool init();

        // Where the "Copy Code" action sends the code, along with the spec's
        // read_handle. Called on the notifier's own thread, so the handler is
        // responsible for any thread hop it needs.
        void set_copy_handler(std::function<void(const std::string& code, const std::string& handle)> handler);

        // Where text typed into a message notification's inline reply field
        // goes, along with the spec's reply_thread. Only servers advertising
        // the "inline-reply" capability show the field; others keep the plain
        // Reply action. Called on the notifier's own thread.
        void set_reply_handler(std::function<void(const std::string& thread, const std::string& body)> handler);

        // Where the "Mute" action on a mirrored iPhone popup sends the app. Called
        // on the notifier's own thread.
        void set_mute_handler(std::function<void(const std::string& app_id, const std::string& app_name)> handler);

        // Where the Trust / Not now actions on a network prompt go. Notifier's own thread.
        void set_network_trust_handler(std::function<void(const std::string& key, bool trust)> handler);

        // Asks whether to show Tether on this network. Shown even with popups off: it is a setting.
        void notify_network_trust(const std::string& key, const std::string& name);

        // Where a mirrored iPhone popup the user dismissed sends its UID. Called on
        // the notifier's own thread.
        void set_dismiss_handler(std::function<void(uint32_t ancs_uid)> handler);

        void notify_file_arrived(const std::filesystem::path& path);

        void notify(const NotificationSpec& spec);

        // Closes the popup shown for this iPhone notification UID, if any.
        void withdraw(uint32_t ancs_uid);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace tether
