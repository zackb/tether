#include <gtest/gtest.h>
#include <tether/event_loop.hpp>
#include <tether/x11_clipboard.hpp>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace tether;
using namespace std::chrono_literals;

// ---- pure helpers -------------------------------------------------------------------------

TEST(X11ClipboardHelpers, ChunkSizeStaysWithinBounds) {
    EXPECT_EQ(x11_chunk_size(0), 4096u);
    EXPECT_EQ(x11_chunk_size(-1), 4096u);
    EXPECT_EQ(x11_chunk_size(100), 4096u);                   // tiny server limit: floor
    EXPECT_EQ(x11_chunk_size(65535), 65535u * 4 - 1024);     // classic 256 KB request limit
    EXPECT_EQ(x11_chunk_size(4 * 1024 * 1024), 256u * 1024); // BIG-REQUESTS: capped
}

TEST(X11ClipboardHelpers, IncrChunksCoverEverythingAndEndEmpty) {
    const auto chunks = x11_incr_chunks(10, 4);
    ASSERT_EQ(chunks.size(), 4u);
    EXPECT_EQ(chunks[0], (std::pair<size_t, size_t>{0, 4}));
    EXPECT_EQ(chunks[1], (std::pair<size_t, size_t>{4, 4}));
    EXPECT_EQ(chunks[2], (std::pair<size_t, size_t>{8, 2}));
    EXPECT_EQ(chunks[3], (std::pair<size_t, size_t>{10, 0})); // terminator

    const auto exact = x11_incr_chunks(8, 4);
    ASSERT_EQ(exact.size(), 3u);
    EXPECT_EQ(exact.back(), (std::pair<size_t, size_t>{8, 0}));

    EXPECT_TRUE(x11_incr_chunks(10, 0).empty());
}

TEST(X11ClipboardHelpers, DisplayPlausible) {
    EXPECT_TRUE(x11_display_plausible(":0"));
    EXPECT_TRUE(x11_display_plausible("localhost:10.0"));
    EXPECT_FALSE(x11_display_plausible(nullptr));
    EXPECT_FALSE(x11_display_plausible(""));
    EXPECT_FALSE(x11_display_plausible("wayland-0"));
}

namespace {
    void make_socket(const std::filesystem::path& p) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT_GE(fd, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, p.c_str(), sizeof(addr.sun_path) - 1);
        ASSERT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        close(fd);
    }
} // namespace

