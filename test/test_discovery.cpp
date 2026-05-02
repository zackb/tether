#include <gtest/gtest.h>
#include <tether/discovery.hpp>
#include <vector>
#include <string>

TEST(DiscoveryTest, GroupDiscoveredHosts_EmptyInput) {
    std::vector<tether::DiscoveredHost> hosts;
    auto result = tether::group_discovered_hosts(hosts);
    EXPECT_TRUE(result.empty());
}

TEST(DiscoveryTest, GroupDiscoveredHosts_SingleHost) {
    std::vector<tether::DiscoveredHost> hosts = {
        {"device.local", "192.168.1.10", 5134, "abc123"}
    };
    auto result = tether::group_discovered_hosts(hosts);
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].name, "device.local");
    EXPECT_EQ(result[0].fingerprint, "abc123");
    ASSERT_EQ(result[0].addresses.size(), 1);
    EXPECT_EQ(result[0].addresses[0].address, "192.168.1.10");
    EXPECT_EQ(result[0].addresses[0].port, 5134);
}

TEST(DiscoveryTest, GroupDiscoveredHosts_MultipleAddressesSameFingerprint) {
    std::vector<tether::DiscoveredHost> hosts = {
        {"device.local", "192.168.1.10", 5134, "abc123"},
        {"device.local", "192.168.1.11", 5134, "abc123"},
        {"device.local", "::1", 5134, "abc123"}
    };
    auto result = tether::group_discovered_hosts(hosts);
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].fingerprint, "abc123");
    ASSERT_EQ(result[0].addresses.size(), 3);
    EXPECT_EQ(result[0].addresses[0].address, "192.168.1.10");
    EXPECT_EQ(result[0].addresses[1].address, "192.168.1.11");
    EXPECT_EQ(result[0].addresses[2].address, "::1");
}

TEST(DiscoveryTest, GroupDiscoveredHosts_MultipleDevices) {
    std::vector<tether::DiscoveredHost> hosts = {
        {"device1.local", "192.168.1.10", 5134, "fp1"},
        {"device2.local", "192.168.1.20", 5134, "fp2"}
    };
    auto result = tether::group_discovered_hosts(hosts);
    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result[0].fingerprint, "fp1");
    EXPECT_EQ(result[1].fingerprint, "fp2");
}

TEST(DiscoveryTest, GroupDiscoveredHosts_EmptyFingerprintGroupsByName) {
    std::vector<tether::DiscoveredHost> hosts = {
        {"device.local", "192.168.1.10", 5134, ""},
        {"device.local", "192.168.1.11", 5134, ""}
    };
    auto result = tether::group_discovered_hosts(hosts);
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].name, "device.local");
    EXPECT_EQ(result[0].fingerprint, "");
    ASSERT_EQ(result[0].addresses.size(), 2);
}

TEST(DiscoveryTest, GroupDiscoveredHosts_IPv4Sorting) {
    std::vector<tether::DiscoveredHost> hosts = {
        {"device.local", "::1", 5134, "fp1"},
        {"device.local", "192.168.1.10", 5134, "fp1"},
        {"device.local", "192.168.1.5", 5134, "fp1"}
    };
    auto result = tether::group_discovered_hosts(hosts);
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].addresses[0].address, "192.168.1.10");
    EXPECT_EQ(result[0].addresses[1].address, "192.168.1.5");
    EXPECT_EQ(result[0].addresses[2].address, "::1");
}

TEST(DiscoveryTest, GroupDiscoveredHosts_MixedFingerprintsAndNames) {
    std::vector<tether::DiscoveredHost> hosts = {
        {"device1", "192.168.1.10", 5134, "fp1"},
        {"device2", "192.168.1.20", 5134, ""},
        {"device1", "192.168.1.11", 5134, "fp1"},
        {"device2", "192.168.1.21", 5134, ""}
    };
    auto result = tether::group_discovered_hosts(hosts);
    ASSERT_EQ(result.size(), 2);
}