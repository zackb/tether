#pragma once

#include <functional>
#include <map>

namespace tether {

    class EpollEventLoop {
    public:
        EpollEventLoop();
        ~EpollEventLoop();

        // Disable copy/move
        EpollEventLoop(const EpollEventLoop&) = delete;
        EpollEventLoop& operator=(const EpollEventLoop&) = delete;

        using Callback = std::function<void(int fd)>;

        // Add a file descriptor to be watched for EPOLLIN
        bool addFd(int fd, Callback cb);

        // Remove a file descriptor from being watched
        bool removeFd(int fd);

        // Run the event loop (blocks forever until stopped)
        void run();

        // Signal the event loop to stop
        void stop();

    private:
        int epoll_fd_;
        bool running_;
        std::map<int, Callback> callbacks_;
    };

} // namespace tether
