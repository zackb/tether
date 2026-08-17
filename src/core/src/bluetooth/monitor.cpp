#include "tether/bluetooth/monitor.hpp"
#include "tether/log.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <mutex>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

namespace tether::bluetooth {

    BluezMonitor* g_bluez = nullptr;

    namespace {
        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* OBJECT_MANAGER = "org.freedesktop.DBus.ObjectManager";

        // BlueZ emits PropertiesChanged continuously during discovery (RSSI churn
        // above all). Coalesce bursts into one refresh instead of re-reading the
        // whole object tree per signal.
        constexpr guint REFRESH_DEBOUNCE_MS = 250;
    } // namespace

    struct MonitorState {
        std::thread thread;
        GMainContext* context = nullptr;
        GMainLoop* loop = nullptr;
        GDBusConnection* conn = nullptr;
        std::atomic<bool> running{false};

        int efd = -1;
        guint pending_refresh = 0;
        std::vector<guint> subscriptions;

        mutable std::mutex mutex;
        BluezObjects objects;
        Capability cap;

        void run();
        void refresh();
        void schedule_refresh();
        void notify();
    };

    namespace {

        gboolean on_refresh_timeout(gpointer user_data) {
            auto* impl = static_cast<MonitorState*>(user_data);
            impl->pending_refresh = 0;
            impl->refresh();
            return G_SOURCE_REMOVE;
        }

        void on_bluez_signal(
            GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer user_data) {
            static_cast<MonitorState*>(user_data)->schedule_refresh();
        }

        // on watcher thread once it iterates its context
        gboolean quit_loop(gpointer user_data) {
            auto* state = static_cast<MonitorState*>(user_data);
            if (state->loop)
                g_main_loop_quit(state->loop);
            return G_SOURCE_REMOVE;
        }

    } // namespace

    void MonitorState::notify() {
        if (efd < 0)
            return;
        uint64_t one = 1;
        if (write(efd, &one, sizeof(one)) < 0)
            debug::log(WARN, "bluetooth: failed to signal event fd");
    }

    void MonitorState::schedule_refresh() {
        if (pending_refresh != 0)
            return;
        GSource* source = g_timeout_source_new(REFRESH_DEBOUNCE_MS);
        g_source_set_callback(source, on_refresh_timeout, this, nullptr);
        pending_refresh = g_source_attach(source, context);
        g_source_unref(source);
    }

