#include "tether/net.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tether/crypto.hpp"
#include "tether/file_transfer.hpp"
#include "tether/wayland.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <vector>

namespace tether {

    struct ClientSession {
        int fd;
        SSL* ssl; // nullptr for plain unix clients
    };

    // Global list of active connected sessions
    static std::map<int, ClientSession> active_sessions;
    static std::set<int> local_subscribers;

    struct ReceivedFileInfo {
        std::string path;
        std::string filename;
        size_t bytes_written = 0;
    };

    struct ConnectedClientSnapshot {
        std::string address;
        std::string fingerprint;
        std::string device_name;
        bool paired = false;
    };

    static std::vector<ReceivedFileInfo> recent_received_files;
    static constexpr size_t kMaxRecentReceivedFiles = 16;
    static std::map<int, ConnectedClientSnapshot> connected_remote_clients;

    void register_client_fd(int fd) { active_sessions[fd] = {fd, nullptr}; }

    void register_client_ssl(int fd, SSL* ssl) { active_sessions[fd] = {fd, ssl}; }

    void unregister_client_fd(int fd) {
        active_sessions.erase(fd);
        local_subscribers.erase(fd);
    }

    void register_local_subscriber(int fd) { local_subscribers.insert(fd); }

    void unregister_local_subscriber(int fd) { local_subscribers.erase(fd); }

