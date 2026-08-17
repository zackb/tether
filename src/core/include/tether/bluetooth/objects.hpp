#pragma once

#include <cstdint>
#include <glib.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Plain-data view of BlueZ's object tree, decoded from org.freedesktop.DBus
// ObjectManager payloads. Deliberately free of any bus or threading concern so
// it can be tested against recorded payloads.
namespace tether::bluetooth {

    // Services the iPhone exposes that we care about
    inline constexpr const char* UUID_MAP_MAS = "00001132-0000-1000-8000-00805f9b34fb";
    inline constexpr const char* UUID_MAP_MNS = "00001133-0000-1000-8000-00805f9b34fb";
    inline constexpr const char* UUID_PBAP_PSE = "0000112f-0000-1000-8000-00805f9b34fb";
    inline constexpr const char* UUID_ANCS = "7905f431-b5ce-4e99-a40f-4b1e122d00d0";

    inline constexpr uint32_t COD_TARGET = 0x0408;
    inline constexpr uint32_t COD_MASK = 0x1fff;

    struct Adapter {
        std::string path;
        std::string address;
        std::string name;
        uint32_t device_class = 0;
        bool powered = false;
        std::vector<std::string> roles;
        // org.bluez.LEAdvertisingManager1 is a separate interface on the same object.
        bool has_advertising_manager = false;
        uint8_t advertising_instances = 0;

        bool has_role(const std::string& role) const;
        bool class_is_handsfree() const { return (device_class & COD_MASK) == COD_TARGET; }

        bool operator==(const Adapter&) const = default;
    };

    struct Device {
        std::string path;
        std::string adapter_path;
        std::string address;
        std::string name;
        bool paired = false;
        bool bonded = false;
        bool trusted = false;
        bool connected = false;
        std::vector<std::string> uuids;

        bool has_le_bearer = false;
        bool le_paired = false;
        bool le_bonded = false;
        bool le_connected = false;

        bool has_uuid(const std::string& uuid) const;
        bool supports_map() const { return has_uuid(UUID_MAP_MAS); }
        bool supports_pbap() const { return has_uuid(UUID_PBAP_PSE); }
        bool supports_ancs() const { return has_uuid(UUID_ANCS); }
        bool looks_like_iphone() const;

        bool operator==(const Device&) const = default;
    };

    struct BluezObjects {
        std::vector<Adapter> adapters;
        std::vector<Device> devices;

        // Whether bluetoothd was started with --experimental. Bearer.LE1 exists only under that flag
        bool experimental_api = false;

        const Adapter* find_adapter(const std::string& path) const;
        const Device* find_device(const std::string& path) const;

        bool operator==(const BluezObjects&) const = default;
    };

    // Whether the machine can carry ANCS, or only MAP and PBAP.
    enum class DeliveryMode { Blocked, Compatibility, Full };

    // Bearer.LE1 cannot be observed until something is bonded, so its absence is
    // only meaningful once a bond exists.
    enum class BearerApi { Unknown, Confirmed, Absent };

    struct Capability {
        DeliveryMode mode = DeliveryMode::Blocked;
        BearerApi bearer_api = BearerApi::Unknown;
        bool adapter_present = false;
        bool powered = false;
        bool le_central = false;
        bool le_peripheral = false;
        bool advertising = false;
        bool class_ok = false;
        // Human-readable explanations for anything short of full mode, shown verbatim
        std::vector<std::string> reasons;

        bool operator==(const Capability&) const = default;
    };

    const char* to_string(DeliveryMode mode);
    const char* to_string(BearerApi api);

    // Decodes an ObjectManager.GetManagedObjects reply, a{oa{sa{sv}}}. Accepts
    // either the raw reply tuple or the unwrapped dictionary. Returns an empty
    // result for null or unexpected input rather than throwing.
    BluezObjects parse_managed_objects(GVariant* reply);

    Capability resolve_capability(const BluezObjects& objects);

    nlohmann::json to_json(const Adapter& adapter);
    nlohmann::json to_json(const Device& device);
    nlohmann::json to_json(const Capability& capability);

} // namespace tether::bluetooth
