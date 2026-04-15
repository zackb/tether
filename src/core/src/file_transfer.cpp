#include "tether/file_transfer.hpp"
#include "tether/base64.hpp"
#include <glib.h>
#include <iostream>

namespace tether {

FileReceiveManager* g_file_manager = nullptr;

FileReceiveManager::FileReceiveManager() {}
FileReceiveManager::~FileReceiveManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, t] : transfers_) {
        if (t->stream && t->stream->is_open()) {
            t->stream->close();
        }
    }
}

std::string FileReceiveManager::get_downloads_dir() {
    if (const char* downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD)) {
        return downloads;
    }

    if (const char* home = g_get_home_dir()) {
        return std::string(home) + "/Downloads";
    }

    return "Downloads";
}

std::filesystem::path FileReceiveManager::resolve_filename(const std::string& raw_name) {
    auto dl_dir = std::filesystem::path(get_downloads_dir());
    std::filesystem::create_directories(dl_dir);

    std::string safe_name = std::filesystem::path(raw_name).filename().string();
    if (safe_name.empty()) {
        safe_name = "received-file";
    }

    std::filesystem::path p = dl_dir / safe_name;
    if (!std::filesystem::exists(p)) return p;

    std::string stem = p.stem().string();
    std::string ext = p.extension().string();

    int counter = 1;
    while (true) {
        p = dl_dir / (stem + "(" + std::to_string(counter) + ")" + ext);
        if (!std::filesystem::exists(p)) return p;
        counter++;
    }
}

bool FileReceiveManager::handle_start(const std::string& transfer_id, const std::string& filename, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (transfers_.find(transfer_id) != transfers_.end()) return false;

    auto t = std::make_shared<Transfer>();
    t->transfer_id = transfer_id;
    t->expected_size = size;
    t->filepath = resolve_filename(filename);

    t->stream = std::make_unique<std::ofstream>(t->filepath, std::ios::binary);
    if (!t->stream->is_open()) {
        std::cerr << "FileReceiveManager: Failed to open output file: " << t->filepath << std::endl;
        return false;
    }

    transfers_[transfer_id] = t;
    std::cout << "FileReceiveManager: Started transfer " << transfer_id << " saving to " << t->filepath << std::endl;
    return true;
}

bool FileReceiveManager::handle_chunk(const std::string& transfer_id, int chunk_index, const std::string& b64_data) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) return false;

    auto& t = it->second;
    auto raw_data = base64_decode(b64_data);

    t->stream->write(reinterpret_cast<const char*>(raw_data.data()), raw_data.size());
    t->bytes_written += raw_data.size();

    return true;
}

bool FileReceiveManager::handle_end(const std::string& transfer_id) {
    std::function<void(const std::filesystem::path&)> on_complete;
    std::filesystem::path completed_path;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = transfers_.find(transfer_id);
        if (it == transfers_.end()) return false;

        auto& t = it->second;
        t->stream->close();

        completed_path = t->filepath;
        on_complete = on_complete_;

        std::cout << "FileReceiveManager: Finished transfer " << transfer_id << ". Wrote " << t->bytes_written << " bytes to " << t->filepath << std::endl;

        transfers_.erase(it);
    }

    if (on_complete) {
        on_complete(completed_path);
    }

    return true;
}

void FileReceiveManager::set_on_complete(std::function<void(const std::filesystem::path&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_complete_ = std::move(callback);
}

}
