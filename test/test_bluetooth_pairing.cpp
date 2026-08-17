#include <gtest/gtest.h>
#include <tether/bluetooth/agent.hpp>
#include <tether/bluetooth/config.hpp>

using namespace tether::bluetooth;

// The agent authorizes services during pairing. Anything outside this set would
// grant the phone a profile Tether has no reason to consume.
TEST(AgentPolicy, AuthorizesOnlyMapAndPbap) {
    EXPECT_TRUE(is_authorized_service("0000112e-0000-1000-8000-00805f9b34fb"));  // PBAP client
    EXPECT_TRUE(is_authorized_service("0000112f-0000-1000-8000-00805f9b34fb"));  // PBAP server
    EXPECT_TRUE(is_authorized_service("00001132-0000-1000-8000-00805f9b34fb"));  // MAP server
    EXPECT_TRUE(is_authorized_service("00001133-0000-1000-8000-00805f9b34fb"));  // MAP notification
    EXPECT_TRUE(is_authorized_service("00001134-0000-1000-8000-00805f9b34fb"));  // MAP client
}

TEST(AgentPolicy, RefusesEverythingElse) {
    // Hands-free / headset: the phone offers these, Tether must not accept them.
    EXPECT_FALSE(is_authorized_service("0000111e-0000-1000-8000-00805f9b34fb"));
    EXPECT_FALSE(is_authorized_service("0000110a-0000-1000-8000-00805f9b34fb"));
    // ANCS is reached over GATT after bonding, never authorized as a profile.
    EXPECT_FALSE(is_authorized_service("7905f431-b5ce-4e99-a40f-4b1e122d00d0"));
    EXPECT_FALSE(is_authorized_service(""));
    EXPECT_FALSE(is_authorized_service("garbage"));
    // A prefix match would be a real hole; require the whole UUID.
    EXPECT_FALSE(is_authorized_service("00001132-0000-1000-8000-00805f9b34f"));
    EXPECT_FALSE(is_authorized_service("00001132-0000-1000-8000-00805f9b34fbb"));
}

TEST(AgentPolicy, ServiceMatchIsCaseInsensitive) {
    EXPECT_TRUE(is_authorized_service("0000112F-0000-1000-8000-00805F9B34FB"));
}

// iOS renders the comparison with leading zeros. Dropping them shows the user a
// code that does not match their phone, which reads as a failed pairing.
TEST(Passkey, FormatsSixDigitsWithLeadingZeros) {
    EXPECT_EQ(format_passkey(123456), "123456");
    EXPECT_EQ(format_passkey(1234), "001234");
    EXPECT_EQ(format_passkey(0), "000000");
    EXPECT_EQ(format_passkey(7), "000007");
    EXPECT_EQ(format_passkey(999999), "999999");
}

TEST(BluetoothConfig, RoundTrips) {
    Config config;
    config.device_address = "60:57:C8:30:6A:F7";
    config.auth_strategy = AuthStrategy::ExplicitPair;
    config.ancs_enabled = false;

    EXPECT_EQ(deserialize_config(serialize_config(config)), config);
}

TEST(BluetoothConfig, DefaultsToConnectFirstAndAncsEnabled) {
    Config config = deserialize_config("{}");
    EXPECT_EQ(config.auth_strategy, AuthStrategy::ConnectFirst);
    EXPECT_TRUE(config.ancs_enabled);
    EXPECT_TRUE(config.device_address.empty());
}

// Connect-first is the default for a reason: a Linux-initiated Pair() can yield
// a BR/EDR-only bond that can never carry ANCS. An unknown value must not
// silently select the workaround.
TEST(BluetoothConfig, UnknownStrategyFallsBackToConnectFirst) {
    EXPECT_EQ(auth_strategy_from_string("nonsense"), AuthStrategy::ConnectFirst);
    EXPECT_EQ(auth_strategy_from_string(""), AuthStrategy::ConnectFirst);
    EXPECT_EQ(auth_strategy_from_string("explicit-pair"), AuthStrategy::ExplicitPair);
}

TEST(BluetoothConfig, SurvivesCorruptFile) {
    Config config = deserialize_config("{not json at all");
    EXPECT_EQ(config, Config{});

    // A valid JSON document of the wrong shape must also yield defaults.
    EXPECT_EQ(deserialize_config("[1,2,3]"), Config{});
    EXPECT_EQ(deserialize_config("\"string\""), Config{});
}
