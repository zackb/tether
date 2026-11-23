#include "tether/session_bus_watcher.hpp"
#include "tether/log.hpp"

#include <future>
#include <gio/gio.h>
#include <string>
#include <thread>

namespace tether {

    struct SessionBusWatcher::Impl {
        std::function<void()> on_closed;
        GMainContext* context = nullptr;
        GMainLoop* loop = nullptr;
        GDBusConnection* connection = nullptr;
        std::thread thread;

        static void closed(GDBusConnection*, gboolean remote_peer_vanished, GError* error, gpointer user_data) {
            auto* self = static_cast<Impl*>(user_data);
            // Ignore our own close on shutdown.
            if (!remote_peer_vanished)
                return;
            debug::log(WARN,
                       "session bus: disconnected ({}); everything using it is now cut off",
                       error ? error->message : "the bus went away");
            if (self->on_closed)
                self->on_closed();
        }
    };

    SessionBusWatcher::SessionBusWatcher(std::function<void()> on_closed) : impl_(std::make_unique<Impl>()) {
        impl_->on_closed = std::move(on_closed);
    }

    SessionBusWatcher::~SessionBusWatcher() {
        // Queued on the watcher's context so the quit lands even before g_main_loop_run().
        if (impl_->context && impl_->loop) {
            GSource* quit = g_idle_source_new();
            g_source_set_callback(
                quit,
                [](gpointer loop) -> gboolean {
                    g_main_loop_quit(static_cast<GMainLoop*>(loop));
                    return G_SOURCE_REMOVE;
                },
                impl_->loop,
                nullptr);
            g_source_attach(quit, impl_->context);
            g_source_unref(quit);
        }
        if (impl_->thread.joinable())
            impl_->thread.join();
        if (impl_->connection) {
            g_signal_handlers_disconnect_by_data(impl_->connection, impl_.get());
            g_object_unref(impl_->connection);
        }
        if (impl_->loop)
            g_main_loop_unref(impl_->loop);
        if (impl_->context)
            g_main_context_unref(impl_->context);
    }

    bool SessionBusWatcher::start() {
        if (impl_->thread.joinable())
            return true;

        GError* error = nullptr;
        gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!address) {
            debug::log(INFO, "session bus: none to watch ({})", error ? error->message : "unknown");
            g_clear_error(&error);
            return false;
        }

        impl_->context = g_main_context_new();
        impl_->loop = g_main_loop_new(impl_->context, FALSE);

        std::promise<bool> connected;
        auto result = connected.get_future();
        impl_->thread = std::thread([impl = impl_.get(), bus = std::string(address), &connected] {
            g_main_context_push_thread_default(impl->context);
            GError* err = nullptr;
            impl->connection = g_dbus_connection_new_for_address_sync(
                bus.c_str(),
                static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                  G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                nullptr,
                nullptr,
                &err);
            if (impl->connection) {
                // The default raises SIGTERM, a clean exit that systemd would not restart.
                g_dbus_connection_set_exit_on_close(impl->connection, FALSE);
                g_signal_connect(impl->connection, "closed", G_CALLBACK(Impl::closed), impl);
            } else {
                debug::log(WARN, "session bus: could not connect to watch it: {}", err ? err->message : "unknown");
                g_clear_error(&err);
            }
            const bool ok = impl->connection != nullptr;
            connected.set_value(ok);
            if (ok)
                g_main_loop_run(impl->loop);
            g_main_context_pop_thread_default(impl->context);
        });
        g_free(address);

        const bool ok = result.get();
        if (!ok)
            impl_->thread.join();
        return ok;
    }

} // namespace tether
