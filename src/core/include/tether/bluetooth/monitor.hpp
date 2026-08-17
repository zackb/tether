#pragma once

#include "tether/bluetooth/objects.hpp"

#include <functional>
#include <gio/gio.h>
#include <memory>

namespace tether::bluetooth {

    struct MonitorState;

    // Watches BlueZ on the system bus and keeps a current snapshot of its object tree.
    class BluezMonitor {
    public:
        BluezMonitor();
        ~BluezMonitor();

        BluezMonitor(const BluezMonitor&) = delete;
        BluezMonitor& operator=(const BluezMonitor&) = delete;

        // Connects to the system bus and starts the watcher thread. Returns false
        // if BlueZ is unreachable; the daemon runs on without Bluetooth support.
        bool start();
        void stop();
        bool running() const;

        // Level-triggered eventfd, readable whenever the snapshot has changed.
        // Register with EpollEventLoop::addFd and call drain() from the callback.
        int event_fd() const;
        void drain();

        BluezObjects snapshot() const;
        Capability capability() const;

        // Shared system-bus connection. GDBusConnection is thread-safe for call.
        GDBusConnection* connection() const;

        // Runs fn on the watcher thread and waits for it to finish.
        void invoke_sync(const std::function<void()>& fn);

    private:
        std::unique_ptr<MonitorState> impl_;
    };

    extern BluezMonitor* g_bluez;

} // namespace tether::bluetooth
