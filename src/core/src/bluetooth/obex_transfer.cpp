#include "tether/bluetooth/obex_transfer.hpp"

#include <chrono>
#include <thread>

namespace tether::bluetooth {

    namespace {

        constexpr const char* OBEX_NAME = "org.bluez.obex";
        constexpr const char* IFACE_TRANSFER = "org.bluez.obex.Transfer1";
        constexpr const char* IFACE_PROPS = "org.freedesktop.DBus.Properties";
        constexpr int POLL_INTERVAL_MS = 200;

        TransferState poll_once(GDBusConnection* bus, const std::string& path) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(bus,
                                                          OBEX_NAME,
                                                          path.c_str(),
                                                          IFACE_PROPS,
                                                          "Get",
                                                          g_variant_new("(ss)", IFACE_TRANSFER, "Status"),
                                                          G_VARIANT_TYPE("(v)"),
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          5000,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                g_clear_error(&error);
                return TransferState::Gone;
            }

            GVariant* boxed = nullptr;
            g_variant_get(reply, "(v)", &boxed);
            std::string status;
            if (boxed && g_variant_is_of_type(boxed, G_VARIANT_TYPE_STRING))
                status = g_variant_get_string(boxed, nullptr);
            if (boxed)
                g_variant_unref(boxed);
            g_variant_unref(reply);

            if (status == "complete")
                return TransferState::Complete;
            if (status == "error")
                return TransferState::Error;
            return TransferState::Active;
        }

    } // namespace

    TransferState wait_for_transfer(GDBusConnection* bus, const std::string& path, int timeout_seconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        TransferState state = TransferState::Active;
        while (std::chrono::steady_clock::now() < deadline) {
            state = poll_once(bus, path);
            if (state != TransferState::Active)
                return state;
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
        }
        return state;
    }

} // namespace tether::bluetooth
