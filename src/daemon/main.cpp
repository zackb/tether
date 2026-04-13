#include <csignal>
#include <iostream>
#include <nlohmann/json.hpp>
#include <tether/core.hpp>
#include <tether/crypto.hpp>
#include <tether/discovery.hpp>
#include <tether/event_loop.hpp>
#include <tether/file_transfer.hpp>
#include <tether/net.hpp>
#include <tether/wayland.hpp>
#include <unistd.h>

tether::EpollEventLoop* g_loop = nullptr;

void signal_handler(int) {
    if (g_loop) {
        std::cout << "\nStopping tetherd..." << std::endl;
        g_loop->stop();
        g_loop = nullptr;
    }
}

int main(int argc, char** argv) {
    std::cout << "tetherd version " << tether::get_version() << "\n";

    try {
        tether::ensure_single_instance();
    } catch (const std::exception& e) {
        std::cerr << "Initialization error: " << e.what() << std::endl;
        return 1;
    }

    if (!tether::Crypto::instance().init()) {
        std::cerr << "Fatal: Failed to initialize OpenSSL mTLS engine." << std::endl;
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
        std::cerr << "Failed to start Unix server" << std::endl;
        return 1;
    }

    tether::TcpServer tcp_srv(loop, 5134);
    if (!tcp_srv.start()) {
        std::cerr << "Failed to start TCP server" << std::endl;
        return 1;
    }

    // Advertise this daemon on the local network via mDNS
    tether::Discovery discovery;
    {
        char hostname[256] = {};
        gethostname(hostname, sizeof(hostname) - 1);
        std::string my_fp = tether::Crypto::instance().get_my_fingerprint();
        if (!discovery.publish(hostname, 5134, my_fp)) {
            std::cerr << "Warning: mDNS advertisement failed (is avahi-daemon running?)" << std::endl;
        }
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
    tether::g_file_manager = &file_mgr;

    std::cout << "tetherd is running. Press Ctrl-C to stop." << std::endl;
    loop.run();

    // discovery destructor calls unpublish() automatically
    std::cout << "tetherd shutdown complete." << std::endl;

    // explicitly null globals to be safe during final stack unwinding
    tether::g_wayland = nullptr;
    tether::g_file_manager = nullptr;
    g_loop = nullptr;

    return 0;
}
