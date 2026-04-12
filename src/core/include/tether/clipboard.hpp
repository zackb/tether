#pragma once

#include <string>
#include <functional>
#include <memory>

class CCZwlrDataControlManagerV1;
class CCWlSeat;
class CCZwlrDataControlDeviceV1;
class CCZwlrDataControlOfferV1;

namespace tether {

class ClipboardManager {
public:
    ClipboardManager(CCZwlrDataControlManagerV1* manager, CCWlSeat* seat);
    ~ClipboardManager();

    // Set callback to receive native Wayland clipboard updates
    void set_update_callback(std::function<void(const std::string&)> cb);

    // Push text from Tether network to native Wayland clipboard
    void copy(const std::string& text);

private:
    CCZwlrDataControlManagerV1* manager_;
    CCWlSeat* seat_;
    std::unique_ptr<CCZwlrDataControlDeviceV1> device_;
    
    std::function<void(const std::string&)> cb_;
    std::unique_ptr<CCZwlrDataControlOfferV1> current_offer_;
};

} // namespace tether
