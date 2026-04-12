#pragma once

#include "tether/event_loop.hpp"
#include "tether/clipboard.hpp"
#include <memory>
#include <functional>
#include <string>

class CCWlDisplay;
class CCWlRegistry;
class CCWlSeat;
class CCZwlrDataControlManagerV1;
struct wl_display;

namespace tether {

class WaylandContext {
public:
    WaylandContext(EpollEventLoop& loop);
    ~WaylandContext();

    bool init();
    void set_clipboard_callback(std::function<void(const std::string&)> cb);
    void copy_to_clipboard(const std::string& text);

private:
    EpollEventLoop& loop_;
    wl_display* raw_display_ = nullptr;
    std::unique_ptr<CCWlDisplay> display_;
    std::unique_ptr<CCWlRegistry> registry_;
    
    std::unique_ptr<CCWlSeat> seat_;
    std::unique_ptr<CCZwlrDataControlManagerV1> data_control_manager_;
    std::unique_ptr<ClipboardManager> clipboard_;

    std::function<void(const std::string&)> clipboard_cb_;
};

} // namespace tether
