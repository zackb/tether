#include "tether/wayland.hpp"
#include "wayland_protocols/ext-data-control-v1.hpp"
#include "wayland_protocols/wayland.hpp"
#include "wayland_protocols/wlr-data-control-unstable-v1.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/timerfd.h>
#include <tether/log.hpp>
#include <unistd.h>
#include <wayland-client.h>

namespace tether {

    WaylandContext* g_wayland = nullptr;

    WaylandContext::WaylandContext(EpollEventLoop& loop) : loop_(loop) {}

    std::vector<std::string> wayland_sockets(const std::filesystem::path& runtime_dir) {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(runtime_dir, ec)) {
            const std::string name = entry.path().filename().string();
            if (!name.starts_with("wayland-") || name.ends_with(".lock"))
                continue;
            if (entry.is_socket(ec))
                names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    WaylandContext::~WaylandContext() {
        g_wayland = nullptr;
        if (retry_fd_ >= 0) {
            loop_.removeFd(retry_fd_);
            close(retry_fd_);
        }
        disconnect();
    }

    void WaylandContext::disconnect() {
        if (!raw_display_)
            return;
        // Crucial: Destroy all regular proxies BEFORE disconnecting the display.
        clipboard_.reset();
        ext_data_control_manager_.reset();
        wlr_data_control_manager_.reset();
        seat_.reset();
        registry_.reset();

        // High-level wrapper for the display itself.
        if (display_) {
            display_.release();
        }

        loop_.removeFd(wl_display_get_fd(raw_display_));
        wl_display_disconnect(raw_display_);
        raw_display_ = nullptr;
        display_name_.clear();
    }

    // A unit started at boot runs before the compositor and without its
    // WAYLAND_DISPLAY, so fall back to any compositor socket in the runtime dir.
    bool WaylandContext::connect_display() {
        const char* env = std::getenv("WAYLAND_DISPLAY");
        const std::string env_name = env && *env ? env : "wayland-0";
        if ((raw_display_ = wl_display_connect(nullptr))) {
            display_name_ = env_name;
            return true;
        }
        const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime_dir || !*runtime_dir)
            return false;
        for (const auto& name : wayland_sockets(runtime_dir)) {
            if (name == env_name)
                continue;
            if ((raw_display_ = wl_display_connect(name.c_str()))) {
                display_name_ = name;
                return true;
            }
        }
        return false;
    }

