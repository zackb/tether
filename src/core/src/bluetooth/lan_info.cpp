#include "tether/bluetooth/lan_info.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/log.hpp"

#include <arpa/inet.h>
#include <bit>
#include <gio/gio.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <nlohmann/json.hpp>

namespace tether::bluetooth {

    std::vector<std::string> lan_addresses() {
        std::vector<std::string> v4;
        std::vector<std::string> v6;
        ifaddrs* head = nullptr;
        if (getifaddrs(&head) != 0)
            return {};

        for (const ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || !ifa->ifa_netmask || !(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
                continue;

            char text[INET6_ADDRSTRLEN] = {};
            int prefix = 0;
            if (ifa->ifa_addr->sa_family == AF_INET) {
                const auto* addr = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
                const auto* mask = reinterpret_cast<const sockaddr_in*>(ifa->ifa_netmask);
                inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text));
                prefix = std::popcount(static_cast<uint32_t>(mask->sin_addr.s_addr));
                v4.push_back(std::string(text) + "/" + std::to_string(prefix));
            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                const auto* addr = reinterpret_cast<const sockaddr_in6*>(ifa->ifa_addr);
                // Unusable without a scope id, which another host cannot know.
                if (IN6_IS_ADDR_LINKLOCAL(&addr->sin6_addr))
                    continue;
                const auto* mask = reinterpret_cast<const sockaddr_in6*>(ifa->ifa_netmask);
                inet_ntop(AF_INET6, &addr->sin6_addr, text, sizeof(text));
                for (uint8_t byte : mask->sin6_addr.s6_addr)
                    prefix += std::popcount(byte);
                v6.push_back(std::string(text) + "/" + std::to_string(prefix));
            }
        }
        freeifaddrs(head);

        v4.insert(v4.end(), v6.begin(), v6.end());
        return v4;
    }

    std::string encode_lan_info(const std::string& name,
                                uint16_t port,
                                const std::string& fingerprint,
                                std::vector<std::string> addresses) {
        while (true) {
            const nlohmann::json doc = {
                {"v", 1}, {"name", name}, {"port", port}, {"fp", fingerprint}, {"addrs", addresses}};
            std::string out = doc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            if (out.size() <= LAN_INFO_MAX_BYTES || addresses.empty())
                return out;
            addresses.pop_back();
        }
    }

    struct LanInfoState {
        GDBusConnection* conn = nullptr;
        std::string name;
        uint16_t port = 0;
        std::string fingerprint;

        guint root_id = 0;
        guint char_id = 0;
        guint watch_id = 0;
        guint added_id = 0;
        GCancellable* cancellable = nullptr;

        bool bluez_up = false;
        std::string wanted;        // adapter the daemon asked for
        std::string registered_on; // adapter BlueZ holds the application on, or one in flight
    };

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* GATT_MANAGER = "org.bluez.GattManager1";
        constexpr const char* APP_PATH = "/org/tether/lan";
        constexpr const char* SERVICE_PATH = "/org/tether/lan/service0";
        constexpr const char* CHAR_PATH = "/org/tether/lan/service0/char0";

        constexpr const char* LAN_XML = R"XML(
<node>
  <interface name='org.freedesktop.DBus.ObjectManager'>
    <method name='GetManagedObjects'>
      <arg type='a{oa{sa{sv}}}' direction='out'/>
    </method>
  </interface>
  <interface name='org.bluez.GattCharacteristic1'>
    <method name='ReadValue'>
      <arg type='a{sv}' direction='in'/>
      <arg type='ay' direction='out'/>
    </method>
  </interface>
