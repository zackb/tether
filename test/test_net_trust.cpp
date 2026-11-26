#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <tether/discovery.hpp>
#include <tether/net_trust.hpp>

using tether::LanNetwork;

namespace {

    sockaddr_in v4(const char* text) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        inet_pton(AF_INET, text, &a.sin_addr);
        return a;
    }

    sockaddr_in6 v6(const char* text) {
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        inet_pton(AF_INET6, text, &a.sin6_addr);
        return a;
    }

} // namespace

TEST(NetTrust, TrustsSavedKeysOnly) {
    std::vector<LanNetwork> nets = {{"home", "Home WiFi", "wlan0", 3}, {"cafe", "Cafe", "wlan0", 3}};
    tether::apply_lan_trust(nets, false, {{"home", "Home WiFi"}});
    EXPECT_TRUE(nets[0].trusted);
    EXPECT_FALSE(nets[1].trusted);
}

TEST(NetTrust, AllScopeTrustsEverything) {
    std::vector<LanNetwork> nets = {{"cafe", "Cafe", "wlan0", 3}};
    tether::apply_lan_trust(nets, true, {});
    EXPECT_TRUE(nets[0].trusted);
}

TEST(NetTrust, FindsTheInterfaceForALocalAddress) {
    auto wlan = v4("192.168.1.20");
    auto eth = v6("fd00::5");
    ifaddrs second{};
    second.ifa_name = const_cast<char*>("enp3s0");
    second.ifa_addr = reinterpret_cast<sockaddr*>(&eth);
    ifaddrs first{};
    first.ifa_next = &second;
    first.ifa_name = const_cast<char*>("wlan0");
    first.ifa_addr = reinterpret_cast<sockaddr*>(&wlan);

    auto local4 = v4("192.168.1.20");
    EXPECT_EQ(tether::ifname_for_local_addr(reinterpret_cast<sockaddr*>(&local4), &first), "wlan0");

    // A dual-stack listener reports IPv4 peers as ::ffff:a.b.c.d.
    auto mapped = v6("::ffff:192.168.1.20");
    EXPECT_EQ(tether::ifname_for_local_addr(reinterpret_cast<sockaddr*>(&mapped), &first), "wlan0");

    auto local6 = v6("fd00::5");
    EXPECT_EQ(tether::ifname_for_local_addr(reinterpret_cast<sockaddr*>(&local6), &first), "enp3s0");

    auto other = v4("10.0.0.1");
    EXPECT_EQ(tether::ifname_for_local_addr(reinterpret_cast<sockaddr*>(&other), &first), "");
}

TEST(NetTrust, LoopbackIsAlwaysTrusted) {
    auto lo4 = v4("127.0.0.1");
    auto lo6 = v6("::1");
    auto mapped = v6("::ffff:127.0.0.1");
    EXPECT_TRUE(tether::lan_local_addr_trusted(reinterpret_cast<sockaddr*>(&lo4)));
    EXPECT_TRUE(tether::lan_local_addr_trusted(reinterpret_cast<sockaddr*>(&lo6)));
    EXPECT_TRUE(tether::lan_local_addr_trusted(reinterpret_cast<sockaddr*>(&mapped)));
}

// The daemon dials only addresses seen on a trusted interface, so grouping must keep it.
TEST(NetTrust, GroupingKeepsTheInterface) {
    const auto devices =
        tether::group_discovered_hosts({{"phone", "192.168.1.9", 5134, "fp", 3}, {"phone", "fd00::9", 5134, "fp", 4}});
    ASSERT_EQ(devices.size(), 1u);
    ASSERT_EQ(devices[0].addresses.size(), 2u);
    EXPECT_EQ(devices[0].addresses[0].ifindex, 3);
    EXPECT_EQ(devices[0].addresses[1].ifindex, 4);
}

// Without NetworkManager, iwd tells one Wi-Fi network from another on the same card.
TEST(NetTrust, IwdKeysWifiBySsidAndSecurity) {
    std::vector<LanNetwork> nets = {{"if:wlan0", "wlan0", "wlan0", 3}, {"if:enp3s0", "enp3s0", "enp3s0", 2}};
    tether::apply_wifi_networks(nets, {{"wlan0", {"Home", "psk"}}});
    EXPECT_EQ(nets[0].key, "iwd:486f6d65:psk");
    EXPECT_EQ(nets[0].name, "Home");
    EXPECT_EQ(nets[1].key, "if:enp3s0");

    // The same SSID without its password is another network.
    std::vector<LanNetwork> twin = {{"if:wlan0", "wlan0", "wlan0", 3}};
    tether::apply_wifi_networks(twin, {{"wlan0", {"Home", "open"}}});
    EXPECT_NE(twin[0].key, nets[0].key);
}
