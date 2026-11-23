#pragma once

#include <functional>
#include <memory>

namespace tether {

    // Calls `on_closed` once when the session bus disconnects us. Uses a private
    // connection on its own thread and context, since GLib reports a close only there.
    class SessionBusWatcher {
    public:
        explicit SessionBusWatcher(std::function<void()> on_closed);
        ~SessionBusWatcher();

        SessionBusWatcher(const SessionBusWatcher&) = delete;
        SessionBusWatcher& operator=(const SessionBusWatcher&) = delete;

        // Connects to the session bus; returns false if there is none.
        bool start();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace tether
