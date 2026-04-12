#include "tether/event_loop.hpp"
#include <sys/epoll.h>
#include <unistd.h>
#include <stdexcept>
#include <iostream>

namespace tether {

EpollEventLoop::EpollEventLoop() : running_(false) {
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        throw std::runtime_error("Failed to create epoll file descriptor");
    }
}

EpollEventLoop::~EpollEventLoop() {
    stop();
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
    }
}

bool EpollEventLoop::addFd(int fd, Callback cb) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        std::cerr << "Failed to add fd to epoll: " << fd << std::endl;
        return false;
    }
    callbacks_[fd] = std::move(cb);
    return true;
}

bool EpollEventLoop::removeFd(int fd) {
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        return false;
    }
    callbacks_.erase(fd);
    return true;
}

void EpollEventLoop::run() {
    running_ = true;
    const int MAX_EVENTS = 10;
    struct epoll_event events[MAX_EVENTS];

    while (running_) {
        int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            std::cerr << "epoll_wait failed" << std::endl;
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            auto it = callbacks_.find(fd);
            if (it != callbacks_.end()) {
                // Execute callback for readable fd
                it->second(fd);
            }
        }
    }
}

void EpollEventLoop::stop() {
    running_ = false;
}

} // namespace tether
