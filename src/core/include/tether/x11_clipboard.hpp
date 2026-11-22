#pragma once

#include "tether/clipboard.hpp"
#include "tether/event_loop.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tether {

    // X11 clipboard backend for sessions without a Wayland data-control protocol
    // (GNOME and other X11 desktops). Implements the ICCCM CLIPBOARD selection:
    // XFixes reports ownership changes, so nothing polls; reads and writes go
    // through selection conversions, with INCR for payloads larger than one request.
    class X11Clipboard final : public ClipboardManager {
    public:
        // Opens its own connection to `display_name` (nullptr means $DISPLAY).
        // Returns nullptr when there is no X server or it lacks XFixes.
        static std::unique_ptr<X11Clipboard> create(EpollEventLoop& loop, const char* display_name = nullptr);

        ~X11Clipboard() override;
        X11Clipboard(const X11Clipboard&) = delete;
        X11Clipboard& operator=(const X11Clipboard&) = delete;

        void set_update_callback(std::function<void(const std::string&, const std::string&)> cb) override;
        void copy(const std::string& text) override;
        void copy_image(const std::string& png) override;

        // Called on the event loop after the X server went away. The object is
        // unusable from then on and should be destroyed.
        void set_disconnect_callback(std::function<void()> cb);

        // Name of the display the connection is on, e.g. ":0".
        const std::string& display_name() const;

    private:
        struct Impl;
        explicit X11Clipboard(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

    // Largest property chunk to write in one request, from the server's maximum
    // request length in 4-byte units. Leaves room for the request header.
    inline size_t x11_chunk_size(long max_request_units) {
        constexpr size_t floor_bytes = 4096;
        constexpr size_t ceiling_bytes = 256 * 1024;
        if (max_request_units <= 0)
            return floor_bytes;
        const size_t bytes = static_cast<size_t>(max_request_units) * 4;
        const size_t usable = bytes > 1024 ? bytes - 1024 : floor_bytes;
        if (usable < floor_bytes)
            return floor_bytes;
        return usable > ceiling_bytes ? ceiling_bytes : usable;
    }

    // Splits `size` bytes into the [offset, length) chunks an INCR transfer sends,
    // ending with the zero-length chunk that tells the requestor it is complete.
    inline std::vector<std::pair<size_t, size_t>> x11_incr_chunks(size_t size, size_t chunk) {
        std::vector<std::pair<size_t, size_t>> out;
        if (chunk == 0)
            return out;
        for (size_t off = 0; off < size; off += chunk)
            out.emplace_back(off, size - off < chunk ? size - off : chunk);
        out.emplace_back(size, 0);
        return out;
    }

    // Display names (":0", ":1", ...) for the X server sockets in `socket_dir` owned by `uid`,
    // sorted. A daemon started before the session has no $DISPLAY; this finds the user's own
    // X server without trying anyone else's.
    std::vector<std::string> x11_displays(const std::string& socket_dir, unsigned uid);

    // True when `display` names something XOpenDisplay could reach: ":0", "host:1.0", etc.
    inline bool x11_display_plausible(const char* display) {
        if (!display || !*display)
            return false;
        const std::string d = display;
        return d.find(':') != std::string::npos;
    }

} // namespace tether
