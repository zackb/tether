#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tether {

    /// Represents a tetherd instance discovered via mDNS/DNS-SD on the local network.
    struct DiscoveredHost {
        std::string name;         ///< Advertised service name (e.g. hostname)
        std::string address;      ///< Resolved IPv4/IPv6 address
        uint16_t port = 0;        ///< TCP port the daemon is listening on
        std::string fingerprint;  ///< TLS certificate SHA-256 fingerprint from TXT record
    };

    /// mDNS service discovery using Avahi.
    ///
    /// The daemon calls publish() to advertise itself as `_tether._tcp`.
    /// Clients call discover() to find daemon instances on the LAN.
    class Discovery {
    public:
        Discovery();
        ~Discovery();

        // Non-copyable, movable
        Discovery(const Discovery&) = delete;
        Discovery& operator=(const Discovery&) = delete;
        Discovery(Discovery&&) noexcept;
        Discovery& operator=(Discovery&&) noexcept;

        /// Publish this tetherd instance on the local network.
        /// @param name     Human-readable service name (typically the hostname).
        /// @param port     TCP port the daemon is listening on.
        /// @param fingerprint  SHA-256 fingerprint of the daemon's TLS certificate.
        /// @return true if the service was successfully registered.
        bool publish(const std::string& name, uint16_t port,
                     const std::string& fingerprint);

        /// Stop advertising this instance.  Called automatically by the destructor.
        void unpublish();

        /// Scan the local network for tetherd instances.
        /// Blocks for up to @p timeout_ms milliseconds and returns all hosts found.
        /// @param timeout_ms  How long to listen for mDNS responses (default 3 s).
        /// @return A list of discovered hosts, possibly empty.
        std::vector<DiscoveredHost> discover(int timeout_ms = 3000);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace tether