    void WaylandContext::start_retry() {
        if (retry_fd_ >= 0)
            return;
        // libwayland can find no socket without it, and complains on every attempt.
        const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime_dir || !*runtime_dir) {
            debug::log(ERR, "WaylandContext: XDG_RUNTIME_DIR unset; clipboard sync unavailable.");
            return;
        }
        retry_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (retry_fd_ < 0) {
            debug::log(ERR, "WaylandContext: no compositor and no retry timer; clipboard sync unavailable.");
            return;
        }
        itimerspec spec{};
        spec.it_value.tv_sec = 2;
        spec.it_interval.tv_sec = 2;
        timerfd_settime(retry_fd_, 0, &spec, nullptr);
        debug::log(INFO, "WaylandContext: no Wayland display yet; waiting for a compositor.");
        loop_.addFd(retry_fd_, [this](int fd) {
            uint64_t ticks = 0;
            ssize_t ignored = read(fd, &ticks, sizeof(ticks));
            (void)ignored;
            if (!connect_display())
                return;
            loop_.removeFd(fd);
            close(fd);
            retry_fd_ = -1;
            setup();
        });
    }

    bool WaylandContext::init() {
        if (!connect_display()) {
            start_retry();
            return false;
        }
        return setup();
    }

    bool WaylandContext::setup() {
        display_ = std::make_unique<CCWlDisplay>((wl_proxy*)raw_display_);

        auto proxy = display_->sendGetRegistry();
        registry_ = std::make_unique<CCWlRegistry>(proxy);

        registry_->setGlobal([this](CCWlRegistry* r, uint32_t name, const char* interface, uint32_t version) {
            if (std::strcmp(interface, "wl_seat") == 0) {
                auto p = wl_registry_bind((wl_registry*)r->resource(), name, &wl_seat_interface, 1);
                seat_ = std::make_unique<CCWlSeat>((wl_proxy*)p);
            } else if (std::strcmp(interface, "zwlr_data_control_manager_v1") == 0) {
                auto p =
                    wl_registry_bind((wl_registry*)r->resource(), name, &zwlr_data_control_manager_v1_interface, 1);
                wlr_data_control_manager_ = std::make_unique<CCZwlrDataControlManagerV1>((wl_proxy*)p);
            } else if (std::strcmp(interface, "ext_data_control_manager_v1") == 0) {
                auto p = wl_registry_bind((wl_registry*)r->resource(), name, &ext_data_control_manager_v1_interface, 1);
                ext_data_control_manager_ = std::make_unique<CCExtDataControlManagerV1>((wl_proxy*)p);
            }
        });

        // synchronous roundtrips to populate globals and capture initial selection
        wl_display_roundtrip(raw_display_);
        wl_display_roundtrip(raw_display_);
        wl_display_roundtrip(raw_display_);

        if (!seat_) {
            debug::log(ERR, "WaylandContext: compositor exposes no wl_seat.");
            return false;
        }

        // Prefer the standardized protocol; KWin only implements this one.
        if (ext_data_control_manager_) {
            clipboard_ = std::make_unique<DataControlClipboard<CCExtDataControlManagerV1,
                                                               CCExtDataControlDeviceV1,
                                                               CCExtDataControlOfferV1,
                                                               CCExtDataControlSourceV1>>(
                ext_data_control_manager_.get(), (wl_proxy*)seat_->resource(), loop_, raw_display_);
        } else if (wlr_data_control_manager_) {
            clipboard_ = std::make_unique<DataControlClipboard<CCZwlrDataControlManagerV1,
                                                               CCZwlrDataControlDeviceV1,
                                                               CCZwlrDataControlOfferV1,
                                                               CCZwlrDataControlSourceV1>>(
                wlr_data_control_manager_.get(), (wl_proxy*)seat_->resource(), loop_, raw_display_);
        } else {
            debug::log(ERR,
                       "WaylandContext: compositor exposes neither ext_data_control_manager_v1 nor "
                       "zwlr_data_control_manager_v1; clipboard sync unavailable.");
            return false;
        }

        clipboard_->set_update_callback([this](const std::string& data, const std::string& mime) {
            const bool is_image = mime == CLIPBOARD_IMAGE_MIME;
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(clip_mutex_);
                if (is_image) {
                    changed = cached_clipboard_image_ != data;
                    cached_clipboard_image_ = data;
                } else {
                    changed = cached_clipboard_ != data || !cached_clipboard_image_.empty();
                    cached_clipboard_ = data;
                    cached_clipboard_image_.clear();
                }
            }
            if (!changed)
                return;
            if (is_image && clipboard_image_cb_) {
                clipboard_image_cb_(data);
            } else if (!is_image && clipboard_cb_) {
                clipboard_cb_(data);
            }
        });

        // Attach to event loop!
        int fd = wl_display_get_fd(raw_display_);
        loop_.addFd(fd, [this](int wfd) {
            struct pollfd pfd{wfd, 0, 0};
            if (poll(&pfd, 1, 0) == 1 && (pfd.revents & (POLLHUP | POLLERR))) {
                debug::log(ERR, "WaylandContext: compositor hung up; waiting for the next one.");
                disconnect();
                start_retry();
                return;
            }
            if (wl_display_dispatch(raw_display_) < 0) {
                debug::log(ERR, "WaylandContext: Display connection lost; waiting for a compositor.");
                disconnect();
                start_retry();
                return;
            }
            wl_display_flush(raw_display_); // flush requests queued during dispatch (offer/source destroys)
        });

        // Flush any pending requests
        wl_display_flush(raw_display_);

        debug::log(INFO, "WaylandContext initialized successfully on {}.", display_name_);
        return true;
    }

    void WaylandContext::set_clipboard_callback(std::function<void(const std::string&)> cb) {
        clipboard_cb_ = std::move(cb);
    }

    void WaylandContext::set_clipboard_image_callback(std::function<void(const std::string&)> cb) {
        clipboard_image_cb_ = std::move(cb);
    }

    void WaylandContext::copy_to_clipboard(const std::string& text) {
        std::string trimmed = text;
        while (!trimmed.empty() && (trimmed.back() == '\0' || isspace((unsigned char)trimmed.back()))) {
            trimmed.pop_back();
        }

        {
            std::lock_guard<std::mutex> lock(clip_mutex_);
            cached_clipboard_ = trimmed;
            cached_clipboard_image_.clear();
        }
        if (clipboard_) {
            clipboard_->copy(text);
            wl_display_flush(raw_display_);
        }
    }

    void WaylandContext::copy_image_to_clipboard(const std::string& png) {
        {
            // cached first, so the compositor echoing our own selection is not rebroadcast
            std::lock_guard<std::mutex> lock(clip_mutex_);
            cached_clipboard_image_ = png;
        }
        if (clipboard_) {
            clipboard_->copy_image(png);
            wl_display_flush(raw_display_);
        }
    }

    std::string WaylandContext::get_clipboard() {
        std::lock_guard<std::mutex> lock(clip_mutex_);
        return cached_clipboard_;
    }

    std::string WaylandContext::get_clipboard_image() {
        std::lock_guard<std::mutex> lock(clip_mutex_);
        return cached_clipboard_image_;
    }

} // namespace tether
