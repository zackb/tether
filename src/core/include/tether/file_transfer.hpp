#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace tether {

    class FileReceiveManager {
    public:
        FileReceiveManager();
        ~FileReceiveManager();

        bool handle_start(const std::string& transfer_id, const std::string& filename, size_t size);
        bool handle_chunk(const std::string& transfer_id, int chunk_index, const std::string& b64_data);
        bool handle_end(const std::string& transfer_id);
        void set_on_complete(std::function<void(const std::filesystem::path&, size_t)> callback);

    private:
        std::string get_downloads_dir();
        std::filesystem::path resolve_filename(const std::string& raw_name);

        struct Transfer {
            std::string transfer_id;
            std::filesystem::path filepath;
            std::unique_ptr<std::ofstream> stream;
            size_t expected_size;
            size_t bytes_written = 0;
        };

        std::mutex mutex_;
        std::map<std::string, std::shared_ptr<Transfer>> transfers_;
        std::function<void(const std::filesystem::path&, size_t)> on_complete_;
    };

    extern FileReceiveManager* g_file_manager;

} // namespace tether
