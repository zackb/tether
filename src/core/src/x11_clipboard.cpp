#include "tether/x11_clipboard.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <poll.h>
#include <sys/stat.h>
#include <tether/log.hpp>

namespace tether {

    namespace {
        // Same ceiling the image transfer to the phone uses.
        constexpr size_t MAX_SELECTION_BYTES = 32u * 1024 * 1024;
        constexpr time_t READ_TIMEOUT_SECONDS = 10;
        constexpr time_t SEND_TIMEOUT_SECONDS = 30;

        // A requestor window that disappears mid-transfer raises BadWindow, and
        // Xlib's default handler exits the process. Log and carry on instead.
        int on_x_error(Display* dpy, XErrorEvent* err) {
            char text[128] = {};
            XGetErrorText(dpy, err->error_code, text, sizeof(text) - 1);
            debug::log(WARN, "X11Clipboard: X error ignored: {} (request {})", text, err->request_code);
            return 0;
        }

        std::string latin1_to_utf8(const std::string& in) {
            std::string out;
            out.reserve(in.size());
            for (unsigned char c : in) {
                if (c < 0x80) {
                    out.push_back(static_cast<char>(c));
                } else {
                    out.push_back(static_cast<char>(0xC0 | (c >> 6)));
                    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
                }
            }
            return out;
        }

