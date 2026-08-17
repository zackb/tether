#include "notification.hpp"
#include <csignal>
#include <ctime>
#include <nlohmann/json.hpp>
#include <tether/bluetooth/config.hpp>
#include <tether/bluetooth/connection.hpp>
#include <tether/bluetooth/contacts.hpp>
#include <tether/bluetooth/monitor.hpp>
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
            // replace bad UTF-8 instead of throwing; a clipboard
            // app can still mislabel binary as text/plain. Don't abort the daemon.
            tether::broadcast_message(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        });
    }

    tether::FileReceiveManager file_mgr;
    tether::DesktopNotifier notifier;
    if (!notifier.init()) {
        debug::log(ERR, "Warning: desktop notifications unavailable");
    } else {
        file_mgr.set_on_complete([&notifier](const std::filesystem::path& path, size_t bytes_written) {
            notifier.notify_file_arrived(path);
            tether::record_received_file(path, bytes_written);
        });
    }
    tether::g_file_manager = &file_mgr;

    // BlueZ runs on its own GLib thread and wakes the loop through an eventfd
    // whenever the adapter or device set changes.
    tether::bluetooth::BluezMonitor bluez;
    tether::bluetooth::ConnectionManager connections(
        bluez,
        [](const nlohmann::json& status) { tether::broadcast_local_event(status.dump()); },
        [&notifier](const tether::bluetooth::Message& message, bool backfill) {
            nlohmann::json event = tether::bluetooth::to_json(message);
            event["command"] = "bt_message";
            tether::broadcast_local_event(event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));

            // ANCS deliberately shows no popup for Messages, because MAP is the
            // copy whose read state stays in sync with the phone. This is that popup.
            if (backfill || message.outgoing || message.read)
                return;

            std::string who = tether::bluetooth::contact_store().name_for(message.thread_key);
            if (who.empty())
                who = message.peer_name.empty() ? message.peer_address : message.peer_name;
            notifier.notify(who.empty() ? "iPhone" : who, message.body);
        });
    if (bluez.start()) {
        tether::bluetooth::g_bluez = &bluez;
        loop.addFd(bluez.event_fd(), [&bluez](int) {
            bluez.drain();
            tether::broadcast_local_event(tether::build_bt_status().dump());
        });
        auto cap = bluez.capability();
        debug::log(INFO, "Bluetooth: {} mode", tether::bluetooth::to_string(cap.mode));
        for (const auto& reason : cap.reasons)
            debug::log(INFO, "Bluetooth: {}", reason);

        // Supervise the selected iPhone's bearers and OBEX sessions. ANCS is only
        // attempted when the controller can carry it and pairing produced a bond
        // that covers LE.
        auto bt_config = tether::bluetooth::load_config();
        const bool ancs = bt_config.ancs_enabled && cap.mode == tether::bluetooth::DeliveryMode::Full;
        tether::bluetooth::g_bt_connections = &connections;

        tether::bluetooth::set_group_replies_enabled(bt_config.group_messages_enabled &&
                                                     bt_config.ancs_content_enabled);
        tether::bluetooth::reload_group_rosters();

        if (ancs) {
            connections.set_notification_handlers(
                [&notifier](const tether::bluetooth::ancs::Notification& notification) {
                    // Messages notifications are the only side channel that says
                    // which conversation a MAP message belongs to.
                    if (notification.app_id == tether::bluetooth::ancs::APP_ID_MESSAGES) {
                        tether::bluetooth::observe_message_notification(notification.title,
                                                                        notification.subtitle,
                                                                        notification.body,
                                                                        static_cast<int64_t>(std::time(nullptr)));
                    }

                    nlohmann::json event = tether::bluetooth::ancs::to_json(notification);
                    event["command"] = "bt_notification";
                    tether::broadcast_local_event(event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));

                    // Messages already arrive over MAP with read state that stays
                    // in sync, so a popup here would be the second copy.
                    if (!tether::bluetooth::ancs::should_show_desktop_popup(notification))
                        return;

                    const std::string title = notification.title.empty() ? notification.app_name : notification.title;
                    notifier.notify(title.empty() ? "iPhone" : title, notification.body);
                },
                [](uint32_t uid) {
                    nlohmann::json event;
                    event["command"] = "bt_notification_removed";
                    event["uid"] = uid;
                    tether::broadcast_local_event(event.dump());
                });
        }

        connections.start(bt_config.device_address, ancs);
    } else {
        debug::log(INFO, "Bluetooth unavailable; messages and notifications are disabled");
    }

    debug::log(INFO, "tetherd is running. Press Ctrl-C to stop.");
    loop.run();

    // discovery destructor calls unpublish() automatically
    debug::log(INFO, "tetherd shutdown complete.");

    // explicitly null globals to be safe during final stack unwinding
    tether::bluetooth::g_bt_connections = nullptr;
    connections.stop();
    tether::bluetooth::g_bluez = nullptr;
    bluez.stop();
    tether::g_wayland = nullptr;
    tether::g_file_manager = nullptr;
    g_loop = nullptr;

    return 0;
}
