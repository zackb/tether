#pragma once

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
    };

    class DesktopNotifier {
    public:
        DesktopNotifier();
        ~DesktopNotifier();

        DesktopNotifier(const DesktopNotifier&) = delete;
        DesktopNotifier& operator=(const DesktopNotifier&) = delete;

        bool init();

        // Where the "Copy Code" action sends the code. Called on the notifier's
        // own thread, so the handler is responsible for any thread hop it needs.
        void set_copy_handler(std::function<void(const std::string&)> handler);

        void notify_file_arrived(const std::filesystem::path& path);

        void notify(const NotificationSpec& spec);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace tether
