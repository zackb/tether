#pragma once

#include <string>
#include <map>
#include <memory>
#include <fstream>
#include <filesystem>
#include <mutex>

namespace tether {

class FileReceiveManager {
public:
    FileReceiveManager();
    ~FileReceiveManager();

    bool handle_start(const std::string& transfer_id, const std::string& filename, size_t size);
    bool handle_chunk(const std::string& transfer_id, int chunk_index, const std::string& b64_data);
    bool handle_end(const std::string& transfer_id);

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
};

extern FileReceiveManager* g_file_manager;

}
