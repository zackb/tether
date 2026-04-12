#include "tether/file_transfer.hpp"
#include "tether/base64.hpp"
#include <iostream>
#include <stdlib.h>
#include <stdio.h>

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
    std::string path;
    FILE* pipe = popen("xdg-user-dir DOWNLOAD 2>/dev/null", "r");
    if (pipe) {
        char buffer[1024];
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            path = buffer;
            if (!path.empty() && path.back() == '\n') path.pop_back();
        }
        pclose(pipe);
    }
    if (path.empty() || path == getenv("HOME")) {
        const char* home = getenv("HOME");
        if (home) path = std::string(home) + "/Downloads";
    }
    return path;
}

std::filesystem::path FileReceiveManager::resolve_filename(const std::string& raw_name) {
    auto dl_dir = std::filesystem::path(get_downloads_dir());
    std::filesystem::create_directories(dl_dir);
    
    std::filesystem::path p = dl_dir / raw_name;
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
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) return false;
    
    auto& t = it->second;
    t->stream->close();
    
    std::cout << "FileReceiveManager: Finished transfer " << transfer_id << ". Wrote " << t->bytes_written << " bytes to " << t->filepath << std::endl;
    
    transfers_.erase(it);
    return true;
}

}
