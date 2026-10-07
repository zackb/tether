#include "tether/bluetooth/objects.hpp"
#include <tether/i18n.hpp>
#include <tether/packaging.hpp>
#include <tether/service.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace tether::bluetooth {

    namespace {

        constexpr const char* IFACE_ADAPTER = "org.bluez.Adapter1";
        constexpr const char* IFACE_DEVICE = "org.bluez.Device1";
        constexpr const char* IFACE_TELEPHONY = "org.bluez.Telephony1";
        constexpr const char* IFACE_CALL = "org.bluez.Call1";
        constexpr const char* IFACE_LE_ADV_MGR = "org.bluez.LEAdvertisingManager1";
        constexpr const char* IFACE_ADV_MONITOR_MGR = "org.bluez.AdvertisementMonitorManager1";
        constexpr const char* IFACE_BEARER_LE = "org.bluez.Bearer.LE1";
        constexpr const char* IFACE_BEARER_BREDR = "org.bluez.Bearer.BREDR1";
        constexpr const char* IFACE_CHARACTERISTIC = "org.bluez.GattCharacteristic1";
        constexpr const char* UUID_ANCS_NOTIFICATION_SOURCE = "9fbf120d-6301-42d9-8c58-25e699a21dbd";
        constexpr const char* UUID_GAP_DEVICE_NAME = "00002a00-0000-1000-8000-00805f9b34fb";

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

        bool has_apple_nearby_advertisement(GVariant* props) {
            GVariant* entries = lookup(props, "ManufacturerData");
            if (!entries || !g_variant_is_of_type(entries, G_VARIANT_TYPE("a{qv}"))) {
                if (entries)
                    g_variant_unref(entries);
                return false;
            }

            bool found = false;
            GVariantIter iter;
            guint16 company = 0;
            GVariant* value = nullptr;
            g_variant_iter_init(&iter, entries);
            while (g_variant_iter_next(&iter, "{qv}", &company, &value)) {
                GVariant* payload = value;
                if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
                    payload = g_variant_get_variant(value);
                    g_variant_unref(value);
                }
                if (company == 0x004c && g_variant_is_of_type(payload, G_VARIANT_TYPE_BYTESTRING)) {
                    gsize size = 0;
                    const auto* bytes = static_cast<const guint8*>(g_variant_get_fixed_array(payload, &size, 1));
                    // 0x10 is Apple's Nearby Info advertisement. Unlike the
                    // service UUIDs it is visible while an unpaired iPhone is
                    // still using a private rotating address.
                    found = bytes && size > 0 && bytes[0] == 0x10;
                }
                g_variant_unref(payload);
                if (found)
                    break;
            }
            g_variant_unref(entries);
            return found;
        }

        // BlueZ nests the object path under the adapter, e.g.
        // /org/bluez/hci0/dev_AA_BB_.. the parent path is the owning adapter.
        std::string parent_path(const std::string& path) {
            size_t slash = path.rfind('/');
            if (slash == std::string::npos || slash == 0)
                return {};
            return path.substr(0, slash);
        }

        // The chip, which BlueZ does not report.
        std::string controller_of(const std::string& adapter_path) {
            const auto slash = adapter_path.find_last_of('/');
            const std::string id = slash == std::string::npos ? adapter_path : adapter_path.substr(slash + 1);
            if (id.empty())
                return {};

            std::ifstream in("/sys/class/bluetooth/" + id + "/device/modalias");
            std::string line;
            std::getline(in, line);
            return line;
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
            a.modalias = get_string(props, "Modalias");
            a.controller = controller_of(path);
            g_variant_unref(props);

            if (GVariant* adv = g_variant_lookup_value(ifaces, IFACE_LE_ADV_MGR, G_VARIANT_TYPE("a{sv}"))) {
                a.has_advertising_manager = true;
                a.advertising_instances = get_byte(adv, "SupportedInstances");
                a.advertising_active_instances = get_byte(adv, "ActiveInstances");
                g_variant_unref(adv);
            }

            if (GVariant* mon = g_variant_lookup_value(ifaces, IFACE_ADV_MONITOR_MGR, G_VARIANT_TYPE("a{sv}"))) {
                a.has_adv_monitor_manager = true;
                g_variant_unref(mon);
            }

            out.adapters.push_back(std::move(a));
        }

        // BlueZ exports these only with --experimental, and only while HFP is connected, absence is the normal state.
        void read_telephony(const std::string& path, GVariant* ifaces, BluezObjects& out) {
            GVariant* props = g_variant_lookup_value(ifaces, IFACE_TELEPHONY, G_VARIANT_TYPE("a{sv}"));
            if (!props)
                return;

            Telephony t;
            t.path = path;
            t.device_path = parent_path(path);
            t.uuid = get_string(props, "UUID");
            t.state = get_string(props, "State");
            t.operator_name = get_string(props, "OperatorName");
            t.uri_schemes = get_strv(props, "SupportedURISchemes");
            t.signal = get_byte(props, "Signal");
            t.battery = get_byte(props, "BattChg");
            t.service = get_bool(props, "Service");
            t.roaming = get_bool(props, "Roaming");
            t.inband_ringtone = get_bool(props, "InbandRingtone");
            g_variant_unref(props);

            out.telephony.push_back(std::move(t));
        }

        void read_call(const std::string& path, GVariant* ifaces, BluezObjects& out) {
            GVariant* props = g_variant_lookup_value(ifaces, IFACE_CALL, G_VARIANT_TYPE("a{sv}"));
            if (!props)
                return;

            Call c;
            c.path = path;
            c.telephony_path = parent_path(path);
            c.number = get_string(props, "LineIdentification");
            c.name = get_string(props, "Name");
            c.incoming_line = get_string(props, "IncomingLine");
            c.state = get_string(props, "State");
            c.multiparty = get_bool(props, "Multiparty");
            g_variant_unref(props);

            out.calls.push_back(std::move(c));
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
            d.modalias = get_string(props, "Modalias");
            d.apple_nearby = has_apple_nearby_advertisement(props);
            d.preferred_bearer = get_string(props, "PreferredBearer");
            d.services_resolved = get_bool(props, "ServicesResolved");
            g_variant_unref(props);

            if (GVariant* le = g_variant_lookup_value(ifaces, IFACE_BEARER_LE, G_VARIANT_TYPE("a{sv}"))) {
                // without --experimental bluetoothd still registers the interface name, but empty.
                d.has_le_bearer = g_variant_n_children(le) > 0;
                d.le_paired = get_bool(le, "Paired");
                d.le_bonded = get_bool(le, "Bonded", d.le_paired);
                d.le_connected = get_bool(le, "Connected");
                g_variant_unref(le);
            }

            if (GVariant* br = g_variant_lookup_value(ifaces, IFACE_BEARER_BREDR, G_VARIANT_TYPE("a{sv}"))) {
                d.has_classic_bearer = true;
                if (GVariant* connected = g_variant_lookup_value(br, "Connected", G_VARIANT_TYPE_BOOLEAN)) {
                    d.classic_state_known = true;
                    d.classic_connected = g_variant_get_boolean(connected);
                    g_variant_unref(connected);
                }
                d.classic_paired = get_bool(br, "Paired");
                d.classic_bonded = get_bool(br, "Bonded", d.classic_paired);
                g_variant_unref(br);
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

    namespace {
        std::string lowered(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return text;
        }
    } // namespace

    bool Adapter::presents_as_apple() const { return lowered(modalias).rfind(MODALIAS_APPLE, 0) == 0; }

    bool Device::looks_like_airpods() const {
        // An iPhone carries the same vendor id, so the audio profile is what separates them.
        if (looks_like_iphone() || !has_uuid(UUID_A2DP_SINK))
            return false;
        if (lowered(modalias).rfind(MODALIAS_APPLE, 0) == 0)
            return true;
        // Modalias is absent until SDP has been read at least once.
        return lowered(name).find("airpod") != std::string::npos;
    }

    const Adapter* BluezObjects::find_adapter(const std::string& path) const {
        auto it = std::find_if(adapters.begin(), adapters.end(), [&](const Adapter& a) { return a.path == path; });
        return it == adapters.end() ? nullptr : &*it;
    }

    const Device* BluezObjects::find_device(const std::string& path) const {
        auto it = std::find_if(devices.begin(), devices.end(), [&](const Device& d) { return d.path == path; });
        return it == devices.end() ? nullptr : &*it;
    }

    const Telephony* BluezObjects::find_telephony(const std::string& device_path) const {
        if (device_path.empty())
            return nullptr;
        auto it = std::find_if(
            telephony.begin(), telephony.end(), [&](const Telephony& t) { return t.device_path == device_path; });
        return it == telephony.end() ? nullptr : &*it;
    }

    std::vector<Call> BluezObjects::calls_for(const std::string& telephony_path) const {
        std::vector<Call> out;
        if (telephony_path.empty())
            return out;
        std::copy_if(calls.begin(), calls.end(), std::back_inserter(out), [&](const Call& c) {
            return c.telephony_path == telephony_path;
        });
        return out;
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
        std::vector<std::string> ancs_char_paths;
        std::vector<std::string> gap_name_paths;
        g_variant_iter_init(&iter, dict);
        while (g_variant_iter_loop(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
            read_adapter(path, ifaces, out);
            read_device(path, ifaces, out);
            read_telephony(path, ifaces, out);
            read_call(path, ifaces, out);

            GVariant* chr = g_variant_lookup_value(ifaces, IFACE_CHARACTERISTIC, G_VARIANT_TYPE("a{sv}"));
            if (!chr)
                continue;
            const std::string uuid = get_string(chr, "UUID");
            if (iequals(uuid, UUID_ANCS_NOTIFICATION_SOURCE) && get_bool(chr, "Notifying"))
                ancs_char_paths.emplace_back(path);
            else if (iequals(uuid, UUID_GAP_DEVICE_NAME))
                gap_name_paths.emplace_back(path);
            g_variant_unref(chr);
        }
        g_variant_unref(dict);

        // A characteristic lives under its device's path. BlueZ enumerates in an
        // unspecified order, so this cannot be done in the loop above.
        for (const auto& char_path : ancs_char_paths) {
            for (auto& device : out.devices) {
                if (char_path.rfind(device.path + "/", 0) == 0)
                    device.ancs_notifying = true;
            }
        }
        for (const auto& char_path : gap_name_paths) {
            for (auto& device : out.devices) {
                if (char_path.rfind(device.path + "/", 0) == 0)
                    device.gap_name_path = char_path;
            }
        }

        // bluetoothd builds the advertisement monitor manager only when the experimental API is on
        out.experimental_api = std::any_of(
            out.adapters.begin(), out.adapters.end(), [](const Adapter& a) { return a.has_adv_monitor_manager; });

        // BlueZ enumerates in a stable but unspecified order; sorting keeps
        // snapshot comparison and UI listing deterministic.
        std::sort(out.adapters.begin(), out.adapters.end(), [](const Adapter& a, const Adapter& b) {
            return a.path < b.path;
        });
        std::sort(
            out.devices.begin(), out.devices.end(), [](const Device& a, const Device& b) { return a.path < b.path; });
        std::sort(out.telephony.begin(), out.telephony.end(), [](const Telephony& a, const Telephony& b) {
            return a.path < b.path;
        });
        std::sort(out.calls.begin(), out.calls.end(), [](const Call& a, const Call& b) { return a.path < b.path; });
        return out;
    }

    namespace {

        // Arch = /usr/lib, Debian and Fedora = /usr/libexec. Empty when neither is
        // visible (sandbox): its /usr is the runtime's.
        std::string bluetoothd_path() {
            for (const char* candidate : {"/usr/lib/bluetooth/bluetoothd", "/usr/libexec/bluetooth/bluetoothd"}) {
                std::error_code ec;
                if (std::filesystem::exists(candidate, ec))
                    return candidate;
            }
            return {};
        }

    } // namespace

    // bluetoothd's path is distro-specific, this command resolves it in the user's shell instead of hardcoding.
    std::string enable_experimental_command(bool systemd) {
        // main.conf's Experimental is the same switch as --experimental, and no init system is involved.
        if (!systemd)
            return "sudo sed -i -E 's/^#?[[:space:]]*Experimental[[:space:]]*=.*/Experimental = true/' "
                   "/etc/bluetooth/main.conf\n"
                   "# then restart bluetoothd with this system's service manager";

        const std::string found = bluetoothd_path();
        const std::string binary = found.empty()
                                       ? "$(ls /usr/lib/bluetooth/bluetoothd /usr/libexec/bluetooth/bluetoothd "
                                         "2>/dev/null | head -1)"
                                       : found;

        return "sudo mkdir -p /etc/systemd/system/bluetooth.service.d\n"
               "printf '[Service]\\nExecStart=\\nExecStart=%s --experimental\\n' \\\n"
               "  \"" +
               binary +
               "\" \\\n"
               "  | sudo tee /etc/systemd/system/bluetooth.service.d/experimental.conf\n"
               "sudo systemctl daemon-reload && sudo systemctl restart bluetooth";
    }

    // Distro packages install the unit. A portable build writes it out.
    std::string set_class_command(const std::string& adapter_id, bool systemd) {
        // Without hostnamed, BlueZ's hostname plugin leaves main.conf's Class alone across restarts.
        // Checked before the unit probe: an Arch package on Artix still ships the unit file.
        if (!systemd)
            return "sudo sed -i -E 's/^#?[[:space:]]*Class[[:space:]]*=.*/Class = 0x000408/' "
                   "/etc/bluetooth/main.conf\n"
                   "echo | sudo btmgmt --index " +
                   adapter_id + " class 4 8";

        const std::string enable = "sudo systemctl enable --now tether-btclass@" + adapter_id;
        std::error_code ec;
        for (const char* dir : {"/etc/systemd/system",
                                "/usr/lib/systemd/system",
                                "/lib/systemd/system",
                                "/usr/local/lib/systemd/system"}) {
            if (std::filesystem::exists(std::string(dir) + "/tether-btclass@.service", ec))
                return enable;
        }

        // The AppImage can write the unit itself, which beats pasting it. Flatpak cannot:
        // "flatpak run" under sudo is the wrong user, so it falls through to the here-document.
        if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage)
            return "sudo \"" + std::string(appimage) +
                   "\" --install-btclass-unit\n"
                   "sudo systemctl daemon-reload\n" +
                   enable;

        return "sudo tee /etc/systemd/system/tether-btclass@.service >/dev/null <<'EOF'\n" +
               std::string(packaging::BTCLASS_UNIT) +
               "EOF\n"
               "sudo systemctl daemon-reload\n" +
               enable;
    }

    namespace {

        // /org/bluez/hci0 -> hci0
        std::string adapter_id_of(const std::string& path) {
            const auto slash = path.find_last_of('/');
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

    } // namespace

    const Adapter* preferred_adapter(const BluezObjects& objects, const std::string& id) {
        if (!id.empty()) {
            for (const auto& a : objects.adapters) {
                if (iequals(adapter_id_of(a.path), id) || iequals(a.address, id))
                    return &a;
            }
        }
        for (const auto& a : objects.adapters) {
            if (a.powered)
                return &a;
        }
        return objects.adapters.empty() ? nullptr : &objects.adapters.front();
    }

    Capability resolve_capability(const BluezObjects& objects,
                                  std::optional<bool> secure_connections,
                                  const std::string& preferred_id) {
        Capability cap;

        const Adapter* adapter = preferred_adapter(objects, preferred_id);

        // ponytail: tetherd is per-user, so its locale is the user's and these
        // strings can be translated here. A system-wide daemon serving several
        // users would have to send codes and let each client translate them.
        if (!adapter) {
            cap.reasons.emplace_back(_("No Bluetooth adapter found."));
            return cap;
        }

        cap.adapter_present = true;
        cap.adapter_id = adapter_id_of(adapter->path);
        cap.powered = adapter->powered;
        cap.le_central = adapter->has_role("central");
        cap.le_peripheral = adapter->has_role("peripheral");
        cap.advertising = adapter->has_advertising_manager && adapter->advertising_instances > 0;
        cap.class_ok = adapter->class_is_handsfree();

        // Only a populated Bearer.LE1 settles this. Bearer.BREDR1 is marker-only on
        // some packaged builds, so it is not evidence either way.
        for (const auto& d : objects.devices) {
            if (d.has_le_bearer)
                cap.bearer_api = BearerApi::Confirmed;

            if ((d.bonded || d.paired) && d.looks_like_iphone()) {
                cap.bonded_device_present = true;
                // The interface being populated says the API is live, not that the
                // bond has an LE half. Only Bearer.LE1.Bonded says that.
                if (d.has_le_bearer && d.le_bonded)
                    cap.bond_has_le = true;
                if (d.ancs_notifying)
                    cap.bond_carries_ancs = true;
            }
        }
        if (cap.bearer_api != BearerApi::Confirmed)
            cap.bearer_api = objects.experimental_api ? BearerApi::Unknown : BearerApi::Absent;

        if (secure_connections.has_value()) {
            cap.secure_connections_known = true;
            cap.secure_connections = *secure_connections;
        }

        if (!cap.powered) {
            cap.reasons.emplace_back(_("Bluetooth adapter is powered off."));
            cap.mode = DeliveryMode::Blocked;
            return cap;
        }
        if (!cap.le_central) {
            cap.reasons.emplace_back(_("Adapter has no LE central role; ANCS is impossible."));
            cap.mode = DeliveryMode::Blocked;
            return cap;
        }

        // Past this point messages and contacts are reachable; only ANCS is at risk.
        cap.mode = DeliveryMode::Full;

        if (!cap.le_peripheral) {
            cap.reasons.emplace_back(_("Adapter cannot advertise as a peripheral, so ANCS cannot be solicited."));
            cap.mode = DeliveryMode::Compatibility;
        }
        if (!cap.advertising) {
            cap.reasons.emplace_back(_("No LE advertising instances available; the iPhone may never show its "
                                       "Bluetooth permission toggles."));
            cap.mode = DeliveryMode::Compatibility;
        }
        if (cap.bearer_api == BearerApi::Absent) {
            cap.setup.push_back({_("Notification mirroring needs BlueZ's experimental bearer API "
                                   "(org.bluez.Bearer.LE1), which bluetoothd is not exposing. Set this up before "
                                   "pairing: a bond made without it has no LE half."),
                                 enable_experimental_command(systemd_booted())});
            cap.mode = DeliveryMode::Compatibility;
        } else if (cap.bearer_api == BearerApi::Unknown && !cap.bonded_device_present) {
            // only an iphone bond can confirm the api
            cap.reasons.emplace_back(_("Bearer API support is unconfirmed until a device is bonded."));
        }

        if (cap.bonded_device_present && !cap.bond_has_le && !cap.bond_carries_ancs) {
            std::string reason = _("A device is bonded, but the bond covers BR/EDR only -- no LE keys were derived, "
                                   "so notification mirroring cannot work on it.");

            if (!cap.advertising || !cap.le_peripheral)
                reason += std::string(" ") +
                          _("This adapter cannot solicit ANCS, so notification mirroring is unavailable on it and "
                            "re-pairing will not change that.");
            else if (cap.secure_connections_known && !cap.secure_connections)
                reason += std::string(" ") +
                          _("Secure Connections is off on this controller, which is what blocks the derivation; "
                            "re-pairing will not help until it is on.");
            else
                reason +=
                    std::string(" ") + _("Delete this computer on the iPhone (Forget This Device) and pair again.");
            cap.reasons.emplace_back(std::move(reason));
        }
        if (!cap.class_ok) {
            cap.setup.push_back({_("The iPhone will not offer its Messages and Contacts permissions until this "
                                   "adapter presents itself as A/V Hands-Free (major 4, minor 8)."),
                                 set_class_command(cap.adapter_id, systemd_booted())});
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
            {"modalias", a.modalias},
            {"controller", a.controller},
            {"advertising_manager", a.has_advertising_manager},
            {"advertising_instances", a.advertising_instances},
            {"advertising_active_instances", a.advertising_active_instances},
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
            {"classic_connected", d.classic_link_up()},
            {"le_bearer", d.has_le_bearer},
            {"le_bonded", d.le_bonded},
            {"le_connected", d.le_connected},
            {"preferred_bearer", d.preferred_bearer},
            {"map", d.supports_map()},
            {"pbap", d.supports_pbap()},
            {"ancs", d.supports_ancs()},
            {"ancs_notifying", d.ancs_notifying},
            {"services_resolved", d.services_resolved},
            {"iphone", d.looks_like_iphone()},
            {"apple_nearby", d.apple_nearby},
            {"airpods", d.looks_like_airpods()},
        };
    }

    nlohmann::json to_json(const SetupStep& s) {
        return {
            {"what", s.what},
            {"command", s.command},
        };
    }

    nlohmann::json to_json(const Capability& c) {
        nlohmann::json setup = nlohmann::json::array();
        for (const auto& step : c.setup)
            setup.push_back(to_json(step));

        return {
            {"mode", to_string(c.mode)},
            {"bearer_api", to_string(c.bearer_api)},
            {"adapter_present", c.adapter_present},
            {"powered", c.powered},
            {"le_central", c.le_central},
            {"le_peripheral", c.le_peripheral},
            {"advertising", c.advertising},
            {"class_ok", c.class_ok},
            {"bonded_device_present", c.bonded_device_present},
            {"bond_has_le", c.bond_has_le},
            {"bond_carries_ancs", c.bond_carries_ancs},
            {"secure_connections",
             c.secure_connections_known ? nlohmann::json(c.secure_connections) : nlohmann::json(nullptr)},
            {"adapter_id", c.adapter_id},
            {"reasons", c.reasons},
            {"setup", setup},
        };
    }

} // namespace tether::bluetooth
