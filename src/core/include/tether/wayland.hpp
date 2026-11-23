#pragma once

#include "tether/clipboard.hpp"
#include "tether/event_loop.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class CCWlDisplay;
class CCWlRegistry;
class CCWlSeat;
class CCZwlrDataControlManagerV1;
class CCExtDataControlManagerV1;
struct wl_display;
class CCExtIdleNotifierV1;
class CCExtIdleNotificationV1;

namespace tether {

    class WaylandContext {
    public:
        WaylandContext(EpollEventLoop& loop);
        ~WaylandContext();

        // Connects now if a compositor is up, otherwise keeps retrying on the loop.
        // False only means clipboard sync is not available yet.
        bool init();

        // Socket name of the connected compositor, empty while disconnected.
        const std::string& display_name() const { return display_name_; }

        bool clipboard_available() const { return clipboard_ != nullptr; }
        void set_clipboard_callback(std::function<void(const std::string&)> cb);
        void set_clipboard_image_callback(std::function<void(const std::string&)> cb);
        void copy_to_clipboard(const std::string& text);
        void copy_image_to_clipboard(const std::string& png);
        std::string get_clipboard();
        std::string get_clipboard_image();

        // True when the clipboard runs on an X11 session rather than Wayland data-control.
        bool clipboard_on_x11() const { return x11_; }

    private:
        bool connect_display();
        bool setup();
        // Clipboard on the X11 session when there is no Wayland compositor (GNOME on Xorg, XFCE, ...).
        bool setup_x11();
        void attach_clipboard();
        void disconnect();
        void start_retry();
        void stop_retry();
        bool x11_ = false;

        std::mutex clip_mutex_;
        std::string cached_clipboard_;
        std::string cached_clipboard_image_;
        EpollEventLoop& loop_;
        wl_display* raw_display_ = nullptr;
        std::string display_name_;
        int retry_fd_ = -1;
        std::unique_ptr<CCWlDisplay> display_;
        std::unique_ptr<CCWlRegistry> registry_;

        std::unique_ptr<CCWlSeat> seat_;
        std::unique_ptr<CCZwlrDataControlManagerV1> wlr_data_control_manager_;
        std::unique_ptr<CCExtDataControlManagerV1> ext_data_control_manager_;
        std::unique_ptr<ClipboardManager> clipboard_;
        std::unique_ptr<CCExtIdleNotifierV1> idle_notifier_;
        std::unique_ptr<CCExtIdleNotificationV1> idle_notification_;

        std::function<void(const std::string&)> clipboard_cb_;
        std::function<void(const std::string&)> clipboard_image_cb_;
    };

    // Sorted wayland-* socket names in runtime_dir, lock files and non-sockets skipped.
    std::vector<std::string> wayland_sockets(const std::filesystem::path& runtime_dir);

    extern WaylandContext* g_wayland;

    // Whether the compositor reports no input for `yield_idle_seconds`. False
    // without a compositor or without ext-idle-notify-v1. Safe from any thread.
    bool wayland_idle();

} // namespace tether
