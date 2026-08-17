#pragma once

#include <functional>
#include <nlohmann/json.hpp>

namespace tether::ui {

    using DaemonEventFn = std::function<void(const nlohmann::json&)>;

    // Connects to unix socket, subscribes to the local event feed, and dispatches each nd event. Starts the daemon once
    // if it is not running, and reconnects on its own after it goes away.
    void daemon_client_start(DaemonEventFn on_event);
    void daemon_client_stop();

    void daemon_send(const nlohmann::json& message);
    bool daemon_connected();

} // namespace tether::ui
