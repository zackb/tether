#include "tether/client.hpp"
#include "tether/core.hpp"
#include "tether/crypto.hpp"
#include "tether/net.hpp"
#include "tether/base64.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tether {

Client::Client() {}

Client::~Client() {
    disconnect();
}

void Client::disconnect() {
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
}

bool Client::is_connected() const {
    return sock_ >= 0;
}

int Client::connect_unix(bool retry) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string path = tether::get_runtime_dir() + "/tetherd.sock";
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        if (!retry) return -1;

        pid_t pid = fork();
        if (pid == 0) {
            std::filesystem::path self_path = std::filesystem::read_symlink("/proc/self/exe");
            std::string daemon_path = (self_path.parent_path() / "tetherd").string();

            if (fork() == 0) {
                freopen("/dev/null", "w", stdout);
                freopen("/dev/null", "w", stderr);
                freopen("/dev/null", "r", stdin);

                execl(daemon_path.c_str(), "tetherd", nullptr);
                execlp("tetherd", "tetherd", nullptr);
                exit(1);
            }
            exit(0);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            usleep(300000);
            return connect_unix(false);
        }
        return -1;
    }
    return sock;
}

bool Client::connect(const std::string& host, int port) {
    disconnect();

    if (!host.empty()) {
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            return false;
        }

        tether::Crypto::instance().init();
        ssl_ = SSL_new(tether::Crypto::instance().get_client_context());
        SSL_set_fd(ssl_, sock_);
        if (SSL_connect(ssl_) <= 0) {
            return false;
        }
        return true;
    } else {
        sock_ = connect_unix(true);
        return sock_ >= 0;
    }
}

std::string Client::send_and_wait(const std::string& payload) {
    char buf[1024 * 1024]; // Generous payload buffer
    
    if (ssl_) {
        int w = SSL_write(ssl_, payload.c_str(), payload.size());
        if (w <= 0) return "";
        int n = SSL_read(ssl_, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            return std::string(buf);
        }
    } else {
        write(sock_, payload.c_str(), payload.size());
        ssize_t n = read(sock_, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            return std::string(buf);
        }
    }
    return "";
}

std::string Client::get_clipboard(std::string& err_out) {
    nlohmann::json j;
    j["command"] = "clipboard_get";
    std::string response = send_and_wait(j.dump() + "\n");
    
    try {
        nlohmann::json r = nlohmann::json::parse(response);
        if (r.contains("content")) {
            return r["content"].get<std::string>();
        } else if (r.contains("message") && r["message"] == "unauthorized") {
            err_out = "Unauthorized. You must pair this device first.";
        }
    } catch (...) {}
    return "";
}

bool Client::set_clipboard(const std::string& text, std::string& err_out) {
    nlohmann::json j;
    j["command"] = "clipboard_set";
    j["content"] = text;
    std::string s_resp = send_and_wait(j.dump() + "\n");
    if (s_resp.find("unauthorized") != std::string::npos) {
         err_out = "Unauthorized. Device not paired.";
         return false;
    }
    return true;
}

std::string Client::list_devices() {
    tether::Crypto::instance().init();
    return tether::Crypto::instance().get_known_hosts_dump();
}

bool Client::accept_device(const std::string& fingerprint, const std::string& name) {
    tether::Crypto::instance().init();
    tether::Crypto::instance().add_known_host(name, fingerprint);
    return true;
}

std::string Client::pair(const std::string& device_name, std::string& err_out) {
    if (!ssl_) {
        err_out = "Pairing requires a TCP+TLS connection. Use --host to specify a target.";
        return "";
    }
    nlohmann::json j;
    j["command"] = "pair_request";
    j["device_name"] = device_name;
    std::string response = send_and_wait(j.dump() + "\n");
    if (response.empty()) {
        err_out = "No response from daemon.";
    }
    return response;
}

bool Client::send_file(const std::string& path, std::string& err_out) {
    if (path.empty() || !std::filesystem::exists(path)) {
        err_out = "Invalid or missing file path.";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        err_out = "Failed to open file.";
        return false;
    }

    size_t file_size = std::filesystem::file_size(path);
    std::string filename = std::filesystem::path(path).filename().string();
    std::string transfer_id = "cli_" + std::to_string(time(nullptr));

    nlohmann::json j_start;
    j_start["command"] = "file_start";
    j_start["filename"] = filename;
    j_start["size"] = file_size;
    j_start["transfer_id"] = transfer_id;
    
    std::string s_resp = send_and_wait(j_start.dump() + "\n");
    if (s_resp.find("unauthorized") != std::string::npos) {
         err_out = "Unauthorized. Device not paired.";
         return false;
    }

    const size_t chunk_size = 512 * 1024;
    std::vector<unsigned char> buffer(chunk_size);
    int chunk_idx = 0;

    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), chunk_size);
        size_t bytes_read = file.gcount();
        if (bytes_read == 0) break;

        nlohmann::json j_chunk;
        j_chunk["command"] = "file_chunk";
        j_chunk["chunk_index"] = chunk_idx++;
        j_chunk["transfer_id"] = transfer_id;
        j_chunk["data"] = tether::base64_encode(buffer.data(), bytes_read);

        send_and_wait(j_chunk.dump() + "\n");
    }

    nlohmann::json j_end;
    j_end["command"] = "file_end";
    j_end["transfer_id"] = transfer_id;

    std::string resp = send_and_wait(j_end.dump() + "\n");
    try {
        nlohmann::json r = nlohmann::json::parse(resp);
        if (r.contains("status") && r["status"] == "success") {
            return true;
        }
    } catch (...) {
    }
    
    err_out = "Transfer sequence explicitly rejected externally.";
    return false;
}

} // namespace tether