    // bluetoothd exposes no property for its own flags, so the argument vector is
    // the only place to read it. Accepts both spellings; Arch's unit uses -E.
    bool bluetoothd_has_experimental() {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
            std::ifstream comm(entry.path() / "comm");
            std::string name;
            if (!comm || !std::getline(comm, name) || name != "bluetoothd")
                continue;

            std::ifstream cmdline(entry.path() / "cmdline", std::ios::binary);
            if (!cmdline)
                continue;
            // Arguments are NUL-separated, so each one compares exactly.
            std::string arg;
            while (std::getline(cmdline, arg, '\0')) {
                if (arg == "-E" || arg == "--experimental")
                    return true;
            }
            return false;
        }
        return false;
    }

    void MonitorState::refresh() {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(conn,
                                                      BLUEZ_NAME,
                                                      "/",
                                                      OBJECT_MANAGER,
                                                      "GetManagedObjects",
                                                      nullptr,
                                                      G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      5000,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            debug::log(WARN, "bluetooth: GetManagedObjects failed: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return;
        }

        BluezObjects parsed = parse_managed_objects(reply);
        g_variant_unref(reply);
        parsed.experimental_api = bluetoothd_has_experimental();
        Capability resolved = resolve_capability(parsed);

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (parsed == objects && resolved == cap)
                return;
            objects = std::move(parsed);
            cap = std::move(resolved);
        }
        notify();
    }

    void MonitorState::run() {
        g_main_context_push_thread_default(context);

        for (const char* signal : {"InterfacesAdded", "InterfacesRemoved"}) {
            subscriptions.push_back(g_dbus_connection_signal_subscribe(conn,
                                                                       BLUEZ_NAME,
                                                                       OBJECT_MANAGER,
                                                                       signal,
                                                                       "/",
                                                                       nullptr,
                                                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                                                       on_bluez_signal,
                                                                       this,
                                                                       nullptr));
        }
        subscriptions.push_back(g_dbus_connection_signal_subscribe(conn,
                                                                   BLUEZ_NAME,
                                                                   "org.freedesktop.DBus.Properties",
                                                                   "PropertiesChanged",
                                                                   nullptr,
                                                                   nullptr,
                                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                                   on_bluez_signal,
                                                                   this,
                                                                   nullptr));

        refresh();

        loop = g_main_loop_new(context, FALSE);
        g_main_loop_run(loop);
        running = false;

        for (guint id : subscriptions)
            g_dbus_connection_signal_unsubscribe(conn, id);
        subscriptions.clear();

        if (pending_refresh != 0) {
            if (GSource* s = g_main_context_find_source_by_id(context, pending_refresh))
                g_source_destroy(s);
            pending_refresh = 0;
        }

        g_main_context_pop_thread_default(context);
    }

    BluezMonitor::BluezMonitor() : impl_(std::make_unique<MonitorState>()) {}

    BluezMonitor::~BluezMonitor() { stop(); }

    bool BluezMonitor::start() {
        if (impl_->running)
            return true;

        GError* error = nullptr;
        impl_->conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
        if (!impl_->conn) {
            debug::log(WARN, "bluetooth: cannot reach the system bus: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return false;
        }

        // fail fast when bluetoothd is not present
        GVariant* probe = g_dbus_connection_call_sync(impl_->conn,
                                                      BLUEZ_NAME,
                                                      "/",
                                                      OBJECT_MANAGER,
                                                      "GetManagedObjects",
                                                      nullptr,
                                                      G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      5000,
                                                      nullptr,
                                                      &error);
        if (!probe) {
            debug::log(WARN, "bluetooth: BlueZ unavailable: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            g_clear_object(&impl_->conn);
            return false;
        }
        impl_->objects = parse_managed_objects(probe);
        impl_->objects.experimental_api = bluetoothd_has_experimental();
        impl_->cap = resolve_capability(impl_->objects);
        g_variant_unref(probe);

        impl_->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (impl_->efd < 0) {
            debug::log(ERR, "bluetooth: eventfd() failed");
            g_clear_object(&impl_->conn);
            return false;
        }

        impl_->context = g_main_context_new();
        impl_->running = true;
        impl_->thread = std::thread([this] { impl_->run(); });
        return true;
    }

    void BluezMonitor::stop() {
        if (impl_->thread.joinable()) {
            g_main_context_invoke(impl_->context, quit_loop, impl_.get());
            impl_->thread.join();
        }
        if (impl_->loop) {
            g_main_loop_unref(impl_->loop);
            impl_->loop = nullptr;
        }
        if (impl_->context) {
            g_main_context_unref(impl_->context);
            impl_->context = nullptr;
        }
        g_clear_object(&impl_->conn);
        if (impl_->efd >= 0) {
            close(impl_->efd);
            impl_->efd = -1;
        }
    }

    bool BluezMonitor::running() const { return impl_->running; }

    int BluezMonitor::event_fd() const { return impl_->efd; }

    void BluezMonitor::drain() {
        if (impl_->efd < 0)
            return;
        uint64_t counter = 0;
        while (read(impl_->efd, &counter, sizeof(counter)) > 0) {
            // Drain every pending wakeup; the snapshot is read separately.
        }
    }

    GDBusConnection* BluezMonitor::connection() const { return impl_->conn; }

    void BluezMonitor::invoke_sync(const std::function<void()>& fn) {
        if (!impl_->context) {
            fn();
            return;
        }

        struct Job {
            const std::function<void()>* fn;
            std::mutex mutex;
            std::condition_variable done;
            bool finished = false;
        } job{&fn, {}, {}, false};

        g_main_context_invoke_full(
            impl_->context,
            G_PRIORITY_DEFAULT,
            [](gpointer data) -> gboolean {
                auto* j = static_cast<Job*>(data);
                (*j->fn)();
                {
                    std::lock_guard<std::mutex> lock(j->mutex);
                    j->finished = true;
                }
                j->done.notify_all();
                return G_SOURCE_REMOVE;
            },
            &job,
            nullptr);

        std::unique_lock<std::mutex> lock(job.mutex);
        job.done.wait(lock, [&job] { return job.finished; });
    }

    BluezObjects BluezMonitor::snapshot() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->objects;
    }

    Capability BluezMonitor::capability() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->cap;
    }

} // namespace tether::bluetooth