TEST(X11ClipboardHelpers, FindsOnlyOwnNumberedSockets) {
    auto dir = std::filesystem::temp_directory_path() / ("tether-x11-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    make_socket(dir / "X10");
    make_socket(dir / "X2");
    make_socket(dir / "Xfoo");       // not a display number
    std::ofstream(dir / "X7") << ""; // not a socket

    EXPECT_EQ(x11_displays(dir.string(), getuid()), (std::vector<std::string>{":2", ":10"})); // numeric order
    EXPECT_TRUE(x11_displays(dir.string(), getuid() + 1).empty()); // another user's server is never tried
    EXPECT_TRUE(x11_displays((dir / "missing").string(), getuid()).empty());

    std::filesystem::remove_all(dir);
}

// ---- against a real X server ---------------------------------------------------------------
// Set TETHER_TEST_X11_DISPLAY to a throwaway server (Xvfb/Xephyr), never the desktop in use:
// these tests take over its clipboard.

namespace {
    const char* test_display() {
        const char* d = std::getenv("TETHER_TEST_X11_DISPLAY");
        return d && *d ? d : nullptr;
    }

    // Wait for one event of `type` on `dpy`, up to `timeout`.
    bool wait_event(Display* dpy, int type, XEvent& out, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            while (XPending(dpy)) {
                XNextEvent(dpy, &out);
                if (out.type == type)
                    return true;
            }
            pollfd p{ConnectionNumber(dpy), POLLIN, 0};
            poll(&p, 1, 20);
        }
        return false;
    }

    // Another X client pasting: convert CLIPBOARD to `target` and read it, following INCR.
    std::string paste(const char* display, const char* target_name) {
        Display* dpy = XOpenDisplay(display);
        if (!dpy)
            return "<no display>";
        Window w = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);
        XSelectInput(dpy, w, PropertyChangeMask);
        const Atom clip = XInternAtom(dpy, "CLIPBOARD", False);
        const Atom target = XInternAtom(dpy, target_name, False);
        const Atom prop = XInternAtom(dpy, "PASTE", False);
        const Atom incr = XInternAtom(dpy, "INCR", False);
        XConvertSelection(dpy, clip, target, prop, w, CurrentTime);
        XFlush(dpy);

        std::string result = "<timeout>";
        XEvent ev;
        if (wait_event(dpy, SelectionNotify, ev, 3s) && ev.xselection.property != None) {
            Atom type;
            int format;
            unsigned long n, after;
            unsigned char* data = nullptr;
            XGetWindowProperty(dpy, w, prop, 0, 1 << 24, True, AnyPropertyType, &type, &format, &n, &after, &data);
            if (type == incr) {
                if (data)
                    XFree(data);
                result.clear();
                for (;;) {
                    // deleting the property (done above via delete=True) asks for the next chunk
                    if (!wait_event(dpy, PropertyNotify, ev, 3s)) {
                        result = "<incr timeout>";
                        break;
                    }
                    if (ev.xproperty.state != PropertyNewValue)
                        continue;
                    XGetWindowProperty(
                        dpy, w, prop, 0, 1 << 24, True, AnyPropertyType, &type, &format, &n, &after, &data);
                    const size_t len = n;
                    if (data) {
                        result.append(reinterpret_cast<char*>(data), len);
                        XFree(data);
                    }
                    if (len == 0)
                        break;
                }
            } else {
                result.assign(reinterpret_cast<char*>(data), n);
                if (data)
                    XFree(data);
            }
        } else if (ev.type == SelectionNotify) {
            result = "<refused>";
        }
        XDestroyWindow(dpy, w);
        XCloseDisplay(dpy);
        return result;
    }

    // Another X client copying: own CLIPBOARD and serve UTF8_STRING until destroyed.
    class Copier {
    public:
        Copier(const char* display, std::string text) : text_(std::move(text)) {
            dpy_ = XOpenDisplay(display);
            win_ = XCreateSimpleWindow(dpy_, DefaultRootWindow(dpy_), 0, 0, 1, 1, 0, 0, 0);
            clip_ = XInternAtom(dpy_, "CLIPBOARD", False);
            utf8_ = XInternAtom(dpy_, "UTF8_STRING", False);
            targets_ = XInternAtom(dpy_, "TARGETS", False);
            XSetSelectionOwner(dpy_, clip_, win_, CurrentTime);
            XFlush(dpy_);
            thread_ = std::thread([this] { serve(); });
        }
        ~Copier() {
            stop_ = true;
            thread_.join();
            XDestroyWindow(dpy_, win_);
            XCloseDisplay(dpy_);
        }

    private:
        void serve() {
            while (!stop_) {
                XEvent ev;
                if (!wait_event(dpy_, SelectionRequest, ev, 50ms))
                    continue;
                auto& r = ev.xselectionrequest;
                XEvent reply{};
                reply.xselection.type = SelectionNotify;
                reply.xselection.requestor = r.requestor;
                reply.xselection.selection = r.selection;
                reply.xselection.target = r.target;
                reply.xselection.time = r.time;
                reply.xselection.property = r.property;
                if (r.target == targets_) {
                    Atom list[] = {targets_, utf8_};
                    XChangeProperty(dpy_,
                                    r.requestor,
                                    r.property,
                                    XA_ATOM,
                                    32,
                                    PropModeReplace,
                                    reinterpret_cast<unsigned char*>(list),
                                    2);
                } else if (r.target == utf8_) {
                    XChangeProperty(dpy_,
                                    r.requestor,
                                    r.property,
                                    utf8_,
                                    8,
                                    PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(text_.data()),
                                    static_cast<int>(text_.size()));
                } else {
                    reply.xselection.property = None;
                }
                XSendEvent(dpy_, r.requestor, False, NoEventMask, &reply);
                XFlush(dpy_);
            }
        }

        std::string text_;
        Display* dpy_;
        Window win_;
        Atom clip_, utf8_, targets_;
        std::atomic<bool> stop_{false};
        std::thread thread_;
    };

    // Runs the event loop on its own thread for the length of a test.
    struct LoopThread {
        EpollEventLoop loop;
        std::thread thread;
        void start() {
            thread = std::thread([this] { loop.run(); });
        }
        // stop() only clears a flag; it has to run on the loop to wake epoll_wait.
        void finish() {
            if (!thread.joinable())
                return;
            loop.post([this] { loop.stop(); });
            thread.join();
        }
        ~LoopThread() { finish(); }
        void run_sync(std::function<void()> fn) {
            std::mutex m;
            std::condition_variable cv;
            bool done = false;
            loop.post([&] {
                fn();
                std::lock_guard<std::mutex> lock(m);
                done = true;
                cv.notify_one();
            });
            std::unique_lock<std::mutex> lock(m);
            cv.wait_for(lock, 3s, [&] { return done; });
        }
    };
} // namespace

