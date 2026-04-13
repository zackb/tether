#include "tether/event_loop.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <map>

class CCZwlrDataControlManagerV1;
class CCWlSeat;
class CCZwlrDataControlDeviceV1;
class CCZwlrDataControlOfferV1;

namespace tether {

    class ClipboardManager {
    public:
        ClipboardManager(CCZwlrDataControlManagerV1* manager, CCWlSeat* seat, EpollEventLoop& loop);
        ~ClipboardManager();

        // Set callback to receive native Wayland clipboard updates
        void set_update_callback(std::function<void(const std::string&)> cb);

        // Push text from Tether network to native Wayland clipboard
        void copy(const std::string& text);

    private:
        CCZwlrDataControlManagerV1* manager_;
        CCWlSeat* seat_;
        EpollEventLoop& loop_;
        std::unique_ptr<CCZwlrDataControlDeviceV1> device_;

        std::function<void(const std::string&)> cb_;
        std::unique_ptr<CCZwlrDataControlOfferV1> current_offer_;
        std::vector<std::string> current_mimes_;
        std::map<int, std::string> pending_reads_;
    };

} // namespace tether