        // STRING is Latin-1 by definition; characters outside it become '?'.
        std::string utf8_to_latin1(const std::string& in) {
            std::string out;
            out.reserve(in.size());
            for (size_t i = 0; i < in.size();) {
                const unsigned char c = static_cast<unsigned char>(in[i]);
                if (c < 0x80) {
                    out.push_back(static_cast<char>(c));
                    ++i;
                } else if ((c & 0xE0) == 0xC0 && i + 1 < in.size()) {
                    const unsigned cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(in[i + 1]) & 0x3F);
                    out.push_back(cp < 0x100 ? static_cast<char>(cp) : '?');
                    i += 2;
                } else {
                    out.push_back('?');
                    ++i;
                    while (i < in.size() && (static_cast<unsigned char>(in[i]) & 0xC0) == 0x80)
                        ++i;
                }
            }
            return out;
        }
    } // namespace

    struct X11Clipboard::Impl {
        explicit Impl(EpollEventLoop& l) : loop(l) {}

        EpollEventLoop& loop;
        Display* dpy = nullptr;
        Window win = 0;
        int fd = -1;
        int xfixes_event_base = 0;
        size_t chunk = 4096;
        std::string name;
        bool lost = false;

        std::function<void(const std::string&, const std::string&)> cb;
        std::function<void()> on_disconnect;

        Atom CLIPBOARD = None, TARGETS = None, TIMESTAMP = None, INCR = None, UTF8_STRING = None, TEXT = None,
             PROP = None;

        // What we own after copy()/copy_image().
        bool owning = false;
        std::shared_ptr<const std::string> owned;
        std::vector<Atom> owned_targets;

        // Outgoing INCR transfers, one per requestor window and property.
        struct Outgoing {
            Window requestor;
            Atom property;
            Atom type;
            std::shared_ptr<const std::string> data;
            size_t offset;
            time_t started;
        };
        std::vector<Outgoing> outgoing;

        // The one read in flight: TARGETS first, then the chosen target, maybe via INCR.
        enum class Read { Idle, Targets, Data, Incr };
        Read read = Read::Idle;
        std::string read_mime;
        std::string read_buf;
        time_t read_started = 0;

        // ---- helpers --------------------------------------------------------------------

        std::string atom_name(Atom a) {
            if (a == None)
                return {};
            char* n = XGetAtomName(dpy, a);
            std::string s = n ? n : "";
            if (n)
                XFree(n);
            return s;
        }

        // Reads a whole property in pieces. Format-32 data arrives as an array of long.
        bool get_property(Window w, Atom prop, bool del, Atom& type, int& format, std::string& out) {
            out.clear();
            long offset = 0;
            for (;;) {
                Atom actual = None;
                int fmt = 0;
                unsigned long nitems = 0, after = 0;
                unsigned char* data = nullptr;
                if (XGetWindowProperty(
                        dpy, w, prop, offset, 1 << 20, False, AnyPropertyType, &actual, &fmt, &nitems, &after, &data) !=
                    Success) {
                    return false;
                }
                type = actual;
                format = fmt;
                if (data) {
                    const size_t unit = fmt == 32 ? sizeof(long) : static_cast<size_t>(fmt / 8);
                    out.append(reinterpret_cast<const char*>(data), nitems * unit);
                    XFree(data);
                }
                if (out.size() > MAX_SELECTION_BYTES)
                    return false;
                if (after == 0)
                    break;
                // offset counts 32-bit units of the property, whatever its format
                offset += static_cast<long>(nitems * (fmt / 8) / 4);
            }
            if (del)
                XDeleteProperty(dpy, w, prop);
            return true;
        }

        void send_notify(const XSelectionRequestEvent& req, Atom property) {
            XEvent ev{};
            ev.xselection.type = SelectionNotify;
            ev.xselection.display = req.display;
            ev.xselection.requestor = req.requestor;
            ev.xselection.selection = req.selection;
            ev.xselection.target = req.target;
            ev.xselection.property = property;
            ev.xselection.time = req.time;
            XSendEvent(dpy, req.requestor, False, NoEventMask, &ev);
        }

        // ---- reading other clients' selection --------------------------------------------

        void start_read(Time when) {
            if (read != Read::Idle)
                XDeleteProperty(dpy, win, PROP); // abandon the read in flight
            read = Read::Targets;
            read_buf.clear();
            read_mime.clear();
            read_started = std::time(nullptr);
            XConvertSelection(dpy, CLIPBOARD, TARGETS, PROP, win, when);
        }

        void finish_read(std::string data) {
            read = Read::Idle;
            const bool is_image = read_mime == CLIPBOARD_IMAGE_MIME;
            if (!is_image) {
                if (read_mime == "STRING")
                    data = latin1_to_utf8(data);
                while (!data.empty() && (data.back() == '\0' || isspace(static_cast<unsigned char>(data.back()))))
                    data.pop_back();
            }
            debug::log(INFO, "X11Clipboard: desktop clipboard changed ({}, {} bytes)", read_mime, data.size());
            if (cb)
                cb(data, read_mime);
        }

        void on_selection_notify(const XSelectionEvent& e) {
            if (e.requestor != win || e.selection != CLIPBOARD || read == Read::Idle)
                return;

            if (e.property == None) {
                // Ancient owners answer no TARGETS; ask for plain UTF-8 text directly.
                if (read == Read::Targets) {
                    read = Read::Data;
                    read_mime = "UTF8_STRING";
                    XConvertSelection(dpy, CLIPBOARD, UTF8_STRING, PROP, win, e.time);
                } else {
                    read = Read::Idle;
                }
                return;
            }

            Atom type = None;
            int format = 0;
            std::string data;

            if (read == Read::Targets) {
                if (!get_property(win, PROP, true, type, format, data) || format != 32) {
                    read = Read::Idle;
                    return;
                }
                const auto* atoms = reinterpret_cast<const Atom*>(data.data());
                const size_t count = data.size() / sizeof(long);
                std::vector<std::string> names;
                std::vector<Atom> list(atoms, atoms + count);
                for (Atom a : list)
                    names.push_back(atom_name(a));
                const std::string best = pick_clipboard_mime(names);
                if (best.empty()) {
                    read = Read::Idle; // nothing we can carry, e.g. a file list
                    return;
                }
                read_mime = best;
                read = Read::Data;
                XConvertSelection(dpy, CLIPBOARD, XInternAtom(dpy, best.c_str(), False), PROP, win, e.time);
                return;
            }

            if (read == Read::Data) {
                if (!get_property(win, PROP, false, type, format, data)) {
                    XDeleteProperty(dpy, win, PROP);
                    read = Read::Idle;
                    return;
                }
                if (type == INCR) {
                    // Deleting the INCR marker asks the owner for the first chunk.
                    read = Read::Incr;
                    read_buf.clear();
                    XDeleteProperty(dpy, win, PROP);
                    return;
                }
                XDeleteProperty(dpy, win, PROP);
                finish_read(std::move(data));
            }
        }

        void on_incr_chunk() {
            Atom type = None;
            int format = 0;
            std::string data;
            if (!get_property(win, PROP, true, type, format, data)) {
                read = Read::Idle;
                return;
            }
            if (data.empty()) {
                finish_read(std::move(read_buf));
                return;
            }
            read_buf.append(data);
            if (read_buf.size() > MAX_SELECTION_BYTES) {
                debug::log(WARN, "X11Clipboard: selection over {} bytes, ignored", MAX_SELECTION_BYTES);
                read = Read::Idle;
                read_buf.clear();
            }
        }

        // ---- serving our own selection ---------------------------------------------------

        void own(const std::string& data, const std::vector<std::string>& mimes) {
            owned = std::make_shared<const std::string>(data);
            owned_targets.clear();
            for (const auto& m : mimes)
                owned_targets.push_back(XInternAtom(dpy, m.c_str(), False));
            read = Read::Idle; // whatever we were reading is no longer the selection
            XSetSelectionOwner(dpy, CLIPBOARD, win, CurrentTime);
            owning = XGetSelectionOwner(dpy, CLIPBOARD) == win;
            if (!owning)
                debug::log(ERR, "X11Clipboard: could not take the CLIPBOARD selection");
            else
                debug::log(INFO, "X11Clipboard: now serving {} bytes from the phone", data.size());
            XFlush(dpy);
        }

        void on_selection_request(const XSelectionRequestEvent& req) {
            const Atom property = req.property == None ? req.target : req.property; // obsolete clients
            if (req.selection != CLIPBOARD || !owning || !owned) {
                send_notify(req, None);
                return;
            }

            if (req.target == TARGETS) {
                std::vector<Atom> list = {TARGETS, TIMESTAMP};
                list.insert(list.end(), owned_targets.begin(), owned_targets.end());
                XChangeProperty(dpy,
                                req.requestor,
                                property,
                                XA_ATOM,
                                32,
                                PropModeReplace,
                                reinterpret_cast<const unsigned char*>(list.data()),
                                static_cast<int>(list.size()));
                send_notify(req, property);
                return;
            }
            if (req.target == TIMESTAMP) {
                long now = static_cast<long>(CurrentTime);
                XChangeProperty(dpy,
                                req.requestor,
                                property,
                                XA_INTEGER,
                                32,
                                PropModeReplace,
                                reinterpret_cast<const unsigned char*>(&now),
                                1);
                send_notify(req, property);
                return;
            }
            if (std::find(owned_targets.begin(), owned_targets.end(), req.target) == owned_targets.end()) {
                send_notify(req, None); // includes MULTIPLE, which nothing needs from a text/image owner
                return;
            }

            std::shared_ptr<const std::string> payload = owned;
            Atom type = req.target;
            if (req.target == XA_STRING) {
                payload = std::make_shared<const std::string>(utf8_to_latin1(*owned));
            } else if (req.target == TEXT) {
                type = UTF8_STRING; // TEXT means "any text encoding": say which one
            }

            if (payload->size() <= chunk) {
                XChangeProperty(dpy,
                                req.requestor,
                                property,
                                type,
                                8,
                                PropModeReplace,
                                reinterpret_cast<const unsigned char*>(payload->data()),
                                static_cast<int>(payload->size()));
                send_notify(req, property);
                return;
            }

            // INCR: announce the size, then send a chunk each time the requestor deletes the property.
            XSelectInput(dpy, req.requestor, PropertyChangeMask);
            long size = static_cast<long>(payload->size());
            XChangeProperty(dpy,
                            req.requestor,
                            property,
                            INCR,
                            32,
                            PropModeReplace,
                            reinterpret_cast<const unsigned char*>(&size),
                            1);
            outgoing.push_back({req.requestor, property, type, payload, 0, std::time(nullptr)});
            send_notify(req, property);
        }

        void on_requestor_deleted(Window w, Atom prop) {
            auto it = std::find_if(outgoing.begin(), outgoing.end(), [&](const Outgoing& o) {
                return o.requestor == w && o.property == prop;
            });
            if (it == outgoing.end())
                return;
            const size_t left = it->data->size() - it->offset;
            const size_t len = std::min(left, chunk);
            XChangeProperty(dpy,
                            w,
                            prop,
                            it->type,
                            8,
                            PropModeReplace,
                            reinterpret_cast<const unsigned char*>(it->data->data() + it->offset),
                            static_cast<int>(len));
            if (len == 0) {
                outgoing.erase(it); // the zero-length chunk just sent ends the transfer
                stop_watching(w);
            } else {
                it->offset += len;
            }
        }

        void stop_watching(Window w) {
            const bool still_used =
                std::any_of(outgoing.begin(), outgoing.end(), [&](const Outgoing& o) { return o.requestor == w; });
            if (!still_used && w != win)
                XSelectInput(dpy, w, NoEventMask);
        }

        void expire_stale() {
            const time_t now = std::time(nullptr);
            if (read != Read::Idle && now - read_started > READ_TIMEOUT_SECONDS) {
                debug::log(WARN, "X11Clipboard: clipboard owner stopped answering; read abandoned");
                read = Read::Idle;
                read_buf.clear();
            }
            for (auto it = outgoing.begin(); it != outgoing.end();) {
                if (now - it->started > SEND_TIMEOUT_SECONDS) {
                    const Window w = it->requestor;
                    it = outgoing.erase(it);
                    stop_watching(w);
                } else {
                    ++it;
                }
            }
        }

        // ---- event dispatch ---------------------------------------------------------------

        void handle(XEvent& ev) {
            if (ev.type == xfixes_event_base + XFixesSelectionNotify) {
                const auto& sn = reinterpret_cast<const XFixesSelectionNotifyEvent&>(ev);
                if (sn.selection != CLIPBOARD || sn.owner == win)
                    return; // our own copy() coming back: not news
                if (sn.owner == None) {
                    read = Read::Idle;
                    if (cb)
                        cb("", "");
                    return;
                }
                start_read(sn.selection_timestamp);
                return;
            }

            switch (ev.type) {
            case SelectionNotify:
                on_selection_notify(ev.xselection);
                break;
            case SelectionRequest:
                on_selection_request(ev.xselectionrequest);
                break;
            case SelectionClear:
                if (ev.xselectionclear.selection == CLIPBOARD && ev.xselectionclear.window == win) {
                    owning = false;
                    owned.reset();
                    owned_targets.clear();
                }
                break;
            case PropertyNotify:
                if (ev.xproperty.window == win && ev.xproperty.atom == PROP && ev.xproperty.state == PropertyNewValue &&
                    read == Read::Incr) {
                    on_incr_chunk();
                } else if (ev.xproperty.state == PropertyDelete) {
                    on_requestor_deleted(ev.xproperty.window, ev.xproperty.atom);
                }
                break;
            default:
                break;
            }
        }

        void dispatch() {
            if (lost)
                return;
            // XPending reads the socket and also drains events Xlib queued during earlier calls,
            // which the fd alone would never wake us for.
            while (!lost && XPending(dpy) > 0) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                handle(ev);
            }
            if (lost)
                return;
            expire_stale();
            XFlush(dpy);
        }

        void connection_lost() {
            if (lost)
                return;
            lost = true;
            debug::log(ERR, "X11Clipboard: lost the connection to X display {}", name);
            // Never tear down from inside an Xlib callback or our own fd callback.
            loop.post([cb = on_disconnect] {
                if (cb)
                    cb();
            });
        }
    };

    std::vector<std::string> x11_displays(const std::string& socket_dir, unsigned uid) {
        std::vector<std::pair<long, std::string>> found;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(socket_dir, ec)) {
            const std::string name = entry.path().filename().string();
            if (name.size() < 2 || name[0] != 'X' ||
                !std::all_of(name.begin() + 1, name.end(), [](unsigned char c) { return std::isdigit(c); }))
                continue;
            struct stat st{};
            if (lstat(entry.path().c_str(), &st) != 0 || !S_ISSOCK(st.st_mode) || st.st_uid != uid)
                continue;
            found.emplace_back(std::stol(name.substr(1)), ":" + name.substr(1));
        }
        std::sort(found.begin(), found.end());
        std::vector<std::string> names;
        for (auto& f : found)
            names.push_back(std::move(f.second));
        return names;
    }

    std::unique_ptr<X11Clipboard> X11Clipboard::create(EpollEventLoop& loop, const char* display_name) {
        const char* target = display_name ? display_name : std::getenv("DISPLAY");
        if (!x11_display_plausible(target))
            return nullptr;

        Display* dpy = XOpenDisplay(target);
        if (!dpy)
            return nullptr;

        int event_base = 0, error_base = 0;
        if (!XFixesQueryExtension(dpy, &event_base, &error_base)) {
            debug::log(ERR, "X11Clipboard: X server {} has no XFixes; clipboard sync unavailable.", target);
            XCloseDisplay(dpy);
            return nullptr;
        }

        auto impl = std::make_unique<Impl>(loop);
        impl->dpy = dpy;
        impl->fd = ConnectionNumber(dpy);
        impl->xfixes_event_base = event_base;
        impl->name = DisplayString(dpy);
        const long max_request = XExtendedMaxRequestSize(dpy) ? XExtendedMaxRequestSize(dpy) : XMaxRequestSize(dpy);
        impl->chunk = x11_chunk_size(max_request);

        XSetErrorHandler(on_x_error);

        impl->CLIPBOARD = XInternAtom(dpy, "CLIPBOARD", False);
        impl->TARGETS = XInternAtom(dpy, "TARGETS", False);
        impl->TIMESTAMP = XInternAtom(dpy, "TIMESTAMP", False);
        impl->INCR = XInternAtom(dpy, "INCR", False);
        impl->UTF8_STRING = XInternAtom(dpy, "UTF8_STRING", False);
        impl->TEXT = XInternAtom(dpy, "TEXT", False);
        impl->PROP = XInternAtom(dpy, "TETHER_SELECTION", False);

        // An unmapped window: selection owner, conversion target and INCR receiver.
        impl->win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), -10, -10, 1, 1, 0, 0, 0);
        XStoreName(dpy, impl->win, "tether clipboard");
        XSelectInput(dpy, impl->win, PropertyChangeMask);
        XFixesSelectSelectionInput(dpy,
                                   impl->win,
                                   impl->CLIPBOARD,
                                   XFixesSetSelectionOwnerNotifyMask | XFixesSelectionWindowDestroyNotifyMask |
                                       XFixesSelectionClientCloseNotifyMask);

        std::unique_ptr<X11Clipboard> self(new X11Clipboard(std::move(impl)));
        Impl* raw = self->impl_.get();

        // Xlib calls this when the server goes away; returning (instead of exiting) needs libX11 >= 1.7.
        XSetIOErrorExitHandler(dpy, [](Display*, void* data) { static_cast<Impl*>(data)->connection_lost(); }, raw);

        loop.addFd(raw->fd, [raw](int fd) {
            struct pollfd pfd{fd, 0, 0};
            if (poll(&pfd, 1, 0) == 1 && (pfd.revents & (POLLHUP | POLLERR))) {
                raw->connection_lost();
                return;
            }
            raw->dispatch();
        });

        // Pick up whatever is on the clipboard already, as the Wayland backend does.
        if (Window owner = XGetSelectionOwner(dpy, raw->CLIPBOARD); owner != None && owner != raw->win)
            raw->start_read(CurrentTime);
        XFlush(dpy);

        debug::log(INFO, "X11Clipboard: syncing the CLIPBOARD selection on X display {}.", raw->name);
        return self;
    }

    X11Clipboard::X11Clipboard(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    X11Clipboard::~X11Clipboard() {
        if (!impl_)
            return;
        impl_->loop.removeFd(impl_->fd);
        if (impl_->lost) {
            // The server is gone; XCloseDisplay would only try to talk to it. Free the client side.
            impl_->dpy = nullptr;
            return;
        }
        for (const auto& o : impl_->outgoing)
            XSelectInput(impl_->dpy, o.requestor, NoEventMask);
        XDestroyWindow(impl_->dpy, impl_->win);
        XCloseDisplay(impl_->dpy);
    }

    void X11Clipboard::set_update_callback(std::function<void(const std::string&, const std::string&)> cb) {
        impl_->cb = std::move(cb);
    }

    void X11Clipboard::set_disconnect_callback(std::function<void()> cb) { impl_->on_disconnect = std::move(cb); }

    // Offer the same text types the Wayland backend does, plus the X11-specific ones.
    void X11Clipboard::copy(const std::string& text) {
        if (impl_->lost)
            return;
        impl_->own(text, {"UTF8_STRING", "text/plain;charset=utf-8", "text/plain", "TEXT", "STRING"});
    }

    void X11Clipboard::copy_image(const std::string& png) {
        if (impl_->lost)
            return;
        impl_->own(png, {CLIPBOARD_IMAGE_MIME});
    }

    const std::string& X11Clipboard::display_name() const { return impl_->name; }

} // namespace tether
