#include "tether/bluetooth/diagnostics.hpp"

#include <cstdlib>
#include <gtest/gtest.h>

using namespace tether::bluetooth;

TEST(BtDiagnostics, RedactsBareAddress) {
    Redactor r;
    EXPECT_EQ(r.text("connecting to 60:57:C8:30:6A:F7"), "connecting to <address-1>");
}

TEST(BtDiagnostics, RedactsDeviceNodeInObjectPath) {
    Redactor r;
    EXPECT_EQ(r.text("/org/bluez/hci0/dev_60_57_C8_30_6A_F7"), "/org/bluez/hci0/dev_<address-1>");
}

TEST(BtDiagnostics, SameDeviceGetsSamePlaceholderAcrossSpellings) {
    Redactor r;
    const std::string out = r.text("dev_60_57_C8_30_6A_F7 is 60:57:c8:30:6a:f7");
    EXPECT_EQ(out, "dev_<address-1> is <address-1>");
}

TEST(BtDiagnostics, DistinctDevicesGetDistinctPlaceholders) {
    Redactor r;
    const std::string out = r.text("AA:BB:CC:DD:EE:FF and 11:22:33:44:55:66");
    EXPECT_EQ(out, "<address-1> and <address-2>");
}

TEST(BtDiagnostics, RedactsEmailAndPhoneNumber) {
    Redactor r;
    EXPECT_EQ(r.text("sent to tel:+15035550101"), "sent to tel:<number-1>");
    EXPECT_EQ(r.text("sent to zack@example.com"), "sent to <email-1>");
}

TEST(BtDiagnostics, LeavesTimestampsAndErrorCodesAlone) {
    Redactor r;
    const std::string out = r.text("2026-08-16T10:11:12 failed with 0x43 after 30000 ms");
    EXPECT_EQ(out, "2026-08-16T10:11:12 failed with 0x43 after 30000 ms");
}

TEST(BtDiagnostics, RedactsHomeAndRuntimeDirectories) {
    setenv("HOME", "/home/someone", 1);
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    Redactor r;
    EXPECT_EQ(r.text("staged /run/user/1000/tether/out.bmsg"), "staged <runtime>/tether/out.bmsg");
    EXPECT_EQ(r.text("wrote /home/someone/.config/tether/bluetooth.json"),
              "wrote <home>/.config/tether/bluetooth.json");
}

TEST(BtDiagnostics, RedactsRuntimeDirectoryWithoutTheEnvironmentVariable) {
    unsetenv("XDG_RUNTIME_DIR");
    Redactor r;
    EXPECT_EQ(r.text("/run/user/4242/tether/tetherd.sock"), "<runtime>/tether/tetherd.sock");
}

TEST(BtDiagnostics, DropsContentKeysRatherThanRedactingThem) {
    Redactor r;
    const nlohmann::json in = {
        {"command", "bt_message"},
        {"body", "meet me at the usual place"},
        {"sender", "Jane"},
        {"name", "Jane's iPhone"},
        {"address", "60:57:C8:30:6A:F7"},
    };

    const nlohmann::json out = r.value(in);
    EXPECT_FALSE(out.contains("body"));
    EXPECT_FALSE(out.contains("sender"));
    EXPECT_FALSE(out.contains("name"));
    EXPECT_EQ(out["address"], "<address-1>");
}

TEST(BtDiagnostics, RedactsNestedStructures) {
    Redactor r;
    const nlohmann::json in = {{"adapters", {{{"path", "/org/bluez/hci0"}, {"address", "AA:BB:CC:DD:EE:FF"}}}}};
    const nlohmann::json out = r.value(in);
    EXPECT_EQ(out["adapters"][0]["address"], "<address-1>");
    EXPECT_EQ(out["adapters"][0]["path"], "/org/bluez/hci0");
}

TEST(BtDiagnostics, TimelineKeepsOnlyStateEvents) {
    clear_diagnostic_timeline();
    record_diagnostic_event({{"command", "bt_pair_progress"}, {"step", "connect"}});
    record_diagnostic_event({{"command", "bt_message"}, {"body", "hello"}});
    record_diagnostic_event({{"command", "clipboard_updated"}, {"content", "hello"}});

    const auto timeline = diagnostic_timeline();
    ASSERT_EQ(timeline.size(), 1u);
    EXPECT_EQ(timeline[0]["command"], "bt_pair_progress");
}

TEST(BtDiagnostics, TimelineStripsContentAtRecordTime) {
    clear_diagnostic_timeline();
    record_diagnostic_event({{"command", "bt_send_result"}, {"success", true}, {"body", "secret"}});

    const auto timeline = diagnostic_timeline();
    ASSERT_EQ(timeline.size(), 1u);
    EXPECT_FALSE(timeline[0].contains("body"));
    EXPECT_TRUE(timeline[0]["success"]);
}

TEST(BtDiagnostics, TimelineIsOrderedAndMonotonic) {
    clear_diagnostic_timeline();
    record_diagnostic_event({{"command", "bt_pair_progress"}, {"step", "first"}});
    record_diagnostic_event({{"command", "bt_pair_progress"}, {"step", "second"}});

    const auto timeline = diagnostic_timeline();
    ASSERT_EQ(timeline.size(), 2u);
    EXPECT_EQ(timeline[0]["step"], "first");
    EXPECT_EQ(timeline[1]["step"], "second");
    EXPECT_LE(timeline[0]["at_ms"].get<int64_t>(), timeline[1]["at_ms"].get<int64_t>());
}

TEST(BtDiagnostics, TimelineIsBounded) {
    clear_diagnostic_timeline();
    for (int i = 0; i < 500; ++i)
        record_diagnostic_event({{"command", "bt_pair_progress"}, {"step", std::to_string(i)}});

    const auto timeline = diagnostic_timeline();
    EXPECT_EQ(timeline.size(), 200u);
    // The oldest entries are the ones dropped.
    EXPECT_EQ(timeline.back()["step"], "499");
}

TEST(BtDiagnostics, ReportCarriesNoAddressOrContent) {
    clear_diagnostic_timeline();
    record_diagnostic_event({{"command", "bt_connection_changed"},
                             {"link_reason", "Connecting to 60:57:C8:30:6A:F7"},
                             {"map_open", false}});

    const nlohmann::json status = {{"adapters", {{{"address", "AA:BB:CC:DD:EE:FF"}, {"name", "someone-laptop"}}}}};
    const nlohmann::json connection = {{"link_reason", "no route to 60:57:C8:30:6A:F7"}};

    const std::string dumped = build_diagnostics(status, connection).dump();
    EXPECT_EQ(dumped.find("60:57:C8:30:6A:F7"), std::string::npos);
    EXPECT_EQ(dumped.find("AA:BB:CC:DD:EE:FF"), std::string::npos);
    EXPECT_EQ(dumped.find("someone-laptop"), std::string::npos);
}
