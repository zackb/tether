#include "tether/bluetooth/agent.hpp"
#include "tether/log.hpp"

#include <algorithm>
#include <array>

namespace tether::bluetooth {

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* AGENT_PATH = "/org/tether/bt_agent";
        constexpr const char* AGENT_MANAGER = "org.bluez.AgentManager1";
        // DisplayYesNo yields numeric comparison, the only method Tether supports.
        constexpr const char* AGENT_CAPABILITY = "DisplayYesNo";

        constexpr const char* AGENT_XML = R"XML(
<node>
  <interface name='org.bluez.Agent1'>
    <method name='Release'/>
    <method name='RequestPinCode'>
      <arg type='o' name='device' direction='in'/>
      <arg type='s' name='pincode' direction='out'/>
    </method>
    <method name='DisplayPinCode'>
      <arg type='o' name='device' direction='in'/>
      <arg type='s' name='pincode' direction='in'/>
    </method>
    <method name='RequestPasskey'>
      <arg type='o' name='device' direction='in'/>
      <arg type='u' name='passkey' direction='out'/>
    </method>
    <method name='DisplayPasskey'>
      <arg type='o' name='device' direction='in'/>
      <arg type='u' name='passkey' direction='in'/>
      <arg type='q' name='entered' direction='in'/>
    </method>
    <method name='RequestConfirmation'>
      <arg type='o' name='device' direction='in'/>
      <arg type='u' name='passkey' direction='in'/>
    </method>
    <method name='RequestAuthorization'>
      <arg type='o' name='device' direction='in'/>
    </method>
    <method name='AuthorizeService'>
      <arg type='o' name='device' direction='in'/>
      <arg type='s' name='uuid' direction='in'/>
    </method>
    <method name='Cancel'/>
  </interface>
</node>)XML";

        constexpr const char* ERROR_REJECTED = "org.bluez.Error.Rejected";

        bool iequals(const std::string& a, const std::string& b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
                       return std::tolower(x) == std::tolower(y);
                   });
        }

    } // namespace

    bool is_authorized_service(const std::string& uuid) {
        static const std::array<const char*, 5> allowed = {
            "0000112e-0000-1000-8000-00805f9b34fb", // PBAP client
            "0000112f-0000-1000-8000-00805f9b34fb", // PBAP server
            "00001132-0000-1000-8000-00805f9b34fb", // MAP server
            "00001133-0000-1000-8000-00805f9b34fb", // MAP notification server
            "00001134-0000-1000-8000-00805f9b34fb", // MAP client
        };
        return std::any_of(allowed.begin(), allowed.end(), [&](const char* u) { return iequals(uuid, u); });
    }

    std::string format_passkey(uint32_t passkey) {
        std::string digits = std::to_string(passkey % 1000000u);
        return std::string(6 - digits.size(), '0') + digits;
    }

    struct AgentState {
        GDBusConnection* conn = nullptr;
        std::string device_path;
        PairingAgent::ConfirmHandler on_confirm;
        std::string path = AGENT_PATH;
        guint registration_id = 0;
        bool registered_with_bluez = false;
    };

    namespace {

        void reject(GDBusMethodInvocation* invocation, const char* why) {
            g_dbus_method_invocation_return_dbus_error(invocation, ERROR_REJECTED, why);
        }

        void handle_method(GDBusConnection*,
                           const gchar*,
                           const gchar*,
                           const gchar*,
                           const gchar* method,
                           GVariant* params,
                           GDBusMethodInvocation* invocation,
                           gpointer user_data) {
            auto* state = static_cast<AgentState*>(user_data);
            std::string name = method ? method : "";

            if (name == "Release") {
                g_dbus_method_invocation_return_value(invocation, nullptr);
                return;
            }

            if (name == "Cancel") {
                g_dbus_method_invocation_return_value(invocation, nullptr);
                return;
            }

            // Every remaining method names a device first. Refuse anything that is
            // not the device this agent was created for, so an unrelated phone or
            // headset cannot ride along on a pairing the user started.
            const gchar* device = nullptr;
            g_variant_get_child(params, 0, "&o", &device);
            const std::string device_path = device ? device : "";
            if (device_path != state->device_path) {
                debug::log(WARN, "bluetooth: agent refused {} for unexpected device {}", name, device_path);
                reject(invocation, "Not the device being paired");
                return;
            }

            if (name == "RequestPinCode" || name == "DisplayPinCode" || name == "RequestPasskey" ||
                name == "DisplayPasskey") {
                debug::log(WARN, "bluetooth: agent refused unsupported pairing method {}", name);
                reject(invocation, "Only numeric comparison pairing is supported");
                return;
            }

            if (name == "AuthorizeService") {
                const gchar* uuid = nullptr;
                g_variant_get_child(params, 1, "&s", &uuid);
                const std::string service = uuid ? uuid : "";
                if (!is_authorized_service(service)) {
                    debug::log(INFO, "bluetooth: agent refused service {}", service);
                    reject(invocation, "Service not used by Tether");
                    return;
                }
                g_dbus_method_invocation_return_value(invocation, nullptr);
                return;
            }

            if (name == "RequestAuthorization") {
                // Pairing with no comparison to show the user. Accepting silently
                // would defeat the confirmation this agent exists to provide.
                reject(invocation, "Numeric comparison is required");
                return;
            }

            if (name == "RequestConfirmation") {
                guint32 passkey = 0;
                g_variant_get_child(params, 1, "u", &passkey);

                if (state->on_confirm && state->on_confirm(format_passkey(passkey)))
                    g_dbus_method_invocation_return_value(invocation, nullptr);
                else
                    reject(invocation, "Rejected by the user");
                return;
            }

            reject(invocation, "Unsupported method");
        }

        const GDBusInterfaceVTable AGENT_VTABLE = {handle_method, nullptr, nullptr, {nullptr}};

    } // namespace

    PairingAgent::PairingAgent(GDBusConnection* connection, std::string device_path, ConfirmHandler on_confirm)
        : state_(std::make_unique<AgentState>()) {
        state_->conn = connection;
        state_->device_path = std::move(device_path);
        state_->on_confirm = std::move(on_confirm);
    }

    PairingAgent::~PairingAgent() {
        unregister_with_bluez();
        unexport_object();
    }

    const std::string& PairingAgent::path() const { return state_->path; }

    bool PairingAgent::export_object() {
        if (state_->registration_id != 0)
            return true;

        GError* error = nullptr;
        GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(AGENT_XML, &error);
        if (!info) {
            debug::log(ERR, "bluetooth: bad agent introspection: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return false;
        }

        state_->registration_id = g_dbus_connection_register_object(
            state_->conn, state_->path.c_str(), info->interfaces[0], &AGENT_VTABLE, state_.get(), nullptr, &error);
        g_dbus_node_info_unref(info);

        if (state_->registration_id == 0) {
            debug::log(ERR, "bluetooth: cannot export agent: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return false;
        }
        return true;
    }

    bool PairingAgent::register_with_bluez() {
        if (state_->registered_with_bluez)
            return true;
        if (state_->registration_id == 0)
            return false;

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(state_->conn,
                                                      BLUEZ_NAME,
                                                      "/org/bluez",
                                                      AGENT_MANAGER,
                                                      "RegisterAgent",
                                                      g_variant_new("(os)", state_->path.c_str(), AGENT_CAPABILITY),
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      5000,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            debug::log(ERR, "bluetooth: RegisterAgent failed: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return false;
        }
        g_variant_unref(reply);
        state_->registered_with_bluez = true;

        reply = g_dbus_connection_call_sync(state_->conn,
                                            BLUEZ_NAME,
                                            "/org/bluez",
                                            AGENT_MANAGER,
                                            "RequestDefaultAgent",
                                            g_variant_new("(o)", state_->path.c_str()),
                                            nullptr,
                                            G_DBUS_CALL_FLAGS_NONE,
                                            5000,
                                            nullptr,
                                            &error);
        if (!reply) {
            debug::log(WARN, "bluetooth: RequestDefaultAgent failed: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            // Keep going: pairing can still succeed if no other agent competes.
        } else {
            g_variant_unref(reply);
        }
        return true;
    }

    void PairingAgent::unregister_with_bluez() {
        if (state_->registered_with_bluez) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(state_->conn,
                                                          BLUEZ_NAME,
                                                          "/org/bluez",
                                                          AGENT_MANAGER,
                                                          "UnregisterAgent",
                                                          g_variant_new("(o)", state_->path.c_str()),
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

    void PairingAgent::unexport_object() {
        if (state_->registration_id != 0) {
            g_dbus_connection_unregister_object(state_->conn, state_->registration_id);
            state_->registration_id = 0;
        }
    }

} // namespace tether::bluetooth