    // Helper for robust SSL writes on non-blocking sockets.
    // Handles SSL_ERROR_WANT_WRITE by retrying.
    static int robust_ssl_write(SSL* ssl, const void* buf, int num) {
        if (!ssl)
            return -1;
        int total_written = 0;
        const char* p = static_cast<const char*>(buf);
        while (total_written < num) {
            int n = SSL_write(ssl, p + total_written, num - total_written);
            if (n <= 0) {
                int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                    continue; // Busy-wait retry for simplicity in this daemon
                }
                return n; // Fatal error
            }
            total_written += n;
        }
        return total_written;
    }

    void broadcast_message(const std::string& msg, int exclude_fd) {
        std::string packet = msg;
        if (packet.back() != '\n')
            packet += '\n';

        for (auto const& [fd, session] : active_sessions) {
            if (fd == exclude_fd)
                continue;
            if (session.ssl) {
                robust_ssl_write(session.ssl, packet.c_str(), packet.size());
            } else {
                write(fd, packet.c_str(), packet.size());
            }
        }
    }

    static void write_plain_packet(int fd, const std::string& packet) {
        write(fd, packet.c_str(), packet.size());
    }

    void broadcast_local_event(const std::string& msg, int exclude_fd) {
        std::string packet = msg;
        if (packet.empty() || packet.back() != '\n')
            packet += '\n';

        for (int fd : local_subscribers) {
            if (fd == exclude_fd)
                continue;
            write_plain_packet(fd, packet);
        }
    }

    static nlohmann::json load_pending_pairs() {
        std::string pending_path = get_runtime_dir() + "/pending_pairs.json";
        nlohmann::json pending = nlohmann::json::object();

        std::ifstream ifs(pending_path);
        if (!ifs.is_open()) {
            return pending;
        }

        try {
            pending = nlohmann::json::parse(ifs);
        } catch (...) {
            pending = nlohmann::json::object();
        }

        return pending;
    }

    static std::string lookup_known_host_name(const std::string& fingerprint) {
        try {
            auto known = nlohmann::json::parse(Crypto::instance().get_known_hosts_dump());
            if (known.contains(fingerprint) && known[fingerprint].is_string()) {
                return known[fingerprint].get<std::string>();
            }
        } catch (...) {
        }

        return "";
    }

    static nlohmann::json build_local_state_snapshot() {
        nlohmann::json snapshot;
        snapshot["command"] = "state_snapshot";

        nlohmann::json paired_devices = nlohmann::json::array();
        try {
            auto known = nlohmann::json::parse(Crypto::instance().get_known_hosts_dump());
            for (auto& [fingerprint, name_value] : known.items()) {
                nlohmann::json device;
                device["fingerprint"] = fingerprint;
                device["device_name"] = name_value.is_string() ? name_value.get<std::string>() : "Unknown Device";
                paired_devices.push_back(device);
            }
        } catch (...) {
        }
        snapshot["paired_devices"] = paired_devices;

        nlohmann::json pending_pairs = nlohmann::json::array();
        auto pending = load_pending_pairs();
        for (auto& [fingerprint, name_value] : pending.items()) {
            nlohmann::json item;
            item["fingerprint"] = fingerprint;
            item["device_name"] = name_value.is_string() ? name_value.get<std::string>() : "Unknown Device";
            pending_pairs.push_back(item);
        }
        snapshot["pending_pairs"] = pending_pairs;

        nlohmann::json connected_clients = nlohmann::json::array();
        for (auto const& [fd, client] : connected_remote_clients) {
            nlohmann::json item;
            item["fd"] = fd;
            item["address"] = client.address;
            item["fingerprint"] = client.fingerprint;
            item["device_name"] = client.device_name;
            item["paired"] = client.paired;
            connected_clients.push_back(item);
        }
        snapshot["connected_clients"] = connected_clients;
        snapshot["recent_received_files"] = nlohmann::json::array();
        for (const auto& file : recent_received_files) {
            nlohmann::json item;
            item["path"] = file.path;
            item["filename"] = file.filename;
            item["bytes_written"] = file.bytes_written;
            snapshot["recent_received_files"].push_back(item);
        }

        return snapshot;
    }

    void record_received_file(const std::filesystem::path& path, size_t bytes_written) {
        ReceivedFileInfo file;
        file.path = path.string();
        file.filename = path.filename().string();
        file.bytes_written = bytes_written;

        recent_received_files.insert(recent_received_files.begin(), file);
        if (recent_received_files.size() > kMaxRecentReceivedFiles) {
            recent_received_files.resize(kMaxRecentReceivedFiles);
        }

        nlohmann::json event;
        event["command"] = "file_received";
        event["path"] = file.path;
        event["filename"] = file.filename;
        event["bytes_written"] = file.bytes_written;
        broadcast_local_event(event.dump());
    }

    size_t broadcast_tcp_message(const std::string& msg, int exclude_fd) {
        std::string packet = msg;
        if (packet.back() != '\n')
            packet += '\n';

        size_t recipients = 0;
        for (auto const& [fd, session] : active_sessions) {
            if (fd == exclude_fd || !session.ssl)
                continue;

            robust_ssl_write(session.ssl, packet.c_str(), packet.size());
            recipients++;
        }

        return recipients;
    }

    std::string get_runtime_dir() {
        const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!xdg_runtime) {
            throw std::runtime_error("XDG_RUNTIME_DIR is not set");
        }
        std::filesystem::path tether_dir = std::filesystem::path(xdg_runtime) / "tether";
        if (!std::filesystem::exists(tether_dir)) {
            std::filesystem::create_directories(tether_dir);
            std::filesystem::permissions(
                tether_dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
        }
        return tether_dir.string();
    }

    void ensure_single_instance() {
        std::string lock_file = get_runtime_dir() + "/tetherd.lock";
        int fd = open(lock_file.c_str(), O_RDWR | O_CREAT, 0600);
        if (fd < 0) {
            throw std::system_error(errno, std::system_category(), "Failed to open lock file");
        }

        if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
            if (errno == EWOULDBLOCK) {
                std::cerr << "tetherd is already running." << std::endl;
                exit(1);
            } else {
                throw std::system_error(errno, std::system_category(), "Failed to lock file");
            }
        }
        // TODO: dont do this
        // we intentionally leak the fd so the lock is held for the lifetime of the process.
        // lock is automatically released by OS on exit.
    }

    // --- UnixServer ---

    UnixServer::UnixServer(EpollEventLoop& loop) : loop_(loop) { socket_path_ = get_runtime_dir() + "/tetherd.sock"; }

    UnixServer::~UnixServer() { stop(); }

    bool UnixServer::start() {
        server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            std::cerr << "Failed to create unix socket" << std::endl;
            return false;
        }

        // Set non-blocking
        int flags = fcntl(server_fd_, F_GETFL, 0);
        fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

        unlink(socket_path_.c_str());

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "Failed to bind unix socket: " << std::strerror(errno) << std::endl;
            return false;
        }

        if (listen(server_fd_, SOMAXCONN) < 0) {
            std::cerr << "Failed to listen on unix socket" << std::endl;
            return false;
        }

        loop_.addFd(server_fd_, [this](int fd) { handle_accept(fd); });
        std::cout << "UnixServer listening on " << socket_path_ << std::endl;
        return true;
    }

    void UnixServer::stop() {
        if (server_fd_ >= 0) {
            loop_.removeFd(server_fd_);
            close(server_fd_);
            server_fd_ = -1;
            unlink(socket_path_.c_str());
        }

        // Clean up any remaining clients
        std::vector<int> client_fds;
        for (auto const& [fd, _] : client_buffers_) {
            client_fds.push_back(fd);
        }
        for (int fd : client_fds) {
            loop_.removeFd(fd);
            close(fd);
            unregister_client_fd(fd);
            client_buffers_.erase(fd);
        }
    }

    void UnixServer::handle_accept(int fd) {
        int client_fd = accept(fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "UnixServer accept error: " << std::strerror(errno) << std::endl;
            }
            return;
        }

        // Set non-blocking
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        // Register client for broadcasts
        register_client_fd(client_fd);
        loop_.addFd(client_fd, [this](int cfd) { handle_client(cfd); });
        std::cout << "UnixServer: New connection (fd: " << client_fd << ")" << std::endl;
    }

    void UnixServer::handle_client(int client_fd) {
        // 64k buffer sizes for chunks
        char buf[65536];
        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            client_buffers_[client_fd] += std::string(buf, n);

            size_t pos;
            while ((pos = client_buffers_[client_fd].find('\n')) != std::string::npos) {
                std::string msg = client_buffers_[client_fd].substr(0, pos);
                client_buffers_[client_fd].erase(0, pos + 1);

                try {
                    nlohmann::json j = nlohmann::json::parse(msg);

                    if (j.contains("command") && j["command"] == "subscribe") {
                        register_local_subscriber(client_fd);
                        std::string payload = build_local_state_snapshot().dump() + "\n";
                        write(client_fd, payload.c_str(), payload.size());
                        continue;
                    } else if (j.contains("command") && j["command"] == "unsubscribe") {
                        unregister_local_subscriber(client_fd);
                        std::string payload = "{\"command\":\"unsubscribed\"}\n";
                        write(client_fd, payload.c_str(), payload.size());
                        continue;
                    } else if (j.contains("command") && j["command"] == "state_snapshot") {
                        std::string payload = build_local_state_snapshot().dump() + "\n";
                        write(client_fd, payload.c_str(), payload.size());
                        continue;
                    } else if (j.contains("command") && j["command"] == "clipboard_set" && j.contains("content")) {
                        std::string content = j["content"];
                        if (g_wayland)
                            g_wayland->copy_to_clipboard(content);
                    } else if (j.contains("command") && j["command"] == "clipboard_get") {
                        if (g_wayland) {
                            nlohmann::json resp;
                            resp["command"] = "clipboard_content";
                            resp["content"] = g_wayland->get_clipboard();
                            std::string payload = resp.dump() + "\n";
                            write(client_fd, payload.c_str(), payload.size());
                            continue;
                        }
                    } else if (j.contains("command") && j["command"] == "file_start") {
                        if (broadcast_tcp_message(msg) == 0) {
                            nlohmann::json resp;
                            resp["command"] = "error";
                            resp["message"] = "no_connected_mobile_client";
                            std::string payload = resp.dump() + "\n";
                            write(client_fd, payload.c_str(), payload.size());
                            continue;
                        }
                    } else if (j.contains("command") && j["command"] == "file_chunk") {
                        if (broadcast_tcp_message(msg) == 0) {
                            nlohmann::json resp;
                            resp["command"] = "error";
                            resp["message"] = "no_connected_mobile_client";
                            std::string payload = resp.dump() + "\n";
                            write(client_fd, payload.c_str(), payload.size());
                            continue;
                        }
                    } else if (j.contains("command") && j["command"] == "file_end") {
                        if (broadcast_tcp_message(msg) == 0) {
                            nlohmann::json resp;
                            resp["command"] = "error";
                            resp["message"] = "no_connected_mobile_client";
                            std::string payload = resp.dump() + "\n";
                            write(client_fd, payload.c_str(), payload.size());
                            continue;
                        }

                        nlohmann::json resp;
                        resp["command"] = "file_status";
                        resp["transfer_id"] = j["transfer_id"];
                        resp["status"] = "success";
                        std::string payload = resp.dump() + "\n";
                        write(client_fd, payload.c_str(), payload.size());
                        continue;
                    }
                } catch (...) {
                }

                std::string response = "OK\n";
                write(client_fd, response.c_str(), response.size());
            }
        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            // Disconnected
            std::cout << "UnixServer: Client disconnected (fd: " << client_fd << ")" << std::endl;
            client_buffers_.erase(client_fd);
            unregister_client_fd(client_fd);
            loop_.removeFd(client_fd);
            close(client_fd);
        }
    }

    // --- TcpServer ---

    TcpServer::TcpServer(EpollEventLoop& loop, int bind_port) : loop_(loop), bind_port_(bind_port) {}

    TcpServer::~TcpServer() { stop(); }

    bool TcpServer::start() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            std::cerr << "Failed to create tcp socket" << std::endl;
            return false;
        }

        int opt = 1;
        if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
            std::cerr << "TcpServer setsockopt reuse failed" << std::endl;
        }

        // Set non-blocking
        int flags = fcntl(server_fd_, F_GETFL, 0);
        fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(bind_port_);

        if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "Failed to bind tcp socket on port " << bind_port_ << ": " << std::strerror(errno)
                      << std::endl;
            return false;
        }

        if (listen(server_fd_, SOMAXCONN) < 0) {
            std::cerr << "Failed to listen on tcp socket" << std::endl;
            return false;
        }

        loop_.addFd(server_fd_, [this](int fd) { handle_accept(fd); });
        std::cout << "TcpServer listening on 0.0.0.0:" << bind_port_ << std::endl;
        return true;
    }

    void TcpServer::stop() {
        if (server_fd_ >= 0) {
            loop_.removeFd(server_fd_);
            close(server_fd_);
            server_fd_ = -1;
        }

        // Clean up all active clients and SSL sessions
        std::vector<int> client_fds;
        for (auto const& [fd, _] : active_ssl_) {
            client_fds.push_back(fd);
        }
        for (int fd : client_fds) {
            SSL* ssl = active_ssl_[fd];
            if (ssl)
                SSL_free(ssl);

            loop_.removeFd(fd);
            close(fd);
            unregister_client_fd(fd);

            active_ssl_.erase(fd);
            client_buffers_.erase(fd);
            ssl_handshake_complete_.erase(fd);
            client_paired_.erase(fd);
            client_info_.erase(fd);
            connected_remote_clients.erase(fd);
        }
    }

    void TcpServer::handle_accept(int fd) {
        sockaddr_in client_addr{};
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(fd, reinterpret_cast<sockaddr*>(&client_addr), &addrlen);
        if (client_fd < 0)
            return;

        // Set non-blocking
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), ip, INET_ADDRSTRLEN);
        std::cout << "TcpServer: New connection from " << ip << " (fd: " << client_fd << ")" << std::endl;

        // SSL Wrapping
        SSL* ssl = SSL_new(tether::Crypto::instance().get_server_context());
        SSL_set_fd(ssl, client_fd);
        SSL_set_accept_state(ssl);

        active_ssl_[client_fd] = ssl;
        ssl_handshake_complete_[client_fd] = false;
        client_paired_[client_fd] = false;
        client_info_[client_fd].address = ip;
        connected_remote_clients[client_fd].address = ip;

        // We do NOT register for broadcasts until SSL handshake is complete
        loop_.addFd(client_fd, [this](int cfd) { handle_client(cfd); });
    }

    void TcpServer::handle_client(int client_fd) {
        SSL* ssl = active_ssl_[client_fd];

        if (!ssl_handshake_complete_[client_fd]) {
            int ret = SSL_accept(ssl);
            if (ret == 1) {
                ssl_handshake_complete_[client_fd] = true;
                std::string print = Crypto::get_peer_fingerprint(ssl);
                client_info_[client_fd].fingerprint = print;
                connected_remote_clients[client_fd].fingerprint = print;
                if (Crypto::instance().is_host_known(print)) {
                    client_paired_[client_fd] = true;
                    client_info_[client_fd].paired = true;
                    connected_remote_clients[client_fd].paired = true;
                    std::string device_name = lookup_known_host_name(print);
                    if (!device_name.empty()) {
                        client_info_[client_fd].device_name = device_name;
                        connected_remote_clients[client_fd].device_name = client_info_[client_fd].device_name;
                    }
                    register_client_ssl(client_fd, ssl);

                    nlohmann::json event;
                    event["command"] = "client_connected";
                    event["address"] = client_info_[client_fd].address;
                    event["fingerprint"] = print;
                    event["device_name"] = client_info_[client_fd].device_name;
                    event["paired"] = true;
                    broadcast_local_event(event.dump());
                } else {
                    std::cout << "TcpServer: Untrusted client connected. Fingerprint: " << print << std::endl;
                }
            } else {
                int err = SSL_get_error(ssl, ret);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    return; // Wait for Epoll to re-trigger
                }

                // Handshake strictly failed
                SSL_free(ssl);
                active_ssl_.erase(client_fd);
                client_buffers_.erase(client_fd);
                ssl_handshake_complete_.erase(client_fd);
                client_paired_.erase(client_fd);
                client_info_.erase(client_fd);
                connected_remote_clients.erase(client_fd);
                unregister_client_fd(client_fd);
                loop_.removeFd(client_fd);
                close(client_fd);
                return;
            }
        }

        char buf[65536];
        int n = SSL_read(ssl, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            client_buffers_[client_fd] += std::string(buf, n);

            size_t pos;
            while ((pos = client_buffers_[client_fd].find('\n')) != std::string::npos) {
                std::string msg = client_buffers_[client_fd].substr(0, pos);
                client_buffers_[client_fd].erase(0, pos + 1);

                try {
                    nlohmann::json j = nlohmann::json::parse(msg);

                    if (!client_paired_[client_fd]) {
                        if (j.contains("command") && j["command"] == "pair_request") {
                            std::string print = Crypto::get_peer_fingerprint(ssl);
                            std::string dev_name = j.value("device_name", "Unknown Device");
                            client_info_[client_fd].device_name = dev_name;
                            connected_remote_clients[client_fd].device_name = dev_name;
                            std::cout << "[Pairing Request Pending] from " << dev_name
                                      << ". Accept by running: tether --accept " << print << std::endl;

                            // Persist the pending request so accept_device can retrieve the name
                            {
                                std::string pending_path = get_runtime_dir() + "/pending_pairs.json";
                                nlohmann::json pending;
                                std::ifstream ifs(pending_path);
                                if (ifs.is_open()) {
                                    try {
                                        pending = nlohmann::json::parse(ifs);
                                    } catch (...) {
                                    }
                                    ifs.close();
                                }
                                pending[print] = dev_name;
                                std::ofstream ofs(pending_path);
                                ofs << pending.dump(4);
                            }

                            nlohmann::json resp;
                            resp["command"] = "pair_pending";
                            std::string payload = resp.dump() + "\n";
                            robust_ssl_write(ssl, payload.c_str(), payload.size());

                            nlohmann::json event;
                            event["command"] = "pair_request_received";
                            event["fingerprint"] = print;
                            event["device_name"] = dev_name;
                            event["address"] = client_info_[client_fd].address;
                            broadcast_local_event(event.dump());

                            // Launch the layer-shell dialog for interactive approval
                            spawn_pair_dialog(client_fd, ssl, print, dev_name);
                        } else {
                            std::string rej = "{\"command\":\"error\",\"message\":\"unauthorized\"}\n";
                            robust_ssl_write(ssl, rej.c_str(), rej.size());
                        }
                        continue;
                    }

                    if (j.contains("command") && j["command"] == "clipboard_set" && j.contains("content")) {
                        std::string content = j["content"];
                        if (g_wayland)
                            g_wayland->copy_to_clipboard(content);
                        // Broadcast to everyone (including sender) to ensure robust transport
                        nlohmann::json bc;
                        bc["command"] = "clipboard_updated";
                        bc["content"] = content;
                        broadcast_message(bc.dump(), client_fd);
                    } else if (j.contains("command") && j["command"] == "clipboard_get") {
                        if (g_wayland) {
                            nlohmann::json resp;
                            resp["command"] = "clipboard_content";
                            resp["content"] = g_wayland->get_clipboard();
                            std::string payload = resp.dump() + "\n";
                            robust_ssl_write(ssl, payload.c_str(), payload.size());
                            continue;
                        }
                    } else if (j.contains("command") && j["command"] == "file_start") {
                        if (g_file_manager)
                            g_file_manager->handle_start(j["transfer_id"], j["filename"], j["size"]);
                    } else if (j.contains("command") && j["command"] == "file_chunk") {
                        if (g_file_manager)
                            g_file_manager->handle_chunk(j["transfer_id"], j["chunk_index"], j["data"]);
                    } else if (j.contains("command") && j["command"] == "file_end") {
                        if (g_file_manager && g_file_manager->handle_end(j["transfer_id"])) {
                            nlohmann::json resp;
                            resp["command"] = "file_status";
                            resp["transfer_id"] = j["transfer_id"];
                            resp["status"] = "success";
                            std::string payload = resp.dump() + "\n";
                            robust_ssl_write(ssl, payload.c_str(), payload.size());
                            continue;
                        }
                    }
                } catch (...) {
                }

                std::string response = "OK\n";
                robust_ssl_write(ssl, response.c_str(), response.size());
            }
        } else {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                return;
            }

            std::cout << "TcpServer: Client disconnected (fd: " << client_fd << ")" << std::endl;
            if (connected_remote_clients.count(client_fd)) {
                nlohmann::json event;
                event["command"] = "client_disconnected";
                event["address"] = connected_remote_clients[client_fd].address;
                event["fingerprint"] = connected_remote_clients[client_fd].fingerprint;
                event["device_name"] = connected_remote_clients[client_fd].device_name;
                event["paired"] = connected_remote_clients[client_fd].paired;
                broadcast_local_event(event.dump());
            }
            SSL_free(ssl);
            active_ssl_.erase(client_fd);
            client_buffers_.erase(client_fd);
            ssl_handshake_complete_.erase(client_fd);
            client_paired_.erase(client_fd);
            client_info_.erase(client_fd);
            connected_remote_clients.erase(client_fd);
            unregister_client_fd(client_fd);
            loop_.removeFd(client_fd);
            close(client_fd);
        }
    }

    void TcpServer::spawn_pair_dialog(int client_fd,
                                      SSL* ssl,
                                      const std::string& fingerprint,
                                      const std::string& device_name) {
        // Create a pipe so the parent can detect when the child exits via epoll
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            std::cerr << "spawn_pair_dialog: pipe() failed: " << std::strerror(errno) << std::endl;
            return;
        }

        pid_t pid = fork();
        if (pid < 0) {
            std::cerr << "spawn_pair_dialog: fork() failed: " << std::strerror(errno) << std::endl;
            close(pipefd[0]);
            close(pipefd[1]);
            return;
        }

        if (pid == 0) {
            // --- Child process ---
            close(pipefd[0]); // close read end
            // The write end stays open; it will close when this process exits,
            // signaling EOF to the parent's read end.

            // Build body text with truncated fingerprint for readability
            std::string short_fp = fingerprint;
            if (short_fp.size() > 16) {
                short_fp = short_fp.substr(0, 16) + "...";
            }
            std::string body = device_name + " (" + short_fp + ") wants to pair with this device.";

            // Try alongside the daemon binary first, then fall back to PATH
            std::filesystem::path self_path;
            try {
                self_path = std::filesystem::read_symlink("/proc/self/exe");
            } catch (...) {
            }
            std::string sibling = (self_path.parent_path() / "tether-dialog").string();

            execl(sibling.c_str(),
                  "tether-dialog",
                  "--title",
                  "Pairing Request",
                  "--body",
                  body.c_str(),
                  "--accept",
                  "Accept",
                  "--reject",
                  "Reject",
                  "--timeout",
                  "60",
                  nullptr);
            // If sibling path failed, try PATH
            execlp("tether-dialog",
                   "tether-dialog",
                   "--title",
                   "Pairing Request",
                   "--body",
                   body.c_str(),
                   "--accept",
                   "Accept",
                   "--reject",
                   "Reject",
                   "--timeout",
                   "60",
                   nullptr);
            // exec failed entirely
            std::cerr << "spawn_pair_dialog: exec failed: " << std::strerror(errno) << std::endl;
            _exit(3);
        }

        // --- Parent process ---
        close(pipefd[1]); // close write end

        // Set read end non-blocking for epoll
        int flags = fcntl(pipefd[0], F_GETFL, 0);
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

        pending_dialogs_[pipefd[0]] = {pid, client_fd, fingerprint, device_name};

        loop_.addFd(pipefd[0], [this](int read_fd) {
            // Pipe became readable → child exited (EOF on write end)
            char dummy;
            read(read_fd, &dummy, 1); // consume EOF

            auto it = pending_dialogs_.find(read_fd);
            if (it == pending_dialogs_.end()) {
                loop_.removeFd(read_fd);
                close(read_fd);
                return;
            }

            PendingPairDialog info = it->second;
            pending_dialogs_.erase(it);

            int status = 0;
            waitpid(info.pid, &status, 0);
            int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 3;

            if (exit_code == 0) {
                // User accepted — trust the device
                Crypto::instance().add_known_host(info.device_name, info.fingerprint);
                std::cout << "[Pairing Accepted] " << info.device_name << " (" << info.fingerprint << ")" << std::endl;

                // Mark as paired if still connected
                if (client_paired_.count(info.client_fd)) {
                    client_paired_[info.client_fd] = true;
                }
                if (client_info_.count(info.client_fd)) {
                    client_info_[info.client_fd].paired = true;
                    client_info_[info.client_fd].device_name = info.device_name;
                }
                if (connected_remote_clients.count(info.client_fd)) {
                    connected_remote_clients[info.client_fd].paired = true;
                    connected_remote_clients[info.client_fd].device_name = info.device_name;
                }

                if (active_ssl_.count(info.client_fd)) {
                    register_client_ssl(info.client_fd, active_ssl_[info.client_fd]);
                }

                nlohmann::json event;
                event["command"] = "pair_accepted";
                event["fingerprint"] = info.fingerprint;
                event["device_name"] = info.device_name;
                if (connected_remote_clients.count(info.client_fd)) {
                    event["address"] = connected_remote_clients[info.client_fd].address;
                }
                broadcast_local_event(event.dump());

                nlohmann::json connected_event;
                connected_event["command"] = "client_connected";
                connected_event["fingerprint"] = info.fingerprint;
                connected_event["device_name"] = info.device_name;
                connected_event["paired"] = true;
                if (connected_remote_clients.count(info.client_fd)) {
                    connected_event["address"] = connected_remote_clients[info.client_fd].address;
                }
                broadcast_local_event(connected_event.dump());

                // Notify the remote client
                if (active_ssl_.count(info.client_fd)) {
                    nlohmann::json resp;
                    resp["command"] = "pair_accepted";
                    std::string payload = resp.dump() + "\n";
                    robust_ssl_write(active_ssl_[info.client_fd], payload.c_str(), payload.size());
                }

                // Clean up pending_pairs.json
                std::string pending_path = get_runtime_dir() + "/pending_pairs.json";
                nlohmann::json pending;
                std::ifstream ifs(pending_path);
                if (ifs.is_open()) {
                    try {
                        pending = nlohmann::json::parse(ifs);
                    } catch (...) {
                    }
                    ifs.close();
                }
                if (pending.contains(info.fingerprint)) {
                    pending.erase(info.fingerprint);
                    std::ofstream ofs(pending_path);
                    ofs << pending.dump(4);
                }
            } else {
                std::cout << "[Pairing Rejected] " << info.device_name << " (exit code " << exit_code << ")"
                          << std::endl;

                nlohmann::json event;
                event["command"] = "pair_rejected";
                event["fingerprint"] = info.fingerprint;
                event["device_name"] = info.device_name;
                if (connected_remote_clients.count(info.client_fd)) {
                    event["address"] = connected_remote_clients[info.client_fd].address;
                }
                broadcast_local_event(event.dump());

                // Notify the remote client
                if (active_ssl_.count(info.client_fd)) {
                    nlohmann::json resp;
                    resp["command"] = "pair_rejected";
                    std::string payload = resp.dump() + "\n";
                    robust_ssl_write(active_ssl_[info.client_fd], payload.c_str(), payload.size());
                }
            }

            // Clean up the pipe and remove from epoll at the very end
            loop_.removeFd(read_fd);
            close(read_fd);
        });

        std::cout << "spawn_pair_dialog: Launched dialog (pid " << pid << ") for " << device_name << std::endl;
    }

} // namespace tether
