#include "tether/wayland.hpp"
#include "wayland_protocols/wayland.hpp"
#include "wayland_protocols/wlr-data-control-unstable-v1.hpp"

#include <wayland-client.h>
#include <iostream>
#include <cstring>

namespace tether {

WaylandContext* g_wayland = nullptr;

WaylandContext::WaylandContext(EpollEventLoop& loop) : loop_(loop) {}

WaylandContext::~WaylandContext() {
    if (raw_display_) {
        // remove from poll
        loop_.removeFd(wl_display_get_fd(raw_display_));
        wl_display_disconnect(raw_display_);
    }
}

bool WaylandContext::init() {
    raw_display_ = wl_display_connect(nullptr);
    if (!raw_display_) {
        std::cerr << "WaylandContext: Failed to connect to Wayland display" << std::endl;
        return false;
    }

    display_ = std::make_unique<CCWlDisplay>((wl_proxy*)raw_display_);
    
    auto proxy = display_->sendGetRegistry();
    registry_ = std::make_unique<CCWlRegistry>(proxy);

    registry_->setGlobal([this](CCWlRegistry* r, uint32_t name, const char* interface, uint32_t version) {
        if (std::strcmp(interface, "wl_seat") == 0) {
            auto p = wl_registry_bind((wl_registry*)r->resource(), name, &wl_seat_interface, 1);
            seat_ = std::make_unique<CCWlSeat>((wl_proxy*)p);
        } else if (std::strcmp(interface, "zwlr_data_control_manager_v1") == 0) {
            auto p = wl_registry_bind((wl_registry*)r->resource(), name, &zwlr_data_control_manager_v1_interface, 1);
            data_control_manager_ = std::make_unique<CCZwlrDataControlManagerV1>((wl_proxy*)p);
        }
    });

    // synchronous roundtrips to populate globals and capture initial selection
    wl_display_roundtrip(raw_display_);
    wl_display_roundtrip(raw_display_);
    wl_display_roundtrip(raw_display_);

    if (!seat_ || !data_control_manager_) {
        std::cerr << "WaylandContext: Failed to obtain necessary globals." << std::endl;
        return false;
    }

    clipboard_ = std::make_unique<ClipboardManager>(data_control_manager_.get(), seat_.get(), loop_);
    
    clipboard_->set_update_callback([this](const std::string& text) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(clip_mutex_);
            if (cached_clipboard_ != text) {
                cached_clipboard_ = text;
                changed = true;
            }
        }
        if (changed && clipboard_cb_) {
            clipboard_cb_(text);
        }
    });

    // Attach to event loop!
    int fd = wl_display_get_fd(raw_display_);
    loop_.addFd(fd, [this](int) {
        if (wl_display_dispatch(raw_display_) < 0) {
            std::cerr << "WaylandContext: Display disconnected." << std::endl;
        }
    });

    // Flush any pending requests
    wl_display_flush(raw_display_);

    std::cout << "WaylandContext initialized successfully." << std::endl;
    return true;
}

void WaylandContext::set_clipboard_callback(std::function<void(const std::string&)> cb) {
    clipboard_cb_ = std::move(cb);
}

void WaylandContext::copy_to_clipboard(const std::string& text) {
    std::string trimmed = text;
    while (!trimmed.empty() && (trimmed.back() == '\0' || isspace((unsigned char)trimmed.back()))) {
        trimmed.pop_back();
    }

    {
        std::lock_guard<std::mutex> lock(clip_mutex_);
        cached_clipboard_ = trimmed;
    }
    if (clipboard_) {
        clipboard_->copy(text);
        wl_display_flush(raw_display_);
    }
}

std::string WaylandContext::get_clipboard() {
    std::lock_guard<std::mutex> lock(clip_mutex_);
    return cached_clipboard_;
}

} // namespace tether
