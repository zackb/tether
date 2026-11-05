#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <tether/bluetooth/lan_info.hpp>
#include <vector>

using tether::bluetooth::encode_lan_info;
using tether::bluetooth::LAN_INFO_MAX_BYTES;

TEST(LanInfoTest, EncodesContract) {
    const auto doc = nlohmann::json::parse(encode_lan_info("btw", 5134, "ab12", {"192.168.1.28/24", "fd12::5/64"}));
    EXPECT_EQ(doc["v"], 1);
    EXPECT_EQ(doc["name"], "btw");
    EXPECT_EQ(doc["port"], 5134);
    EXPECT_EQ(doc["fp"], "ab12");
    EXPECT_EQ(doc["addrs"], nlohmann::json::array({"192.168.1.28/24", "fd12::5/64"}));
}

TEST(LanInfoTest, DropsTailAddressesToFit) {
    std::vector<std::string> addrs;
    for (int i = 0; i < 60; ++i)
        addrs.push_back("10.0.0." + std::to_string(i) + "/24");
    const std::string out = encode_lan_info("host", 5134, std::string(64, 'f'), addrs);
    EXPECT_LE(out.size(), LAN_INFO_MAX_BYTES);

    const auto kept = nlohmann::json::parse(out)["addrs"];
    ASSERT_FALSE(kept.empty());
    ASSERT_LT(kept.size(), addrs.size());
    EXPECT_EQ(kept[0], addrs[0]);
    // Only whole entries are dropped, and only from the tail.
    for (size_t i = 0; i < kept.size(); ++i)
        EXPECT_EQ(kept[i], addrs[i]);
}

TEST(LanInfoTest, HostAddressesSkipLoopbackAndLinkLocal) {
    bool seen_v6 = false;
    for (const auto& addr : tether::bluetooth::lan_addresses()) {
        ASSERT_NE(addr.find('/'), std::string::npos) << addr;
        EXPECT_NE(addr.rfind("127.", 0), 0u) << addr;
        EXPECT_NE(addr.rfind("::1/", 0), 0u) << addr;
        EXPECT_NE(addr.rfind("fe80:", 0), 0u) << addr;
        const bool v6 = addr.find(':') != std::string::npos;
        EXPECT_FALSE(seen_v6 && !v6) << "IPv4 after IPv6: " << addr;
        seen_v6 = seen_v6 || v6;
    }
}
