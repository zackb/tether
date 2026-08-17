#include "tether/bluetooth/objects.hpp"

#include <algorithm>
#include <cstring>

namespace tether::bluetooth {

    namespace {

        constexpr const char* IFACE_ADAPTER = "org.bluez.Adapter1";
        constexpr const char* IFACE_DEVICE = "org.bluez.Device1";
        constexpr const char* IFACE_LE_ADV_MGR = "org.bluez.LEAdvertisingManager1";
        constexpr const char* IFACE_BEARER_LE = "org.bluez.Bearer.LE1";

        bool iequals(const std::string& a, const std::string& b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
                       return std::tolower(x) == std::tolower(y);
                   });
        }

        // Property lookup helpers. Every one tolerates a missing key or an
        // unexpected type.
        GVariant* lookup(GVariant* props, const char* key) {
            if (!props || !g_variant_is_of_type(props, G_VARIANT_TYPE("a{sv}")))
                return nullptr;
            return g_variant_lookup_value(props, key, nullptr);
        }

        std::string get_string(GVariant* props, const char* key) {
            GVariant* v = lookup(props, key);
            if (!v)
                return {};
            std::string out;
            if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
                out = g_variant_get_string(v, nullptr);
            g_variant_unref(v);
            return out;
        }

        bool get_bool(GVariant* props, const char* key, bool fallback = false) {
            GVariant* v = lookup(props, key);
            if (!v)
                return fallback;
            bool out = fallback;
            if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
                out = g_variant_get_boolean(v);
            g_variant_unref(v);
            return out;
        }

        uint32_t get_uint32(GVariant* props, const char* key) {
            GVariant* v = lookup(props, key);
            if (!v)
                return 0;
            uint32_t out = 0;
            if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
                out = g_variant_get_uint32(v);
            g_variant_unref(v);
            return out;
        }

        uint8_t get_byte(GVariant* props, const char* key) {
            GVariant* v = lookup(props, key);
            if (!v)
                return 0;
            uint8_t out = 0;
            if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTE))
                out = g_variant_get_byte(v);
            g_variant_unref(v);
            return out;
        }

        std::vector<std::string> get_strv(GVariant* props, const char* key) {
            std::vector<std::string> out;
            GVariant* v = lookup(props, key);
            if (!v)
                return out;
            if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)) {
                gsize n = 0;
                const gchar** items = g_variant_get_strv(v, &n);
                if (items) {
                    out.reserve(n);
                    for (gsize i = 0; i < n; ++i)
                        out.emplace_back(items[i]);
                    g_free(items);
                }
            }
            g_variant_unref(v);
            return out;
        }

        // BlueZ nests the object path under the adapter, e.g.
        // /org/bluez/hci0/dev_AA_BB_.. the parent path is the owning adapter.
        std::string parent_path(const std::string& path) {
            size_t slash = path.rfind('/');
            if (slash == std::string::npos || slash == 0)
                return {};
            return path.substr(0, slash);
        }

        void read_adapter(const std::string& path, GVariant* ifaces, BluezObjects& out) {
            GVariant* props = g_variant_lookup_value(ifaces, IFACE_ADAPTER, G_VARIANT_TYPE("a{sv}"));
            if (!props)
                return;

            Adapter a;
            a.path = path;
            a.address = get_string(props, "Address");
            a.name = get_string(props, "Alias");
            if (a.name.empty())
                a.name = get_string(props, "Name");
            a.device_class = get_uint32(props, "Class");
            a.powered = get_bool(props, "Powered");
            a.roles = get_strv(props, "Roles");
            g_variant_unref(props);

            if (GVariant* adv = g_variant_lookup_value(ifaces, IFACE_LE_ADV_MGR, G_VARIANT_TYPE("a{sv}"))) {
                a.has_advertising_manager = true;
                a.advertising_instances = get_byte(adv, "SupportedInstances");
                g_variant_unref(adv);
            }

            out.adapters.push_back(std::move(a));
        }

        void read_device(const std::string& path, GVariant* ifaces, BluezObjects& out) {
            GVariant* props = g_variant_lookup_value(ifaces, IFACE_DEVICE, G_VARIANT_TYPE("a{sv}"));
            if (!props)
                return;

            Device d;
            d.path = path;
            d.adapter_path = get_string(props, "Adapter");
            if (d.adapter_path.empty())
                d.adapter_path = parent_path(path);
            d.address = get_string(props, "Address");
            d.name = get_string(props, "Alias");
            if (d.name.empty())
                d.name = get_string(props, "Name");
            d.paired = get_bool(props, "Paired");
            // Older BlueZ has no Bonded property; a paired device is bonded there.
            d.bonded = get_bool(props, "Bonded", d.paired);
            d.trusted = get_bool(props, "Trusted");
            d.connected = get_bool(props, "Connected");
            d.uuids = get_strv(props, "UUIDs");
            g_variant_unref(props);

            if (GVariant* le = g_variant_lookup_value(ifaces, IFACE_BEARER_LE, G_VARIANT_TYPE("a{sv}"))) {
                d.has_le_bearer = true;
                d.le_paired = get_bool(le, "Paired");
                d.le_bonded = get_bool(le, "Bonded", d.le_paired);
                d.le_connected = get_bool(le, "Connected");
                g_variant_unref(le);
            }

            out.devices.push_back(std::move(d));
        }

    } // namespace

    bool Adapter::has_role(const std::string& role) const {
        return std::find(roles.begin(), roles.end(), role) != roles.end();
    }

    bool Device::has_uuid(const std::string& uuid) const {
        return std::any_of(uuids.begin(), uuids.end(), [&](const std::string& u) { return iequals(u, uuid); });
    }

    bool Device::looks_like_iphone() const { return supports_ancs() || (supports_map() && supports_pbap()); }

    const Adapter* BluezObjects::find_adapter(const std::string& path) const {
        auto it = std::find_if(adapters.begin(), adapters.end(), [&](const Adapter& a) { return a.path == path; });
        return it == adapters.end() ? nullptr : &*it;
    }

    const Device* BluezObjects::find_device(const std::string& path) const {
        auto it = std::find_if(devices.begin(), devices.end(), [&](const Device& d) { return d.path == path; });
        return it == devices.end() ? nullptr : &*it;
    }

    const char* to_string(DeliveryMode mode) {
        switch (mode) {
        case DeliveryMode::Full:
            return "full";
        case DeliveryMode::Compatibility:
            return "compatibility";
        default:
            return "blocked";
        }
    }

    const char* to_string(BearerApi api) {
        switch (api) {
        case BearerApi::Confirmed:
            return "confirmed";
        case BearerApi::Absent:
            return "absent";
        default:
            return "unknown";
        }
    }

    BluezObjects parse_managed_objects(GVariant* reply) {
        BluezObjects out;
        if (!reply)
            return out;

        // g_dbus_connection_call returns the reply wrapped in a single-element
        // tuple; tests and callers may pass the dictionary directly.
        GVariant* dict = nullptr;
        if (g_variant_is_of_type(reply, G_VARIANT_TYPE("(a{oa{sa{sv}}})")))
            dict = g_variant_get_child_value(reply, 0);
        else if (g_variant_is_of_type(reply, G_VARIANT_TYPE("a{oa{sa{sv}}}")))
            dict = g_variant_ref(reply);
        else
            return out;

        GVariantIter iter;
        const gchar* path = nullptr;
        GVariant* ifaces = nullptr;
        g_variant_iter_init(&iter, dict);
        while (g_variant_iter_loop(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
            read_adapter(path, ifaces, out);
            read_device(path, ifaces, out);
        }
        g_variant_unref(dict);

        // BlueZ enumerates in a stable but unspecified order; sorting keeps
        // snapshot comparison and UI listing deterministic.
        std::sort(out.adapters.begin(), out.adapters.end(), [](const Adapter& a, const Adapter& b) {
            return a.path < b.path;
        });
        std::sort(
            out.devices.begin(), out.devices.end(), [](const Device& a, const Device& b) { return a.path < b.path; });
        return out;
    }

    Capability resolve_capability(const BluezObjects& objects) {
        Capability cap;

        // prefer powered adapter, fall back to the first so the reasons below
        // can explain an unpowered one rather than reporting nothing at all.
        const Adapter* adapter = nullptr;
        for (const auto& a : objects.adapters) {
            if (a.powered) {
                adapter = &a;
                break;
            }
        }
        if (!adapter && !objects.adapters.empty())
            adapter = &objects.adapters.front();

        if (!adapter) {
            cap.reasons.emplace_back("No Bluetooth adapter found.");
            return cap;
        }

        cap.adapter_present = true;
        cap.powered = adapter->powered;
        cap.le_central = adapter->has_role("central");
        cap.le_peripheral = adapter->has_role("peripheral");
        cap.advertising = adapter->has_advertising_manager && adapter->advertising_instances > 0;
        cap.class_ok = adapter->class_is_handsfree();

        for (const auto& d : objects.devices) {
            if (d.has_le_bearer)
                cap.bearer_api = BearerApi::Confirmed;
        }
        if (cap.bearer_api != BearerApi::Confirmed)
            cap.bearer_api = objects.experimental_api ? BearerApi::Unknown : BearerApi::Absent;

        if (!cap.powered) {
            cap.reasons.emplace_back("Bluetooth adapter is powered off.");
            cap.mode = DeliveryMode::Blocked;
            return cap;
        }
        if (!cap.le_central) {
            cap.reasons.emplace_back("Adapter has no LE central role; ANCS is impossible.");
            cap.mode = DeliveryMode::Blocked;
            return cap;
        }

        // Past this point messages and contacts are reachable; only ANCS is at risk.
        cap.mode = DeliveryMode::Full;

        if (!cap.le_peripheral) {
            cap.reasons.emplace_back("Adapter cannot advertise as a peripheral, so ANCS cannot be solicited.");
            cap.mode = DeliveryMode::Compatibility;
        }
        if (!cap.advertising) {
            cap.reasons.emplace_back("No LE advertising instances available; the iPhone may never show its "
                                     "Bluetooth permission toggles.");
            cap.mode = DeliveryMode::Compatibility;
        }
        if (cap.bearer_api == BearerApi::Absent) {
            cap.reasons.emplace_back("BlueZ is not exposing org.bluez.Bearer.LE1. Run bluetoothd with "
                                     "--experimental to enable notification mirroring.");
            cap.mode = DeliveryMode::Compatibility;
        } else if (cap.bearer_api == BearerApi::Unknown) {
            cap.reasons.emplace_back("Bearer API support is unconfirmed until a device is bonded.");
        }
        if (!cap.class_ok) {
            cap.reasons.emplace_back("Adapter Class of Device is not A/V Hands-Free (major 4, minor 8); "
                                     "the iPhone will not offer its Messages and Contacts permissions.");
        }

        return cap;
    }

    nlohmann::json to_json(const Adapter& a) {
        return {
            {"path", a.path},
            {"address", a.address},
            {"name", a.name},
            {"class", a.device_class},
            {"powered", a.powered},
            {"roles", a.roles},
            {"advertising_instances", a.advertising_instances},
            {"class_ok", a.class_is_handsfree()},
        };
    }

    nlohmann::json to_json(const Device& d) {
        return {
            {"path", d.path},
            {"adapter", d.adapter_path},
            {"address", d.address},
            {"name", d.name},
            {"paired", d.paired},
            {"bonded", d.bonded},
            {"trusted", d.trusted},
            {"connected", d.connected},
            {"le_bearer", d.has_le_bearer},
            {"le_bonded", d.le_bonded},
            {"le_connected", d.le_connected},
            {"map", d.supports_map()},
            {"pbap", d.supports_pbap()},
            {"ancs", d.supports_ancs()},
            {"iphone", d.looks_like_iphone()},
        };
    }

    nlohmann::json to_json(const Capability& c) {
        return {
            {"mode", to_string(c.mode)},
            {"bearer_api", to_string(c.bearer_api)},
            {"adapter_present", c.adapter_present},
            {"powered", c.powered},
            {"le_central", c.le_central},
            {"le_peripheral", c.le_peripheral},
            {"advertising", c.advertising},
            {"class_ok", c.class_ok},
            {"reasons", c.reasons},
        };
    }

} // namespace tether::bluetooth
