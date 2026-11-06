#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tether::bluetooth {

    class BluezMonitor;

    inline constexpr const char* UUID_TETHER_LAN_SERVICE = "ba7a69f7-bf0f-4c09-8c73-cf4ee9afb5b5";
    inline constexpr const char* UUID_TETHER_LAN_INFO = "ba7a69f7-bf0f-4c09-8c73-cf4ee9afb5b6";

    // ATT caps an attribute value at 512 bytes.
    inline constexpr size_t LAN_INFO_MAX_BYTES = 512;

    // This host's addresses as "ip/prefix": interfaces that are up, without loopback or
    // IPv6 link-local, IPv4 first.
    std::vector<std::string> lan_addresses();

    // The characteristic value: compact JSON, addresses dropped from the tail until it fits.
    std::string encode_lan_info(const std::string& name,
                                uint16_t port,
                                const std::string& fingerprint,
                                std::vector<std::string> addresses);

    struct LanInfoState;

    // GATT service that tells a bonded phone where tetherd listens, for networks that block
    // mDNS. Lives on the monitor's thread; re-registers whenever bluetoothd comes back.
    class LanInfoService {
    public:
        LanInfoService(BluezMonitor& monitor, std::string name, uint16_t port, std::string fingerprint);
        ~LanInfoService();

        LanInfoService(const LanInfoService&) = delete;
        LanInfoService& operator=(const LanInfoService&) = delete;

        // Moves the registration to this adapter; empty withdraws it. Cheap when unchanged.
        void set_adapter(const std::string& adapter_path);

    private:
        BluezMonitor& monitor_;
        std::string adapter_;
        std::unique_ptr<LanInfoState> state_;
    };

} // namespace tether::bluetooth
