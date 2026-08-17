#include "daemon_client.hpp"
#include "ui_util.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <gtk/gtk.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <tether/core.hpp>
#include <tether/log.hpp>
#include <tether/net.hpp>
#include <unistd.h>

namespace tether::ui {

    namespace {

        int g_event_fd = -1;
        GIOChannel* g_event_channel = nullptr;
        guint g_event_watch_id = 0;
        guint g_event_retry_id = 0;
        DaemonEventFn g_on_event;

        gboolean start_event_subscription(gpointer);

        void stop_event_subscription() {
            if (g_event_watch_id != 0) {
                g_source_remove(g_event_watch_id);
                g_event_watch_id = 0;
            }
            if (g_event_channel) {
                g_io_channel_shutdown(g_event_channel, TRUE, nullptr);
                g_io_channel_unref(g_event_channel);
                g_event_channel = nullptr;
            } else if (g_event_fd >= 0) {
                close(g_event_fd);
            }
            g_event_fd = -1;
        }

        void schedule_event_retry() {
            if (g_event_retry_id == 0)
                g_event_retry_id = g_timeout_add_seconds(2, start_event_subscription, nullptr);
        }

        gboolean on_event_channel(GIOChannel* source, GIOCondition condition, gpointer) {
            if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
                stop_event_subscription();
                schedule_event_retry();
                set_status_main("Daemon Offline");
                return FALSE;
            }
            if (condition & G_IO_IN) {
                gchar* line = nullptr;
                gsize length = 0;
                GError* error = nullptr;
                while (true) {
                    GIOStatus status = g_io_channel_read_line(source, &line, &length, nullptr, &error);
                    if (status == G_IO_STATUS_AGAIN) {
                        if (line)
                            g_free(line);
                        break;
                    }
                    if (status == G_IO_STATUS_EOF || status == G_IO_STATUS_ERROR) {
                        if (error) {
                            debug::log(ERR, "Stream err: {}", error->message);
                            g_error_free(error);
                        }
                        if (line)
                            g_free(line);
                        stop_event_subscription();
                        schedule_event_retry();
                        set_status_main("Daemon Offline");
                        return FALSE;
                    }
                    if (line && length > 0) {
                        try {
                            if (g_on_event)
                                g_on_event(nlohmann::json::parse(std::string(line, length)));
                        } catch (...) {
                        }
                    }
                    if (line)
                        g_free(line);
                }
            }
            return TRUE;
        }

        gboolean start_event_subscription(gpointer) {
            if (g_event_retry_id != 0) {
                g_source_remove(g_event_retry_id);
                g_event_retry_id = 0;
            }
            stop_event_subscription();
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) {
                schedule_event_retry();
                return G_SOURCE_REMOVE;
            }

            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::string path = tether::get_runtime_dir() + "/tetherd.sock";
            std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                close(fd);
                static bool autostart_attempted = false;
                if (!autostart_attempted) {
                    autostart_attempted = true;
                    pid_t pid = fork();
                    if (pid == 0) {
                        std::filesystem::path self_path = std::filesystem::read_symlink("/proc/self/exe");
                        std::string daemon_path = (self_path.parent_path() / "tetherd").string();
                        if (fork() == 0) {
                            if (freopen("/dev/null", "w", stdout) == nullptr) {
                            }
                            if (freopen("/dev/null", "w", stderr) == nullptr) {
                            }
                            if (freopen("/dev/null", "r", stdin) == nullptr) {
                            }
                            execl(daemon_path.c_str(), "tetherd", nullptr);
                            execlp("tetherd", "tetherd", nullptr);
                            exit(1);
                        }
                        exit(0);
                    } else if (pid > 0) {
                        int status;
                        waitpid(pid, &status, 0);
                    }
                }
                schedule_event_retry();
                set_status_main("Daemon Offline");
                return G_SOURCE_REMOVE;
            }

            g_event_fd = fd;
            g_event_channel = g_io_channel_unix_new(fd);
            g_io_channel_set_encoding(g_event_channel, nullptr, nullptr);
            g_io_channel_set_buffered(g_event_channel, TRUE);
            g_io_channel_set_flags(g_event_channel, G_IO_FLAG_NONBLOCK, nullptr);
            g_event_watch_id = g_io_add_watch(g_event_channel,
                                              static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
                                              on_event_channel,
                                              nullptr);
            static const char kSubscribe[] = "{\"command\":\"subscribe\"}\n";
            if (write(fd, kSubscribe, sizeof(kSubscribe) - 1) < 0) {
                debug::log(ERR, "subscribe write error\n");
            }
            set_status_main("Daemon Online");
            return G_SOURCE_REMOVE;
        }

    } // namespace

    void daemon_client_start(DaemonEventFn on_event) {
        g_on_event = std::move(on_event);
        start_event_subscription(nullptr);
    }

    void daemon_client_stop() {
        stop_event_subscription();
        if (g_event_retry_id != 0) {
            g_source_remove(g_event_retry_id);
            g_event_retry_id = 0;
        }
    }

    void daemon_send(const nlohmann::json& message) {
        if (g_event_fd >= 0) {
            std::string payload = message.dump() + "\n";
            if (::write(g_event_fd, payload.c_str(), payload.size()) < 0) {
                debug::log(ERR, "daemon write error\n");
            }
        }
    }

    bool daemon_connected() { return g_event_fd >= 0; }

} // namespace tether::ui
