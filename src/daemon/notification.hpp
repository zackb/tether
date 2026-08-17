#pragma once

#include <filesystem>
#include <memory>

namespace tether {

    class FileArrivalNotifier {
    public:
        FileArrivalNotifier();
        ~FileArrivalNotifier();

        FileArrivalNotifier(const FileArrivalNotifier&) = delete;
        FileArrivalNotifier& operator=(const FileArrivalNotifier&) = delete;

        bool init();
        void notify_file_arrived(const std::filesystem::path& path);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace tether
