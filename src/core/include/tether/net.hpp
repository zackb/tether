#pragma once

#include "tether/event_loop.hpp"
#include <filesystem>
#include <map>
#include <openssl/ssl.h>
#include <string>
#include <sys/types.h>

namespace tether {

    // Check if tetherd is already running. Exits if another instance holds the lock.
    void ensure_single_instance();
    std::string get_runtime_dir();

    class TcpServer;
    class UnixServer;

    // Simple registry for active clients to support broadcasting
    void register_client_fd(int fd);
    void register_client_ssl(int fd, SSL* ssl);
    void unregister_client_fd(int fd);
    void broadcast_message(const std::string& msg, int exclude_fd = -1);
    void register_local_subscriber(int fd);
    void unregister_local_subscriber(int fd);
    void broadcast_local_event(const std::string& msg, int exclude_fd = -1);
    void record_received_file(const std::filesystem::path& path, size_t bytes_written);
    size_t broadcast_tcp_message(const std::string& msg, int exclude_fd = -1);

    class UnixServer {
    public:
        UnixServer(EpollEventLoop& loop);
        ~UnixServer();

        bool start();
        void stop();

    private:
        void handle_accept(int fd);
        void handle_client(int client_fd);

        EpollEventLoop& loop_;
        int server_fd_ = -1;
        std::string socket_path_;
        std::map<int, std::string> client_buffers_;
    };

    class TcpServer {
    public:
        // bind_port = 0 binds to any available port
        TcpServer(EpollEventLoop& loop, int bind_port = 5134);
        ~TcpServer();

        bool start();
        void stop();

    private:
        struct ConnectedClientInfo {
            std::string address;
            std::string fingerprint;
            std::string device_name;
            bool paired = false;
        };

        void handle_accept(int fd);
        void handle_client(int client_fd);
        void spawn_pair_dialog(int client_fd, SSL* ssl, const std::string& fingerprint, const std::string& device_name);

        EpollEventLoop& loop_;
        int server_fd_ = -1;
        int bind_port_;
        std::map<int, std::string> client_buffers_;
        std::map<int, SSL*> active_ssl_;
        std::map<int, bool> ssl_handshake_complete_;
        std::map<int, bool> client_paired_;
        std::map<int, ConnectedClientInfo> client_info_;

        // Track spawned dialog child processes
        struct PendingPairDialog {
            pid_t pid;
            int client_fd;
            std::string fingerprint;
            std::string device_name;
        };
        // keyed by the pipe read-fd we monitor in epoll
        std::map<int, PendingPairDialog> pending_dialogs_;
    };

} // namespace tether
