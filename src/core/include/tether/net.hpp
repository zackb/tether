#pragma once

#include "tether/event_loop.hpp"
#include <string>

namespace tether {

    // Check if tetherd is already running. Exits if another instance holds the lock.
    void ensure_single_instance();
    std::string get_runtime_dir();

    class TcpServer;
    class UnixServer;

    // Simple registry for active clients to support broadcasting
    void register_client_fd(int fd);
    void unregister_client_fd(int fd);
    void broadcast_message(const std::string& msg);

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
        void handle_accept(int fd);
        void handle_client(int client_fd);

        EpollEventLoop& loop_;
        int server_fd_ = -1;
        int bind_port_;
        std::map<int, std::string> client_buffers_;
    };

} // namespace tether
