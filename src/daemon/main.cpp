#include "notification.hpp"
#include <csignal>
#include <nlohmann/json.hpp>
#include <tether/core.hpp>
#include <tether/crypto.hpp>
#include <tether/discovery.hpp>
#include <tether/event_loop.hpp>
#include <tether/file_transfer.hpp>
#include <tether/log.hpp>
#include <tether/net.hpp>
#include <tether/wayland.hpp>
#include <unistd.h>

tether::EpollEventLoop* g_loop = nullptr;

void signal_handler(int) {
    if (g_loop) {
        debug::log(INFO, "\nStopping tetherd...");
        g_loop->stop();
        g_loop = nullptr;
    }
}

int main(int argc, char** argv) {
    debug::log(INFO, "tetherd version {}", tether::get_version());

    try {
        tether::ensure_single_instance();
    } catch (const std::exception& e) {
        debug::log(ERR, "Initialization error: {}", e.what());
        return 1;
    }

    if (!tether::Crypto::instance().init()) {
        debug::log(ERR, "Fatal: Failed to initialize OpenSSL mTLS engine.");
        return 1;
    }

    // Capture signals gracefully
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE to prevent crash on closed connections

    tether::EpollEventLoop loop;
    g_loop = &loop;

    tether::UnixServer unix_srv(loop);
    if (!unix_srv.start()) {
        debug::log(ERR, "Failed to start Unix server");
        return 1;
    }

    tether::TcpServer tcp_srv(loop, 5134);
    if (!tcp_srv.start()) {
        debug::log(ERR, "Failed to start TCP server");
        return 1;
    }

    // Advertise this daemon on the local network via mDNS
    tether::Discovery discovery;
    {
        char hostname[256] = {};
        gethostname(hostname, sizeof(hostname) - 1);
        std::string my_fp = tether::Crypto::instance().get_my_fingerprint();
        if (!discovery.publish(hostname, 5134, my_fp)) {
            debug::log(ERR, "Warning: mDNS advertisement failed (is avahi-daemon running?)");
        }

        discovery.start_continuous_browse([](const std::vector<tether::DiscoveredDevice>& devices) {
            nlohmann::json payload;
            payload["command"] = "discovery_result";
            payload["devices"] = nlohmann::json::array();
            for (const auto& dev : devices) {
                nlohmann::json d;
                d["name"] = dev.name;
                d["fingerprint"] = dev.fingerprint;
                d["addresses"] = nlohmann::json::array();
                for (const auto& addr : dev.addresses) {
                    nlohmann::json a;
                    a["address"] = addr.address;
                    a["port"] = addr.port;
                    d["addresses"].push_back(a);
                }
                payload["devices"].push_back(d);
            }
            tether::broadcast_local_event(payload.dump());
        });
    }

    tether::WaylandContext wayland_srv(loop);
    tether::g_wayland = &wayland_srv;
    if (wayland_srv.init()) {
        wayland_srv.set_clipboard_callback([](const std::string& text) {
            nlohmann::json j;
            j["command"] = "clipboard_updated";
            j["content"] = text;
            tether::broadcast_message(j.dump());
        });
    }

    tether::FileReceiveManager file_mgr;
    tether::FileArrivalNotifier notifier;
    if (!notifier.init()) {
        debug::log(ERR, "Warning: desktop notifications unavailable");
    } else {
        file_mgr.set_on_complete([&notifier](const std::filesystem::path& path, size_t bytes_written) {
            notifier.notify_file_arrived(path);
            tether::record_received_file(path, bytes_written);
        });
    }
    tether::g_file_manager = &file_mgr;

    debug::log(INFO, "tetherd is running. Press Ctrl-C to stop.");
    loop.run();

    // discovery destructor calls unpublish() automatically
    debug::log(INFO, "tetherd shutdown complete.");

    // explicitly null globals to be safe during final stack unwinding
    tether::g_wayland = nullptr;
    tether::g_file_manager = nullptr;
    g_loop = nullptr;

    return 0;
}
