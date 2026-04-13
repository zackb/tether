#include "tether/clipboard.hpp"
#include "wayland_protocols/wlr-data-control-unstable-v1.hpp"
#include "wayland_protocols/wayland.hpp"
#include <iostream>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

namespace tether {

ClipboardManager::ClipboardManager(CCZwlrDataControlManagerV1* manager, CCWlSeat* seat, EpollEventLoop& loop)
    : manager_(manager), seat_(seat), loop_(loop) {
    
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

        // Use non-blocking read end for the event loop
        fcntl(pipefs[0], F_SETFL, O_NONBLOCK);

        current_offer_->sendReceive(best_mime.c_str(), pipefs[1]);
        close(pipefs[1]);

        loop_.addFd(pipefs[0], [this](int fd) {
            char buf[4096];
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                pending_reads_[fd].append(buf, n);
            } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
                // Done reading or error
                std::string result = pending_reads_[fd];
                pending_reads_.erase(fd);
                loop_.removeFd(fd);
                close(fd);

                // Trim trailing nulls and whitespace
                while (!result.empty() && (result.back() == '\0' || isspace(result.back()))) {
                    result.pop_back();
                }

                if (cb_) {
                    cb_(result);
                }
            }
        });
    });
}

ClipboardManager::~ClipboardManager() {
    for (auto const& [fd, _] : pending_reads_) {
        loop_.removeFd(fd);
        close(fd);
    }
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

    // Capture text by value for the callback
    std::string text_to_send = text;
    source->setSend([text_to_send](CCZwlrDataControlSourceV1* s, const char* mime_type, int32_t fd) {
        // For writing, a detached thread is safer than reading because it doesn't 
        // trigger an immediate broadcast callback on its own; however, for 
        // strict single-threading we could use EPOLLOUT. 
        // Given that we are primarily fixing the READ data race, we'll keep 
        // the write simple but ensure it doesn't touch any manager state.
        std::thread([text_to_send, fd]() {
            write(fd, text_to_send.c_str(), text_to_send.size());
            close(fd);
        }).detach();
    });

    source->setCancelled([](CCZwlrDataControlSourceV1* s) {
        s->sendDestroy();
    });

    device_->sendSetSelection(source.release());
}

} // namespace tether
