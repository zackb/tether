#include "tether/clipboard.hpp"
#include "wayland_protocols/wlr-data-control-unstable-v1.hpp"
#include "wayland_protocols/wayland.hpp"
#include <iostream>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

namespace tether {

ClipboardManager::ClipboardManager(CCZwlrDataControlManagerV1* manager, CCWlSeat* seat)
    : manager_(manager), seat_(seat) {
    
    auto proxy = manager_->sendGetDataDevice((wl_proxy*)seat_->resource());
    device_ = std::make_unique<CCZwlrDataControlDeviceV1>(proxy);

    device_->setDataOffer([this](CCZwlrDataControlDeviceV1*, wl_proxy* offer_proxy) {
        current_offer_ = std::make_unique<CCZwlrDataControlOfferV1>(offer_proxy);
        current_mimes_.clear();
        
        current_offer_->setOffer([this](CCZwlrDataControlOfferV1* offer, const char* mime_type) {
            current_mimes_.push_back(mime_type);
        });
    });

    device_->setSelection([this](CCZwlrDataControlDeviceV1*, wl_proxy* offer_proxy) {
        if (!offer_proxy) {
            if (cb_) cb_("");
            return;
        }

        if (!current_offer_) return;

        // Selection changed, negotiate best mime type
        std::string best_mime = "text/plain"; // fallback
        bool found = false;
        
        static const std::vector<std::string> priorities = {
            "text/plain;charset=utf-8",
            "UTF8_STRING",
            "text/plain"
        };

        for (const auto& p : priorities) {
            for (const auto& m : current_mimes_) {
                if (m == p) {
                    best_mime = m;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }

        int pipefs[2];
        if (pipe(pipefs) < 0) return;

        current_offer_->sendReceive(best_mime.c_str(), pipefs[1]);
        close(pipefs[1]);

        std::thread([this, fd = pipefs[0]]() {
            std::string result;
            char buf[1024];
            while (true) {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n <= 0) break;
                result.append(buf, n);
            }
            close(fd);

            if (cb_) {
                cb_(result);
            }
        }).detach();
    });
}

ClipboardManager::~ClipboardManager() {
    if (device_) {
        device_->sendDestroy();
    }
}

void ClipboardManager::set_update_callback(std::function<void(const std::string&)> cb) {
    cb_ = std::move(cb);
}

void ClipboardManager::copy(const std::string& text) {
    auto proxy = manager_->sendCreateDataSource();
    auto source = std::make_unique<CCZwlrDataControlSourceV1>(proxy);
    
    source->sendOffer("text/plain");
    source->sendOffer("UTF8_STRING");

    // Wayland uses detached thread approach nicely too for offering data
    std::string text_copy = text;
    source->setSend([text_copy](CCZwlrDataControlSourceV1* s, const char* mime_type, int32_t fd) {
        std::thread([text_copy, fd]() {
            write(fd, text_copy.c_str(), text_copy.size());
            close(fd);
        }).detach();
    });

    source->setCancelled([](CCZwlrDataControlSourceV1* s) {
        s->sendDestroy();
    });

    device_->sendSetSelection(source.release()); // Leak to compositor until cancelled
}

} // namespace tether
