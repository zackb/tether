#pragma once

#include <cstdint>
#include <glib.h>
#include <nlohmann/json.hpp>
#include <optional>
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
    inline constexpr const char* UUID_A2DP_SINK = "0000110b-0000-1000-8000-00805f9b34fb";

    // Modalias prefix for Apple's Bluetooth vendor id, shared by every Apple device.
    inline constexpr const char* MODALIAS_APPLE = "bluetooth:v004c";

    inline constexpr uint32_t COD_TARGET = 0x0408;
    inline constexpr uint32_t COD_MASK = 0x1fff;

    struct Adapter {
        std::string path;
        std::string address;
        std::string name;
        uint32_t device_class = 0;
        bool powered = false;
        std::vector<std::string> roles;
        // BlueZ's own device id, not the chip's.
        std::string modalias;
        // The controller itself, from /sys/class/bluetooth/<hciN>/device/modalias.
        std::string controller;
        // org.bluez.LEAdvertisingManager1 is a separate interface on the same object.
        bool has_advertising_manager = false;
        uint8_t advertising_instances = 0;
        // Instances BlueZ currently holds registered.
        uint8_t advertising_active_instances = 0;
        // org.bluez.AdvertisementMonitorManager1
        bool has_adv_monitor_manager = false;

        bool has_role(const std::string& role) const;
        bool class_is_handsfree() const { return (device_class & COD_MASK) == COD_TARGET; }
        bool presents_as_apple() const;

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
        // Device1.Modalias, e.g. "bluetooth:v004Cp200Ed0100". Names the remote's vendor.
        std::string modalias;
        // Apple Nearby Info (company 0x004c, advertisement type 0x10) is
        // available before an unpaired iPhone exposes its service UUIDs.
        bool apple_nearby = false;

        bool ancs_notifying = false;

        // GAP Device Name characteristic
        std::string gap_name_path;

        // Device1.ServicesResolved
        bool services_resolved = false;

        // Device1.PreferredBearer: "le", "bredr" or "last-seen".
        std::string preferred_bearer;

        bool has_le_bearer = false;
        bool le_paired = false;
        bool le_bonded = false;
        bool le_connected = false;

        bool has_classic_bearer = false;
        bool classic_paired = false;
        bool classic_bonded = false;
        bool classic_connected = false;
        bool classic_state_known = false;

        bool classic_link_up() const { return classic_state_known ? classic_connected : connected; }
        bool le_link_up() const { return le_connected || (ancs_notifying && services_resolved); }

        bool has_uuid(const std::string& uuid) const;
        bool supports_map() const { return has_uuid(UUID_MAP_MAS); }
        bool supports_pbap() const { return has_uuid(UUID_PBAP_PSE); }
        bool supports_ancs() const { return has_uuid(UUID_ANCS); }
        bool looks_like_iphone() const;
        // Apple audio gear rather than a phone: AAP battery is worth trying on it.
        bool looks_like_airpods() const;

        bool operator==(const Device&) const = default;
    };

    // One call on an audio gateway, from org.bluez.Call1.
    struct Call {
        std::string path;
        // Owning telephony object, so a second phone's calls stay separate.
        std::string telephony_path;
        // CLIP for incoming calls, the dialled number for outgoing. BlueZ passes
        // the literal "withheld" through when the network refuses caller ID.
        std::string number;
        std::string name;
        std::string incoming_line;
        // BlueZ's own state string, unmapped so an unseen state still reaches
        // the UI: active, held, dialing, alerting, incoming, waiting,
        // response_and_hold, disconnected.
        std::string state;
        bool multiparty = false;

        bool withheld() const { return number == "withheld"; }
        bool ringing() const { return state == "incoming" || state == "waiting"; }
        bool outgoing() const { return state == "dialing" || state == "alerting"; }
        bool connected() const { return state == "active" || state == "held"; }

        bool operator==(const Call&) const = default;
    };

    // An HFP audio gateway, from org.bluez.Telephony1. BlueZ exports one per
    // connected phone at <device>/telephonyN, plus the network state the phone reports over HFP.
    struct Telephony {
        std::string path;
        std::string device_path;
        std::string uuid;
        // connecting, slc_connecting, connected, disconnecting.
        std::string state;
        std::string operator_name;
        std::vector<std::string> uri_schemes;
        // 0-5, as HFP reports them.
        uint8_t signal = 0;
        uint8_t battery = 0;
        bool service = false;
        bool roaming = false;
        bool inband_ringtone = false;

        bool ready() const { return state == "connected"; }

        bool operator==(const Telephony&) const = default;
    };

    struct BluezObjects {
        std::vector<Adapter> adapters;
        std::vector<Device> devices;
        std::vector<Telephony> telephony;
        std::vector<Call> calls;

        // bluetoothd has experimental API on
        bool experimental_api = false;

        const Adapter* find_adapter(const std::string& path) const;
        const Device* find_device(const std::string& path) const;

        // The gateway for a device path, or null.
        const Telephony* find_telephony(const std::string& device_path) const;
        // Calls belonging to one gateway, in path order.
        std::vector<Call> calls_for(const std::string& telephony_path) const;

        bool operator==(const BluezObjects&) const = default;
    };

    // Whether the machine can carry ANCS, or only MAP and PBAP.
    enum class DeliveryMode { Blocked, Compatibility, Full };

    // Bearer.LE1 cannot be observed until something is bonded, so its absence is
    // only meaningful once a bond exists.
    enum class BearerApi { Unknown, Confirmed, Absent };

    struct SetupStep {
        std::string what;
        std::string command;

        bool operator==(const SetupStep&) const = default;
    };

    struct Capability {
        DeliveryMode mode = DeliveryMode::Blocked;
        BearerApi bearer_api = BearerApi::Unknown;
        bool adapter_present = false;
        bool powered = false;
        bool le_central = false;
        bool le_peripheral = false;
        bool advertising = false;
        bool class_ok = false;
        bool bonded_device_present = false;
        bool bond_has_le = false;
        bool bond_carries_ancs = false;
        bool secure_connections = false;
        bool secure_connections_known = false;
        // Adapter the steps below apply to, e.g. "hci0". The class unit is templated on it.
        std::string adapter_id;
        // Human-readable explanations for anything short of full mode, shown verbatim
        std::vector<std::string> reasons;
        // Remediable gaps only. Anything the user cannot act on stays in reasons.
        std::vector<SetupStep> setup;

        bool operator==(const Capability&) const = default;
    };

    const char* to_string(DeliveryMode mode);
    const char* to_string(BearerApi api);

    // Decodes an ObjectManager.GetManagedObjects reply, a{oa{sa{sv}}}. Accepts
    // either the raw reply tuple or the unwrapped dictionary. Returns an empty
    // result for null or unexpected input rather than throwing.
    BluezObjects parse_managed_objects(GVariant* reply);

    // Resolves the pure ObjectManager snapshot plus the optional controller
    // setting read by BluezMonitor. Keeping the external btmgmt probe out of
    // this function makes capability resolution deterministic and prevents a
    // stalled controller query from blocking unrelated callers.
    // The adapter tether uses: by `id` ("hciN" or an address),
    // Null only when there is no adapter at all.
    const Adapter* preferred_adapter(const BluezObjects& objects, const std::string& id = {});

    Capability resolve_capability(const BluezObjects& objects,
                                  std::optional<bool> secure_connections = std::nullopt,
                                  const std::string& preferred_id = {});

    // Shell commands for the two setup steps, for a machine booted with or without systemd.
    std::string enable_experimental_command(bool systemd);
    std::string set_class_command(const std::string& adapter_id, bool systemd);

    nlohmann::json to_json(const Adapter& adapter);
    nlohmann::json to_json(const Device& device);
    nlohmann::json to_json(const SetupStep& step);
    nlohmann::json to_json(const Capability& capability);

} // namespace tether::bluetooth