#define REQUIRE_TEST_DISPLAY()                                                                                         \
    const char* display = test_display();                                                                              \
    if (!display)                                                                                                      \
    GTEST_SKIP() << "set TETHER_TEST_X11_DISPLAY to a throwaway X server (Xvfb/Xephyr) to run"

TEST(X11ClipboardLive, CreateFailsWithoutServer) {
    EpollEventLoop loop;
    EXPECT_EQ(X11Clipboard::create(loop, ":99999"), nullptr);
    EXPECT_EQ(X11Clipboard::create(loop, "not-a-display"), nullptr);
}

TEST(X11ClipboardLive, PhoneTextPastesInOtherApps) {
    REQUIRE_TEST_DISPLAY();
    LoopThread lt;
    auto clip = X11Clipboard::create(lt.loop, display);
    ASSERT_NE(clip, nullptr);
    lt.start();

    const std::string text = "from the phone: héllo ✓ 👋";
    lt.run_sync([&] { clip->copy(text); });

    EXPECT_EQ(paste(display, "UTF8_STRING"), text);
    EXPECT_EQ(paste(display, "text/plain;charset=utf-8"), text);
    EXPECT_EQ(paste(display, "STRING"), "from the phone: h\xe9llo ? ?"); // Latin-1, '?' outside it
    EXPECT_EQ(paste(display, "image/png"), "<refused>");

    lt.finish();
}

TEST(X11ClipboardLive, LargeTextUsesIncr) {
    REQUIRE_TEST_DISPLAY();
    LoopThread lt;
    auto clip = X11Clipboard::create(lt.loop, display);
    ASSERT_NE(clip, nullptr);
    lt.start();

    std::string big(3 * 1024 * 1024, 'x'); // well over one request
    for (size_t i = 0; i < big.size(); i += 997)
        big[i] = static_cast<char>('a' + (i % 26));
    lt.run_sync([&] { clip->copy(big); });

    const std::string got = paste(display, "UTF8_STRING");
    EXPECT_EQ(got.size(), big.size());
    EXPECT_TRUE(got == big);

    lt.finish();
}

TEST(X11ClipboardLive, PhoneImagePastesAsPng) {
    REQUIRE_TEST_DISPLAY();
    LoopThread lt;
    auto clip = X11Clipboard::create(lt.loop, display);
    ASSERT_NE(clip, nullptr);
    lt.start();

    const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(1000, 'p');
    lt.run_sync([&] { clip->copy_image(png); });
    EXPECT_EQ(paste(display, "image/png"), png);
    EXPECT_EQ(paste(display, "UTF8_STRING"), "<refused>");

    lt.finish();
}

TEST(X11ClipboardLive, DesktopCopyIsReported) {
    REQUIRE_TEST_DISPLAY();
    LoopThread lt;
    auto clip = X11Clipboard::create(lt.loop, display);
    ASSERT_NE(clip, nullptr);

    std::mutex m;
    std::condition_variable cv;
    std::vector<std::pair<std::string, std::string>> seen;
    clip->set_update_callback([&](const std::string& data, const std::string& mime) {
        std::lock_guard<std::mutex> lock(m);
        seen.emplace_back(data, mime);
        cv.notify_one();
    });
    lt.start();

    {
        Copier other(display, "copied on the desktop\n");
        std::unique_lock<std::mutex> lock(m);
        ASSERT_TRUE(cv.wait_for(lock, 3s, [&] {
            return std::any_of(
                seen.begin(), seen.end(), [](const auto& s) { return s.first == "copied on the desktop"; });
        }));
    }

    // Our own copy() must not be reported back as a desktop change (no echo to the phone).
    {
        std::lock_guard<std::mutex> lock(m);
        seen.clear();
    }
    lt.run_sync([&] { clip->copy("from the phone"); });
    std::this_thread::sleep_for(300ms);
    {
        std::lock_guard<std::mutex> lock(m);
        EXPECT_TRUE(seen.empty());
    }

    lt.finish();
}
