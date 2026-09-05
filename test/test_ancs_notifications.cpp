#include <algorithm>
#include <gtest/gtest.h>
#include <tether/bluetooth/ancs/notifications.hpp>
#include <tether/bluetooth/config.hpp>

using namespace tether::bluetooth;
using namespace tether::bluetooth::ancs;

namespace {

    Notification make(uint32_t uid, const std::string& app_id) {
        Notification n;
        n.uid = uid;
        n.app_id = app_id;
        n.app_name = derive_app_name(app_id);
        return n;
    }

} // namespace

TEST(AncsAppName, DerivesTheLastBundleComponent) {
    EXPECT_EQ(derive_app_name("com.burbn.instagram"), "Instagram");
    EXPECT_EQ(derive_app_name("com.apple.MobileSMS"), "MobileSMS");
    EXPECT_EQ(derive_app_name("Signal"), "Signal");
    EXPECT_EQ(derive_app_name("com.example."), "com.example.");
    EXPECT_EQ(derive_app_name(""), "");
}

TEST(AncsIcons, PrefersTheAppOverItsCategory) {
    Notification n = make(1, "com.burbn.instagram");
    n.category = CategoryId::Social;
    const auto icons = icon_candidates(n);
    ASSERT_FALSE(icons.empty());
    EXPECT_EQ(icons.front(), "instagram");
    EXPECT_NE(std::find(icons.begin(), icons.end(), "internet-chat"), icons.end())
        << "the category icon stands in for themes without brand logos";
}

TEST(AncsIcons, FallsBackToTheCategoryForAnUnknownApp) {
    Notification n = make(2, "com.example.unheardof");
    n.category = CategoryId::Email;
    EXPECT_EQ(icon_candidates(n).front(), "mail-unread");
}

TEST(AncsIcons, AlwaysEndsWithAnIconThatShipsWithTether) {
    for (uint8_t category = 0; category <= static_cast<uint8_t>(CategoryId::Entertainment); ++category) {
        Notification n = make(3, "");
        n.category = static_cast<CategoryId>(category);
        const auto icons = icon_candidates(n);
        ASSERT_FALSE(icons.empty()) << "category " << int(category);
        EXPECT_EQ(icons.back(), "tether") << "category " << int(category);
    }
}

TEST(AncsLaunchTarget, OffersAWayToReachTheLinuxApp) {
    const auto whatsapp = launch_target("net.whatsapp.WhatsApp");
    EXPECT_EQ(whatsapp.uri_scheme, "whatsapp");
    EXPECT_FALSE(whatsapp.desktop_ids.empty());

    const auto telegram = launch_target("ph.telegra.Telegraph");
    EXPECT_EQ(telegram.uri_scheme, launch_target("org.telegram.messenger").uri_scheme)
        << "both Telegram bundle ids reach the same client";

    EXPECT_FALSE(launch_target("org.whispersystems.signal").empty());
}

TEST(AncsLaunchTarget, IsEmptyWithoutALinuxCounterpart) {
    EXPECT_TRUE(launch_target("com.example.unheardof").empty());
    EXPECT_TRUE(launch_target("").empty());
    // Messages notifications reply through tether-gtk instead.
    EXPECT_TRUE(launch_target(APP_ID_MESSAGES).empty());
}

TEST(AncsRegistry, RenameAppTouchesOnlyThatApp) {
    NotificationRegistry registry;
    registry.store(make(1, "com.burbn.instagram"));
    registry.store(make(2, "com.burbn.instagram"));
    registry.store(make(3, "com.apple.MobileSMS"));

    registry.rename_app("com.burbn.instagram", "Instagram");

    EXPECT_EQ(registry.find(1)->app_name, "Instagram");
    EXPECT_EQ(registry.find(2)->app_name, "Instagram");
    EXPECT_EQ(registry.find(3)->app_name, "MobileSMS");
}

TEST(AncsRegistry, RenameAppIgnoresEmptyArguments) {
    NotificationRegistry registry;
    registry.store(make(1, "com.burbn.instagram"));

    registry.rename_app("com.burbn.instagram", "");
    registry.rename_app("", "Instagram");

    EXPECT_EQ(registry.find(1)->app_name, "Instagram");
}

TEST(AncsRegistry, RecentIsOrderedByDeliveryTime) {
    NotificationRegistry registry;
    // Stored in the order iOS replayed them, which bears no relation to when the
    // phone delivered them.
    for (auto [uid, received] : {std::pair<uint32_t, int64_t>{1, 300}, {2, 100}, {3, 200}}) {
        Notification n = make(uid, "com.example.app");
        n.received = received;
        registry.store(n);
    }

    std::vector<Notification> recent = registry.recent();
    ASSERT_EQ(recent.size(), 3u);
    EXPECT_EQ(recent[0].uid, 1u);
    EXPECT_EQ(recent[1].uid, 3u);
    EXPECT_EQ(recent[2].uid, 2u);

    EXPECT_EQ(registry.recent(2).size(), 2u) << "the limit applies after sorting";
    EXPECT_EQ(registry.recent(2)[0].uid, 1u);
}

// Content mirroring was off with no way to turn it on, so a stored false from
// before the toggle existed must not pin the new default.
TEST(BluetoothConfig, UnversionedFileTakesTheContentDefault) {
    const Config config = deserialize_config(R"({"ancs_content_enabled": false})");
    EXPECT_TRUE(config.ancs_content_enabled);
}

TEST(BluetoothConfig, VersionedFileKeepsTheStoredChoice) {
    const Config off = deserialize_config(R"({"config_version": 1, "ancs_content_enabled": false})");
    EXPECT_FALSE(off.ancs_content_enabled);

    const Config on = deserialize_config(R"({"config_version": 1, "ancs_content_enabled": true})");
    EXPECT_TRUE(on.ancs_content_enabled);
}

TEST(BluetoothConfig, SerializeStampsTheVersion) {
    Config config;
    config.ancs_content_enabled = false;
    EXPECT_FALSE(deserialize_config(serialize_config(config)).ancs_content_enabled);
}
