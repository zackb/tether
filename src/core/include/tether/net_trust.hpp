#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

struct ifaddrs;
struct sockaddr;

namespace tether {

    class EpollEventLoop;

    // A network this host is on right now
    struct LanNetwork {
        // NetworkManager connection UUID; without NetworkManager "iwd:<ssid hex>:<security>"
        // for Wi-Fi iwd has joined, else "if:<ifname>".
        std::string key;
        // The connection's name, or the interface name.
        std::string name;
        std::string ifname;
        int ifindex = 0;
        bool trusted = false;

        bool operator==(const LanNetwork&) const = default;
    };

    // Marks each network trusted: all of them, or those whose key is saved.
    void apply_lan_trust(std::vector<LanNetwork>& networks,
                         bool trust_all,
                         const std::map<std::string, std::string>& trusted);

    // The Wi-Fi network an interface is joined to, as iwd reports it.
    struct WifiNetwork {
        std::string ssid;
        // iwd's Network.Type: "open", "psk", "8021x" or "wep".
        std::string security;
    };

    // Keys interfaces joined to a known Wi-Fi network as "iwd:<ssid hex>:<security>", named
    // by the SSID. Others keep their interface key.
    void apply_wifi_networks(std::vector<LanNetwork>& networks, const std::map<std::string, WifiNetwork>& wifi);

    // Name of the interface in `head` holding this local address, or empty. An IPv4-mapped
    // IPv6 address matches the IPv4 one.
    std::string ifname_for_local_addr(const sockaddr* addr, const ifaddrs* head);

    // Watches for network changes on the loop and runs the first refresh.
    void lan_trust_start(EpollEventLoop& loop);

    // Re-reads the current networks and the config. Loop thread only.
    void lan_trust_refresh();

    // Runs on the loop thread after a refresh that changed anything.
    void set_lan_trust_changed_handler(std::function<void()> handler);

    // The interface holding the IPv4 default route, or empty: the network a phone can share.
    std::string default_route_ifname();

    // The rest are safe from any thread.
    std::vector<LanNetwork> lan_networks();
    bool lan_trust_all();
    bool lan_ifindex_trusted(int ifindex);
    bool lan_ifname_trusted(const std::string& ifname);
    // Whether a connection to this local address may proceed. Loopback always may.
    bool lan_local_addr_trusted(const sockaddr* addr);

} // namespace tether
