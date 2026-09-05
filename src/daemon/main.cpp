#include "notification.hpp"
#include <csignal>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sys/timerfd.h>
#include <tether/bluetooth/config.hpp>
#include <tether/bluetooth/connection.hpp>
#include <tether/bluetooth/contacts.hpp>
#include <tether/bluetooth/monitor.hpp>
#include <tether/core.hpp>
#include <tether/crypto.hpp>
#include <tether/discovery.hpp>
#include <tether/event_loop.hpp>
#include <tether/file_transfer.hpp>
#include <tether/i18n.hpp>
#include <tether/log.hpp>
#include <tether/net.hpp>
#include <tether/otp.hpp>
#include <tether/secret_store.hpp>
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

// Append stderr/stdout to the state-dir log unless attached to a terminal.
static void redirect_output_to_log() {
    if (isatty(STDERR_FILENO))
        return;
    try {
        const std::string log_path = tether::get_state_dir() + "/tetherd.log";
        if (freopen(log_path.c_str(), "a", stderr) == nullptr)
            return;
        if (freopen(log_path.c_str(), "a", stdout) == nullptr) {
        }
    } catch (const std::exception&) {
        // Losing the log is survivable; failing to start is not.
    }
}

int main(int argc, char** argv) {
    redirect_output_to_log();
    tether::init_locale();
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

    tether::TcpServer tcp_srv(loop, 5134);
    tether::UnixServer unix_srv(loop, tcp_srv);
    if (!unix_srv.start()) {
        debug::log(ERR, "Failed to start Unix server");
        return 1;
    }

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

        discovery.set_state_callback([](bool available) { tether::set_mdns_available(available); });
        discovery.publish(hostname, 5134, my_fp);

        discovery.start_continuous_browse([&loop, &tcp_srv](const std::vector<tether::DiscoveredDevice>& devices) {
            // Reestablish sessions with peers we already trust.
            const std::string my_fp = tether::Crypto::instance().get_my_fingerprint();
            for (const auto& dev : devices) {
                if (dev.addresses.empty())
                    continue;
                if (!tether::should_dial_peer(
                        my_fp, dev.fingerprint, tether::Crypto::instance().is_host_known(dev.fingerprint)))
                    continue;

                // on avahi's poll thread
                const auto& addr = dev.addresses.front();
                loop.post([&tcp_srv, host = addr.address, port = addr.port, name = dev.name, fp = dev.fingerprint]() {
                    tcp_srv.connect_peer(host, port, name, fp);
                });
            }

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

        tcp_srv.set_peers_changed_callback([&discovery] { discovery.refresh(); });
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
    const bool notifier_ready = notifier.init();
    if (!notifier_ready) {
        debug::log(ERR, "Warning: desktop notifications unavailable");
    } else {
        file_mgr.set_on_complete([&notifier](const std::filesystem::path& path, size_t bytes_written) {
            notifier.notify_file_arrived(path);
            tether::record_received_file(path, bytes_written);
        });
    }
    tether::g_file_manager = &file_mgr;

    notifier.set_copy_handler([&loop](const std::string& code) {
        // libnotify dispatches actions on its own thread; the clipboard belongs to the loop.
        loop.post([code] {
            if (tether::g_wayland)
                tether::g_wayland->copy_to_clipboard(code);
        });
    });

    if (notifier_ready) {
        int mdns_warn_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (mdns_warn_fd >= 0) {
            itimerspec spec{};
            spec.it_value.tv_sec = 15;
            timerfd_settime(mdns_warn_fd, 0, &spec, nullptr);
            loop.addFd(mdns_warn_fd, [&loop, &notifier](int fd) {
                uint64_t ticks = 0;
                ssize_t ignored = read(fd, &ticks, sizeof(ticks));
                (void)ignored;
                loop.removeFd(fd);
                close(fd);
                if (tether::mdns_available())
                    return;
                debug::log(ERR, "mDNS: still unavailable after 15s; notifying the user");
                notifier.notify({_("Tether"),
                                 _("This PC can't be discovered"),
                                 _("avahi-daemon isn't running, so Tether can't advertise itself on the "
                                   "network. Start it with: sudo systemctl enable --now avahi-daemon"),
                                 {"network-wireless-offline", "network-offline", "dialog-warning"},
                                 false,
                                 ""});
            });
        }
    }

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

            std::string otp;
            if (!backfill && !message.outgoing) {
                otp = tether::otp_extract(message.body);
                tether::otp_publish(otp);
            }

            // ANCS deliberately shows no popup for Messages, because MAP is the
            // copy whose read state stays in sync with the phone. This is that popup.
            if (backfill || message.outgoing || message.read)
                return;

            std::string who;
            {
                std::lock_guard<std::mutex> lock(tether::bluetooth::message_store_mutex());
                who = tether::bluetooth::contact_store().name_for(message.thread_key);
            }
            if (who.empty())
                who = message.peer_name.empty() ? message.peer_address : message.peer_name;
            tether::bluetooth::ancs::Notification as_notification;
            as_notification.app_id = tether::bluetooth::ancs::APP_ID_MESSAGES;
            // The reply action is only offered for a thread that has a route back.
            tether::bluetooth::Recipient recipient;
            std::string reply_error;
            const bool repliable =
                tether::bluetooth::recipient_from_thread_key(message.thread_key, recipient, reply_error);
            notifier.notify({_("Messages"),
                             who.empty() ? "iPhone" : who,
                             message.body,
                             tether::bluetooth::ancs::icon_candidates(as_notification),
                             false,
                             repliable ? message.thread_key : std::string{},
                             otp});
        });

    auto bt_config = tether::bluetooth::load_config();

    tether::secret::set_retention(bt_config.retention);
    tether::set_desktop_popups_enabled(bt_config.desktop_popups_enabled);

    // Pick the controller before the first capability, a second adapter never comes up bound to the wrong one.
    bluez.set_preferred_adapter(bt_config.adapter);
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
        tether::bluetooth::g_bt_connections = &connections;

        tether::bluetooth::set_group_replies_enabled(bt_config.group_messages_enabled &&
                                                     bt_config.ancs_content_enabled && bt_config.ancs_enabled);
        tether::bluetooth::reload_group_rosters();

        {
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

                    const std::string otp = tether::otp_extract(notification.title + "\n" + notification.subtitle +
                                                                "\n" + notification.body);
                    tether::otp_publish(otp);

                    nlohmann::json event = tether::bluetooth::ancs::to_json(notification);
                    event["command"] = "bt_notification";
                    tether::broadcast_local_event(event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));

                    // Messages already arrive over MAP with read state that stays
                    // in sync, so a popup here would be the second copy.
                    if (!tether::bluetooth::ancs::should_show_desktop_popup(notification))
                        return;

                    const std::string title = notification.title.empty() ? notification.app_name : notification.title;
                    // Some apps put the whole notification in the subtitle and leave the message empty.
                    const std::string body = notification.body.empty() ? notification.subtitle : notification.body;
                    notifier.notify({notification.app_name,
                                     title.empty() ? "iPhone" : title,
                                     body,
                                     tether::bluetooth::ancs::icon_candidates(notification),
                                     notification.silent,
                                     "",
                                     otp,
                                     notification.app_id});
                },
                [](uint32_t uid) {
                    nlohmann::json event;
                    event["command"] = "bt_notification_removed";
                    event["uid"] = uid;
                    tether::broadcast_local_event(event.dump());
                });
        }

        connections.set_call_handler([](const nlohmann::json& calls) {
            nlohmann::json event;
            event["command"] = "bt_calls";
            event["calls"] = calls;
            tether::broadcast_local_event(event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        });

        connections.start(tether::bluetooth::supervised_address(bt_config), bt_config.ancs_enabled);
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
