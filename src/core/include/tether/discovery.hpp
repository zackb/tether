#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tether {

    // A single network address where a tetherd instance is reachable.
    struct DiscoveredAddress {
        std::string address; //< IPv4 or IPv6 address
        uint16_t port = 0;   //< TCP port
        int ifindex = 0;     //< Interface it was seen on; 0 when unknown
    };

    // Raw per-interface mDNS result (one entry per resolved address).
    struct DiscoveredHost {
        std::string name;        //< Advertised service name (e.g. hostname)
        std::string address;     //< Resolved IPv4/IPv6 address
        uint16_t port = 0;       //< TCP port the daemon is listening on
        std::string fingerprint; //< TLS certificate SHA-256 fingerprint from TXT record
        int ifindex = 0;         //< Interface the record arrived on
    };

    // A logical device, grouping all network interfaces that share the same
    // TLS fingerprint (i.e. the same tetherd instance).
    struct DiscoveredDevice {
        std::string name;                         //< Advertised service name
        std::string fingerprint;                  //< Shared TLS fingerprint
        std::vector<DiscoveredAddress> addresses; //< All reachable addresses
    };

    // Group raw per-interface results into logical devices by fingerprint.
    std::vector<DiscoveredDevice> group_discovered_hosts(const std::vector<DiscoveredHost>& hosts,
                                                         const std::string& exclude_fingerprint = "");

    // mDNS service discovery using Avahi.
    //
    // The daemon calls publish() to advertise itself as `_tether._tcp`.
    // Clients call discover() to find daemon instances on the LAN.
    class Discovery {
    public:
        Discovery();
        ~Discovery();

        // Non-copyable, movable
        Discovery(const Discovery&) = delete;
        Discovery& operator=(const Discovery&) = delete;
        Discovery(Discovery&&) noexcept;
        Discovery& operator=(Discovery&&) noexcept;

        // Reported when mDNS availability changes: false while avahi-daemon is
        // down, true once a client is connected to it.
        using StateCallback = std::function<void(bool available)>;
        void set_state_callback(StateCallback cb);

        // Publish this tetherd instance on the local network.
        // @param name     Human-readable service name (typically the hostname).
        // @param port     TCP port the daemon is listening on.
        // @param fingerprint  SHA-256 fingerprint of the daemon's TLS certificate.
        // @return true if publishing was set up. Registration itself is
        //         asynchronous and waits for avahi-daemon, which need not be
        //         running yet; watch set_state_callback() for the outcome.
        bool publish(const std::string& name, uint16_t port, const std::string& fingerprint);

        // Re-register on the interfaces lan_ifindex_trusted() allows. Any thread.
        void republish();

        // Stop advertising this instance.  Called automatically by the destructor.
        void unpublish();

        // Scan the local network for tetherd instances.
        // Blocks for up to @p timeout_ms milliseconds and returns all hosts found.
        // @param timeout_ms  How long to listen for mDNS responses (default 3 s).
        // @return A list of discovered hosts (one per interface), possibly empty.
        std::vector<DiscoveredHost> discover(int timeout_ms = 3000);

        // Start continuous background discovery.
        // The callback is invoked whenever a new device is found or an existing device is removed.
        // The callback is executed on the background Avahi thread.
        // Only one continuous browse session can run per Discovery instance.
        void start_continuous_browse(std::function<void(const std::vector<DiscoveredDevice>&)> callback);

        // Re-run the browse callback against the current cache.
        void refresh();

        // Stop continuous background discovery.
        void stop_continuous_browse();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace tether
