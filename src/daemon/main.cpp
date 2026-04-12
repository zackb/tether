#include <iostream>
#include <tether/core.hpp>
#include <tether/net.hpp>
#include <tether/event_loop.hpp>
#include <tether/wayland.hpp>
#include <tether/file_transfer.hpp>
#include <nlohmann/json.hpp>
#include <csignal>

tether::EpollEventLoop* g_loop = nullptr;

void signal_handler(int) {
    if (g_loop) {
        std::cout << "\nStopping tetherd..." << std::endl;
        g_loop->stop();
    }
}

int main(int argc, char** argv) {
    std::cout << "tetherd version " << tether::get_version() << "\n";
    
    try {
        tether::ensure_single_instance();
    } catch(const std::exception& e) {
        std::cerr << "Initialization error: " << e.what() << std::endl;
        return 1;
    }

    // Capture Ctrl-C gracefully and stop the event loop
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

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

    std::cout << "tetherd shutdown complete." << std::endl;
    return 0;
}
