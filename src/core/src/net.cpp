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

#include "tether/file_transfer.hpp"
#include "tether/wayland.hpp"
#include "tether/crypto.hpp"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>

namespace tether {

    struct ClientSession {
        int fd;
        SSL* ssl; // nullptr for plain unix clients
    };

    // Global list of active connected sessions
    static std::map<int, ClientSession> active_sessions;

    void register_client_fd(int fd) { 
        active_sessions[fd] = {fd, nullptr}; 
    }

    void register_client_ssl(int fd, SSL* ssl) {
        active_sessions[fd] = {fd, ssl};
    }

    void unregister_client_fd(int fd) { 
        active_sessions.erase(fd); 
    }

    // Helper for robust SSL writes on non-blocking sockets.
    // Handles SSL_ERROR_WANT_WRITE by retrying.
    static int robust_ssl_write(SSL* ssl, const void* buf, int num) {
        if (!ssl) return -1;
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

    void broadcast_message(const std::string& msg) {
        std::string packet = msg;
        if (packet.back() != '\n')
            packet += '\n';

        for (auto const& [fd, session] : active_sessions) {
            if (session.ssl) {
                robust_ssl_write(session.ssl, packet.c_str(), packet.size());
            } else {
                write(fd, packet.c_str(), packet.size());
            }
        }
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

                    if (j.contains("command") && j["command"] == "clipboard_set" && j.contains("content")) {
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
                            write(client_fd, payload.c_str(), payload.size());
                            continue;
                        }
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
    }

    void TcpServer::handle_accept(int fd) {
        sockaddr_in client_addr{};
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(fd, reinterpret_cast<sockaddr*>(&client_addr), &addrlen);
        if (client_fd < 0) return;

        // Set non-blocking
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), ip, INET_ADDRSTRLEN);
        std::cout << "TcpServer: New connection from " << ip << " (fd: " << client_fd << ")" << std::endl;

        // SSL Wrapping
        SSL* ssl = SSL_new(Crypto::instance().get_server_context());
        SSL_set_fd(ssl, client_fd);
        SSL_set_accept_state(ssl);

        active_ssl_[client_fd] = ssl;
        ssl_handshake_complete_[client_fd] = false;
        client_paired_[client_fd] = false;

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
                if (Crypto::instance().is_host_known(print)) {
                    client_paired_[client_fd] = true;
                } else {
                    std::cout << "TcpServer: Untrusted client connected. Fingerprint: " << print << std::endl;
                }
                // Now that handshake is done, we can safely receive broadcasts
                register_client_ssl(client_fd, ssl);
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
                            std::cout << "[Pairing Request Pending] from " << dev_name 
                                      << ". Accept by running: tether --accept " << print << std::endl;

                            // Persist the pending request so accept_device can retrieve the name
                            {
                                std::string pending_path = get_runtime_dir() + "/pending_pairs.json";
                                nlohmann::json pending;
                                std::ifstream ifs(pending_path);
                                if (ifs.is_open()) {
                                    try { pending = nlohmann::json::parse(ifs); } catch (...) {}
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
                        broadcast_message(bc.dump());
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
            SSL_free(ssl);
            active_ssl_.erase(client_fd);
            client_buffers_.erase(client_fd);
            ssl_handshake_complete_.erase(client_fd);
            client_paired_.erase(client_fd);
            unregister_client_fd(client_fd);
            loop_.removeFd(client_fd);
            close(client_fd);
        }
    }

    void TcpServer::spawn_pair_dialog(int client_fd, SSL* ssl,
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
            try { self_path = std::filesystem::read_symlink("/proc/self/exe"); } catch (...) {}
            std::string sibling = (self_path.parent_path() / "tether-dialog").string();

            execl(sibling.c_str(), "tether-dialog",
                  "--title", "Pairing Request",
                  "--body", body.c_str(),
                  "--accept", "Accept",
                  "--reject", "Reject",
                  "--timeout", "60",
                  nullptr);
            // If sibling path failed, try PATH
            execlp("tether-dialog", "tether-dialog",
                   "--title", "Pairing Request",
                   "--body", body.c_str(),
                   "--accept", "Accept",
                   "--reject", "Reject",
                   "--timeout", "60",
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
            loop_.removeFd(read_fd);
            close(read_fd);

            int status = 0;
            waitpid(info.pid, &status, 0);
            int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 3;

            if (exit_code == 0) {
                // User accepted — trust the device
                Crypto::instance().add_known_host(info.device_name, info.fingerprint);
                std::cout << "[Pairing Accepted] " << info.device_name
                          << " (" << info.fingerprint << ")" << std::endl;

                // Mark as paired if still connected
                if (client_paired_.count(info.client_fd)) {
                    client_paired_[info.client_fd] = true;
                }

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
                    try { pending = nlohmann::json::parse(ifs); } catch (...) {}
                    ifs.close();
                }
                if (pending.contains(info.fingerprint)) {
                    pending.erase(info.fingerprint);
                    std::ofstream ofs(pending_path);
                    ofs << pending.dump(4);
                }
            } else {
                std::cout << "[Pairing Rejected] " << info.device_name
                          << " (exit code " << exit_code << ")" << std::endl;

                // Notify the remote client
                if (active_ssl_.count(info.client_fd)) {
                    nlohmann::json resp;
                    resp["command"] = "pair_rejected";
                    std::string payload = resp.dump() + "\n";
                    robust_ssl_write(active_ssl_[info.client_fd], payload.c_str(), payload.size());
                }
            }
        });

        std::cout << "spawn_pair_dialog: Launched dialog (pid " << pid << ") for "
                  << device_name << std::endl;
    }

} // namespace tether
