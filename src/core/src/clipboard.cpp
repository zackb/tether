#include "tether/clipboard.hpp"
#include "wayland_protocols/wayland.hpp"
#include "wayland_protocols/wlr-data-control-unstable-v1.hpp"
#include <cstring>
#include <fcntl.h>
#include <tether/log.hpp>
#include <thread>
#include <unistd.h>

namespace tether {

    ClipboardManager::ClipboardManager(CCZwlrDataControlManagerV1* manager,
                                       CCWlSeat* seat,
                                       EpollEventLoop& loop,
                                       wl_display* display)
        : manager_(manager), seat_(seat), loop_(loop), display_(display) {

        auto proxy = manager_->sendGetDataDevice((wl_proxy*)seat_->resource());
        device_ = std::make_unique<CCZwlrDataControlDeviceV1>(proxy);

        device_->setDataOffer([this](CCZwlrDataControlDeviceV1*, wl_proxy* offer_proxy) {
            auto offer = std::make_unique<CCZwlrDataControlOfferV1>(offer_proxy);

            // Clean up previous offers that were never selected to avoid memory growth,
            // but keep the current one and any very recent ones.
            // Wayland selection events usually follow data_offer events quickly.
            if (offers_.size() > 5) {
                offers_.erase(offers_.begin());
                offer_mimes_.erase(offer_mimes_.begin());
            }

            offer->setOffer([this, offer_proxy](CCZwlrDataControlOfferV1*, const char* mime_type) {
                offer_mimes_[offer_proxy].push_back(mime_type);
            });

            offers_[offer_proxy] = std::move(offer);
        });

        device_->setSelection([this](CCZwlrDataControlDeviceV1*, wl_proxy* offer_proxy) {
            if (!offer_proxy) {
                offers_.clear();
                offer_mimes_.clear();
                if (cb_)
                    cb_("");
                return;
            }

            auto it_mimes = offer_mimes_.find(offer_proxy);
            auto it_offer = offers_.find(offer_proxy);

            if (it_mimes == offer_mimes_.end() || it_offer == offers_.end()) {
                return;
            }

            const auto& current_mimes = it_mimes->second;
            auto& current_offer = it_offer->second;

            // Selection changed, negotiate best mime type
            std::string best_mime = "text/plain"; // fallback
            bool found = false;

            static const std::vector<std::string> priorities = {"text/plain;charset=utf-8",
                                                                "text/plain;charset=UTF-8",
                                                                "UTF8_STRING",
                                                                "text/plain;charset=utf8",
                                                                "text/plain",
                                                                "TEXT",
                                                                "STRING"};

            for (const auto& p : priorities) {
                for (const auto& m : current_mimes) {
                    if (m == p) {
                        best_mime = m;
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
            }

            // Catch-all fallback: if none of our preferred types are found,
            // try the first thing the compositor offered.
            if (!found && !current_mimes.empty()) {
                best_mime = current_mimes[0];
                found = true;
            }

            if (!found)
                return;

            int pipefs[2];
            if (pipe(pipefs) < 0)
                return;

            // Use non-blocking read end for the event loop
            fcntl(pipefs[0], F_SETFL, O_NONBLOCK);

            current_offer->sendReceive(best_mime.c_str(), pipefs[1]);
            close(pipefs[1]);
            wl_display_flush(display_);

            // Cleanup: remove all other offers as this one is now the active selection
            auto saved_offer = std::move(offers_[offer_proxy]);
            auto saved_mimes = std::move(offer_mimes_[offer_proxy]);
            offers_.clear();
            offer_mimes_.clear();
            offers_[offer_proxy] = std::move(saved_offer);
            offer_mimes_[offer_proxy] = std::move(saved_mimes);

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

    void ClipboardManager::set_update_callback(std::function<void(const std::string&)> cb) { cb_ = std::move(cb); }

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
                if (write(fd, text_to_send.c_str(), text_to_send.size()) < 0) {
                    debug::log(ERR, "clipboard write error\n");
                }
                close(fd);
            }).detach();
        });

        source->setCancelled([](CCZwlrDataControlSourceV1* s) { s->sendDestroy(); });

        device_->sendSetSelection(source.release());
    }

} // namespace tether
