#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace tether {

    class DesktopNotifier {
    public:
        DesktopNotifier();
        ~DesktopNotifier();

        DesktopNotifier(const DesktopNotifier&) = delete;
        DesktopNotifier& operator=(const DesktopNotifier&) = delete;

        bool init();
        void notify_file_arrived(const std::filesystem::path& path);

        // A plain notification with no actions.
        void notify(const std::string& summary, const std::string& body);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace tether
