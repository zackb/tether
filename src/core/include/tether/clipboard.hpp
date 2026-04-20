#include "tether/event_loop.hpp"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <wayland-client.h>

class CCZwlrDataControlManagerV1;
class CCWlSeat;
class CCZwlrDataControlDeviceV1;
class CCZwlrDataControlOfferV1;

namespace tether {

    class ClipboardManager {
    public:
        ClipboardManager(CCZwlrDataControlManagerV1* manager,
                         CCWlSeat* seat,
                         EpollEventLoop& loop,
                         wl_display* display);
        ~ClipboardManager();

        // Set callback to receive native Wayland clipboard updates
        void set_update_callback(std::function<void(const std::string&)> cb);

        // Push text from Tether network to native Wayland clipboard
        void copy(const std::string& text);

    private:
        CCZwlrDataControlManagerV1* manager_;
        CCWlSeat* seat_;
        EpollEventLoop& loop_;
        wl_display* display_;
        std::unique_ptr<CCZwlrDataControlDeviceV1> device_;

        std::function<void(const std::string&)> cb_;
        std::map<wl_proxy*, std::unique_ptr<CCZwlrDataControlOfferV1>> offers_;
        std::map<wl_proxy*, std::vector<std::string>> offer_mimes_;
        std::map<int, std::string> pending_reads_;
    };

} // namespace tether
