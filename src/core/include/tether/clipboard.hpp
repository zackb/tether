#pragma once

#include "tether/event_loop.hpp"
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tether/log.hpp>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace tether {

    inline constexpr const char* CLIPBOARD_IMAGE_MIME = "image/png";

    // MIME type to read from a selection offer. Text wins over an image.
    // Empty when nothing usable is offered.
    inline std::string pick_clipboard_mime(const std::vector<std::string>& offered) {
        static const std::vector<std::string> priorities = {"text/plain;charset=utf-8",
                                                            "text/plain;charset=UTF-8",
                                                            "UTF8_STRING",
                                                            "text/plain;charset=utf8",
                                                            "text/plain",
                                                            "TEXT",
                                                            "STRING",
                                                            CLIPBOARD_IMAGE_MIME};
        for (const auto& p : priorities) {
            for (const auto& m : offered) {
                if (m == p)
                    return m;
            }
        }
        return {};
    }

    // Clipboard access needs a data-control protocol. wlroots compositors expose
    // zwlr_data_control_v1, KWin exposes ext_data_control_v1. Both have the
    // exact same shape, so the implementation is reused.
    class ClipboardManager {
    public:
        virtual ~ClipboardManager() = default;

        // Callback for native clipboard updates: selection bytes and the MIME type read.
        // An empty selection reports ("", "").
        virtual void set_update_callback(std::function<void(const std::string&, const std::string&)> cb) = 0;

        // Push text from Tether network to the native clipboard
        virtual void copy(const std::string& text) = 0;
        // Push PNG bytes from Tether network to the native clipboard
        virtual void copy_image(const std::string& png) = 0;
    };

    template <typename Manager, typename Device, typename Offer, typename Source>
    class DataControlClipboard final : public ClipboardManager {
    public:
        DataControlClipboard(Manager* manager, wl_proxy* seat, EpollEventLoop& loop, wl_display* display)
            : manager_(manager), loop_(loop), display_(display) {

            device_ = std::make_unique<Device>(manager_->sendGetDataDevice(seat));

            device_->setDataOffer([this](Device*, wl_proxy* offer_proxy) {
                auto offer = std::make_unique<Offer>(offer_proxy);

                if (offers_.size() > 5) {
                    offers_.erase(offers_.begin());
                    offer_mimes_.erase(offer_mimes_.begin());
                }

                offer->setOffer([this, offer_proxy](Offer*, const char* mime_type) {
                    offer_mimes_[offer_proxy].push_back(mime_type);
                });

                offers_[offer_proxy] = std::move(offer);
            });

            device_->setSelection([this](Device*, wl_proxy* offer_proxy) { on_selection(offer_proxy); });
        }

        ~DataControlClipboard() override {
            for (auto const& [fd, _] : pending_reads_) {
                loop_.removeFd(fd);
                close(fd);
            }
            source_.reset(); // before the device that owns the selection
            if (device_) {
                device_->sendDestroy();
            }
        }

        void set_update_callback(std::function<void(const std::string&, const std::string&)> cb) override {
            cb_ = std::move(cb);
        }

        // GTK asks for text/plain;charset=utf-8 first and reads a bare text/plain as Latin-1,
        // so offer every text type wl-copy does.
        void copy(const std::string& text) override {
            offer(text, {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING"});
        }

        void copy_image(const std::string& png) override { offer(png, {CLIPBOARD_IMAGE_MIME}); }

    private:
        void offer(const std::string& data, std::initializer_list<const char*> mimes) {
            auto source = std::make_unique<Source>(manager_->sendCreateDataSource());

            for (const char* mime : mimes)
                source->sendOffer(mime);

            auto data_to_send = std::make_shared<const std::string>(data);
            source->setSend([data_to_send](Source*, const char*, int32_t fd) {
                // detached write thread: touches no manager state
                std::thread([data_to_send, fd]() {
                    // a pipe takes large payloads in pieces
                    for (size_t off = 0; off < data_to_send->size();) {
                        ssize_t n = write(fd, data_to_send->data() + off, data_to_send->size() - off);
                        if (n < 0 && errno == EINTR)
                            continue;
                        if (n <= 0) {
                            debug::log(ERR, "clipboard write error\n");
                            break;
                        }
                        off += static_cast<size_t>(n);
                    }
                    close(fd);
                }).detach();
            });

            // compositor cancels our source when something else takes the selection.
            source->setCancelled([this](Source* s) {
                if (source_.get() == s)
                    source_.reset();
            });

            device_->sendSetSelection(source.get());
            source_ = std::move(source);
        }

        void on_selection(wl_proxy* offer_proxy) {
            if (!offer_proxy) {
                offers_.clear();
                offer_mimes_.clear();
                if (cb_)
                    cb_("", "");
                return;
            }

            auto it_mimes = offer_mimes_.find(offer_proxy);
            auto it_offer = offers_.find(offer_proxy);

            if (it_mimes == offer_mimes_.end() || it_offer == offers_.end()) {
                return;
            }

            const std::string best_mime = pick_clipboard_mime(it_mimes->second);
            if (best_mime.empty())
                return;

            int pipefs[2];
            if (pipe(pipefs) < 0)
                return;

            // non-blocking read end for the event loop
            fcntl(pipefs[0], F_SETFL, O_NONBLOCK);

            it_offer->second->sendReceive(best_mime.c_str(), pipefs[1]);
            close(pipefs[1]);
            wl_display_flush(display_);

            // Cleanup: remove all other offers as this one is now the active selection
            auto saved_offer = std::move(it_offer->second);
            auto saved_mimes = std::move(it_mimes->second);
            offers_.clear();
            offer_mimes_.clear();
            offers_[offer_proxy] = std::move(saved_offer);
            offer_mimes_[offer_proxy] = std::move(saved_mimes);

            const bool is_image = best_mime == CLIPBOARD_IMAGE_MIME;
            loop_.addFd(pipefs[0], [this, best_mime, is_image](int fd) {
                char buf[65536];
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0) {
                    pending_reads_[fd].append(buf, n);
                } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
                    // Done reading or error
                    std::string result = std::move(pending_reads_[fd]);
                    pending_reads_.erase(fd);
                    loop_.removeFd(fd);
                    close(fd);
                    if (n < 0 && is_image)
                        return; // a truncated PNG is useless

                    // trim trailing nulls and whitespace; image bytes stay intact
                    while (!is_image && !result.empty() &&
                           (result.back() == '\0' || isspace(static_cast<unsigned char>(result.back())))) {
                        result.pop_back();
                    }

                    if (cb_) {
                        cb_(result, best_mime);
                    }
                }
            });
        }

        Manager* manager_;
        EpollEventLoop& loop_;
        wl_display* display_;
        std::unique_ptr<Device> device_;
        std::unique_ptr<Source> source_;

        std::function<void(const std::string&, const std::string&)> cb_;
        std::map<wl_proxy*, std::unique_ptr<Offer>> offers_;
        std::map<wl_proxy*, std::vector<std::string>> offer_mimes_;
        std::map<int, std::string> pending_reads_;
    };

} // namespace tether
