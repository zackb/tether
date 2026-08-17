#include "tether/bluetooth/advert.hpp"
#include "tether/bluetooth/objects.hpp"
#include "tether/log.hpp"

namespace tether::bluetooth {

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* ADVERT_PATH = "/org/tether/bt_advert";
        constexpr const char* ADVERT_MANAGER = "org.bluez.LEAdvertisingManager1";

        // Private/test identifiers that claim no Apple or hardware-vendor
        // identity, following the convention ancs4linux established. No meaning; they exist because the advert iOS
        // responds to carries them.
        constexpr guint16 INERT_MANUFACTURER_ID = 0xffff;
        constexpr const char* INERT_SERVICE_UUID = "00009999-0000-1000-8000-00805f9b34fb";

        // iOS needs the it running long enough to notice
        constexpr guint16 ADVERT_TIMEOUT_SECONDS = 180;

        constexpr const char* ADVERT_XML = R"XML(
<node>
  <interface name='org.bluez.LEAdvertisement1'>
    <method name='Release'/>
    <property name='Type' type='s' access='read'/>
    <property name='SolicitUUIDs' type='as' access='read'/>
    <property name='ManufacturerData' type='a{qv}' access='read'/>
    <property name='ServiceData' type='a{sv}' access='read'/>
    <property name='Discoverable' type='b' access='read'/>
    <property name='Timeout' type='q' access='read'/>
    <property name='LocalName' type='s' access='read'/>
  </interface>
</node>)XML";

        GVariant* inert_bytes() {
            const guint8 payload[] = {0x00};
            return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, payload, sizeof(payload), sizeof(guint8));
        }

        void handle_method(GDBusConnection*,
                           const gchar*,
                           const gchar*,
                           const gchar*,
                           const gchar* method,
                           GVariant*,
                           GDBusMethodInvocation* invocation,
                           gpointer) {
            // BlueZ calls Release when it drops the advertisement on its own.
            if (g_strcmp0(method, "Release") == 0)
                debug::log(INFO, "bluetooth: ANCS advertisement released by BlueZ");
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }

        GVariant* handle_get_property(
            GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* name, GError**, gpointer) {
            const std::string prop = name ? name : "";

            if (prop == "Type")
                return g_variant_new_string("peripheral");

            if (prop == "SolicitUUIDs") {
                GVariantBuilder builder;
                g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
                g_variant_builder_add(&builder, "s", UUID_ANCS);
                return g_variant_builder_end(&builder);
            }

            if (prop == "ManufacturerData") {
                GVariantBuilder builder;
                g_variant_builder_init(&builder, G_VARIANT_TYPE("a{qv}"));
                g_variant_builder_add(&builder, "{qv}", INERT_MANUFACTURER_ID, inert_bytes());
                return g_variant_builder_end(&builder);
            }

            if (prop == "ServiceData") {
                GVariantBuilder builder;
                g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
                g_variant_builder_add(&builder, "{sv}", INERT_SERVICE_UUID, inert_bytes());
                return g_variant_builder_end(&builder);
            }

            if (prop == "Discoverable")
                return g_variant_new_boolean(TRUE);

            if (prop == "Timeout")
                return g_variant_new_uint16(ADVERT_TIMEOUT_SECONDS);

            if (prop == "LocalName")
                return g_variant_new_string("Tether");

            return nullptr;
        }

        const GDBusInterfaceVTable ADVERT_VTABLE = {handle_method, handle_get_property, nullptr, {nullptr}};

    } // namespace

    struct AdvertState {
        GDBusConnection* conn = nullptr;
        std::string adapter_path;
        guint registration_id = 0;
        bool registered_with_bluez = false;
    };

    AncsAdvertisement::AncsAdvertisement(GDBusConnection* connection, std::string adapter_path)
        : state_(std::make_unique<AdvertState>()) {
        state_->conn = connection;
        state_->adapter_path = std::move(adapter_path);
    }

    AncsAdvertisement::~AncsAdvertisement() {
        unregister_with_bluez();
        unexport_object();
    }

    bool AncsAdvertisement::active() const { return state_->registered_with_bluez; }

    bool AncsAdvertisement::export_object() {
        if (state_->registration_id != 0)
            return true;

        GError* error = nullptr;
        GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(ADVERT_XML, &error);
        if (!info) {
            debug::log(ERR, "bluetooth: bad advertisement introspection: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return false;
        }

        state_->registration_id = g_dbus_connection_register_object(
            state_->conn, ADVERT_PATH, info->interfaces[0], &ADVERT_VTABLE, state_.get(), nullptr, &error);
        g_dbus_node_info_unref(info);

        if (state_->registration_id == 0) {
            debug::log(ERR, "bluetooth: cannot export advertisement: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return false;
        }
        return true;
    }

    bool AncsAdvertisement::register_with_bluez() {
        if (state_->registered_with_bluez)
            return true;
        if (state_->registration_id == 0)
            return false;

        GError* error = nullptr;
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        GVariant* reply = g_dbus_connection_call_sync(state_->conn,
                                                      BLUEZ_NAME,
                                                      state_->adapter_path.c_str(),
                                                      ADVERT_MANAGER,
                                                      "RegisterAdvertisement",
                                                      g_variant_new("(oa{sv})", ADVERT_PATH, &options),
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      10000,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            debug::log(ERR, "bluetooth: RegisterAdvertisement failed: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return false;
        }
        g_variant_unref(reply);

        state_->registered_with_bluez = true;
        debug::log(INFO, "bluetooth: soliciting ANCS for {}s", ADVERT_TIMEOUT_SECONDS);
        return true;
    }

    void AncsAdvertisement::unregister_with_bluez() {
        if (state_->registered_with_bluez) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(state_->conn,
                                                          BLUEZ_NAME,
                                                          state_->adapter_path.c_str(),
                                                          ADVERT_MANAGER,
                                                          "UnregisterAdvertisement",
                                                          g_variant_new("(o)", ADVERT_PATH),
                                                          nullptr,
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          5000,
                                                          nullptr,
                                                          &error);
            if (reply)
                g_variant_unref(reply);
            g_clear_error(&error);
            state_->registered_with_bluez = false;
        }
    }

    void AncsAdvertisement::unexport_object() {
        if (state_->registration_id != 0) {
            g_dbus_connection_unregister_object(state_->conn, state_->registration_id);
            state_->registration_id = 0;
        }
    }

} // namespace tether::bluetooth