</node>)XML";

        GVariant* interfaces(const char* iface, GVariant* props) {
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sa{sv}}"));
            g_variant_builder_add(&builder, "{s@a{sv}}", iface, props);
            return g_variant_builder_end(&builder);
        }

        // BlueZ learns the service and characteristic only from this reply, so the
        // service object needs no interface of its own.
        GVariant* managed_objects() {
            GVariantBuilder objects;
            g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));

            GVariantBuilder props;
            g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&props, "{sv}", "UUID", g_variant_new_string(UUID_TETHER_LAN_SERVICE));
            g_variant_builder_add(&props, "{sv}", "Primary", g_variant_new_boolean(TRUE));
            g_variant_builder_add(&objects, "{o@a{sa{sv}}}", SERVICE_PATH,
                                  interfaces("org.bluez.GattService1", g_variant_builder_end(&props)));

            const gchar* flags[] = {"encrypt-read"};
            g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&props, "{sv}", "UUID", g_variant_new_string(UUID_TETHER_LAN_INFO));
            g_variant_builder_add(&props, "{sv}", "Service", g_variant_new_object_path(SERVICE_PATH));
            g_variant_builder_add(&props, "{sv}", "Flags", g_variant_new_strv(flags, 1));
            g_variant_builder_add(&objects, "{o@a{sa{sv}}}", CHAR_PATH,
                                  interfaces("org.bluez.GattCharacteristic1", g_variant_builder_end(&props)));

            return g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&objects));
        }

        void handle_method(GDBusConnection*,
                           const gchar*,
                           const gchar*,
                           const gchar*,
                           const gchar* method,
                           GVariant* parameters,
                           GDBusMethodInvocation* invocation,
                           gpointer user_data) {
            if (g_strcmp0(method, "GetManagedObjects") == 0) {
                g_dbus_method_invocation_return_value(invocation, managed_objects());
                return;
            }

            const auto* state = static_cast<LanInfoState*>(user_data);
            GVariant* options = g_variant_get_child_value(parameters, 0);
            guint16 offset = 0;
            g_variant_lookup(options, "offset", "q", &offset);
            g_variant_unref(options);

            const std::string value =
                encode_lan_info(state->name, state->port, state->fingerprint, lan_addresses());
            if (offset > value.size()) {
                g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.InvalidOffset", "");
                return;
            }
            GVariant* bytes = g_variant_new_fixed_array(
                G_VARIANT_TYPE_BYTE, value.data() + offset, value.size() - offset, sizeof(guint8));
            g_dbus_method_invocation_return_value(invocation, g_variant_new_tuple(&bytes, 1));
        }

        const GDBusInterfaceVTable LAN_VTABLE = {handle_method, nullptr, nullptr, {nullptr}};

        // Reads only the async result before touching state: a cancelled call means state is gone.
        void on_registered(GObject* source, GAsyncResult* result, gpointer user_data) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
            if (reply) {
                g_variant_unref(reply);
                debug::log(INFO, "bluetooth: LAN info service registered on {}",
                           static_cast<LanInfoState*>(user_data)->registered_on);
                return;
            }
            const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
            const std::string message = error && error->message ? error->message : "unknown error";
            g_clear_error(&error);
            if (cancelled || message.find("AlreadyExists") != std::string::npos)
                return;
            // A restarted bluetoothd takes the name before it exports the adapter;
            // on_interfaces_added retries once GattManager1 shows up.
            if (message.find("UnknownObject") != std::string::npos)
                return;
            // No retry until the adapter changes or bluetoothd comes back; mDNS still works.
            debug::log(INFO, "bluetooth: LAN info service not registered: {}", message);
        }

        void call_manager(LanInfoState* state, const std::string& adapter, const char* method, GVariant* args,
                          GAsyncReadyCallback done) {
            g_dbus_connection_call(state->conn, BLUEZ_NAME, adapter.c_str(), GATT_MANAGER, method, args, nullptr,
                                   G_DBUS_CALL_FLAGS_NONE, 10000, state->cancellable, done, state);
        }

        // Monitor thread only. Calls are async because BlueZ answers RegisterApplication
        // only after calling GetManagedObjects back on this same thread.
        void reconcile(LanInfoState* state) {
            if (!state->bluez_up || state->registered_on == state->wanted)
                return;
            if (!state->registered_on.empty())
                call_manager(state, state->registered_on, "UnregisterApplication",
                             g_variant_new("(o)", APP_PATH), nullptr);
            state->registered_on = state->wanted;
            if (state->wanted.empty())
                return;
            GVariantBuilder options;
            g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
            call_manager(state, state->wanted, "RegisterApplication",
                         g_variant_new("(oa{sv})", APP_PATH, &options), on_registered);
        }

        void on_bluez_appeared(GDBusConnection*, const gchar*, const gchar*, gpointer user_data) {
            auto* state = static_cast<LanInfoState*>(user_data);
            state->bluez_up = true;
            reconcile(state);
        }

        void on_interfaces_added(GDBusConnection*,
                                 const gchar*,
                                 const gchar*,
                                 const gchar*,
                                 const gchar*,
                                 GVariant* parameters,
                                 gpointer user_data) {
            auto* state = static_cast<LanInfoState*>(user_data);
            const gchar* path = nullptr;
            GVariant* added = nullptr;
            g_variant_get(parameters, "(&o@a{sa{sv}})", &path, &added);
            GVariant* manager_props = g_variant_lookup_value(added, GATT_MANAGER, nullptr);
            const bool manager = manager_props != nullptr;
            if (manager_props)
                g_variant_unref(manager_props);
            g_variant_unref(added);
            if (!manager || state->wanted != path)
                return;
            state->registered_on.clear();
            reconcile(state);
        }

        // bluetoothd forgets every application when it exits.
        void on_bluez_vanished(GDBusConnection*, const gchar*, gpointer user_data) {
            auto* state = static_cast<LanInfoState*>(user_data);
            state->bluez_up = false;
            state->registered_on.clear();
        }

    } // namespace

    LanInfoService::LanInfoService(BluezMonitor& monitor, std::string name, uint16_t port, std::string fingerprint)
        : monitor_(monitor), state_(std::make_unique<LanInfoState>()) {
        state_->conn = monitor.connection();
        state_->name = std::move(name);
        state_->port = port;
        state_->fingerprint = std::move(fingerprint);

        monitor_.invoke_sync([state = state_.get()] {
            GError* error = nullptr;
            GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(LAN_XML, &error);
            if (!info) {
                debug::log(ERR, "bluetooth: bad LAN info introspection: {}", error ? error->message : "unknown");
                g_clear_error(&error);
                return;
            }
            state->root_id = g_dbus_connection_register_object(
                state->conn, APP_PATH, info->interfaces[0], &LAN_VTABLE, state, nullptr, &error);
            g_clear_error(&error);
            state->char_id = g_dbus_connection_register_object(
                state->conn, CHAR_PATH, info->interfaces[1], &LAN_VTABLE, state, nullptr, &error);
            g_dbus_node_info_unref(info);
            if (state->root_id == 0 || state->char_id == 0) {
                debug::log(ERR, "bluetooth: cannot export LAN info service: {}", error ? error->message : "unknown");
                g_clear_error(&error);
                return;
            }

            state->cancellable = g_cancellable_new();
            // Watches on this thread's context, so the callbacks run here too.
            state->watch_id = g_bus_watch_name_on_connection(state->conn, BLUEZ_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                             on_bluez_appeared, on_bluez_vanished, state, nullptr);
            state->added_id = g_dbus_connection_signal_subscribe(state->conn, BLUEZ_NAME,
                                                                 "org.freedesktop.DBus.ObjectManager",
                                                                 "InterfacesAdded", "/", nullptr,
                                                                 G_DBUS_SIGNAL_FLAGS_NONE, on_interfaces_added,
                                                                 state, nullptr);
        });
    }

    LanInfoService::~LanInfoService() {
        monitor_.invoke_sync([state = state_.get()] {
            if (state->watch_id != 0)
                g_bus_unwatch_name(state->watch_id);
            if (state->added_id != 0)
                g_dbus_connection_signal_unsubscribe(state->conn, state->added_id);
            if (state->cancellable)
                g_cancellable_cancel(state->cancellable);

            if (state->bluez_up && !state->registered_on.empty()) {
                GVariant* reply = g_dbus_connection_call_sync(state->conn, BLUEZ_NAME, state->registered_on.c_str(),
                                                              GATT_MANAGER, "UnregisterApplication",
                                                              g_variant_new("(o)", APP_PATH), nullptr,
                                                              G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
                if (reply)
                    g_variant_unref(reply);
            }

            if (state->char_id != 0)
                g_dbus_connection_unregister_object(state->conn, state->char_id);
            if (state->root_id != 0)
                g_dbus_connection_unregister_object(state->conn, state->root_id);
            g_clear_object(&state->cancellable);
        });
    }

    void LanInfoService::set_adapter(const std::string& adapter_path) {
        if (adapter_path == adapter_)
            return;
        adapter_ = adapter_path;
        monitor_.invoke_sync([state = state_.get(), adapter_path] {
            if (state->watch_id == 0)
                return;
            state->wanted = adapter_path;
            reconcile(state);
        });
    }

} // namespace tether::bluetooth
