#include <algorithm>
#include <filesystem>
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

TEST(AncsIcons, PrefersTheThemeOverBundledArt) {
    const std::vector<std::pair<std::string, std::vector<std::string>>> expected{
        {"com.apple.MobileSMS", {"internet-chat"}},
        {"com.microsoft.Office.Outlook", {"ms-outlook"}},
        {"com.hammerandchisel.discord", {"discord", "tether-app-discord"}},
        {"com.atebits.Tweetie2", {"twitter", "tether-app-x"}},
        {"org.whispersystems.signal", {"signal-desktop", "tether-app-signal"}},
        {"com.instructure.icanvas", {"tether-app-canvas"}},
    };

    for (const auto& [app_id, icons] : expected) {
        const auto candidates = icon_candidates(make(1, app_id));
        ASSERT_GE(candidates.size(), icons.size()) << app_id;
        EXPECT_TRUE(std::equal(icons.begin(), icons.end(), candidates.begin())) << app_id;
    }
}

TEST(AncsIcons, EveryBundledNameHasAnIconFile) {
    const char* app_ids[] = {
        "com.atebits.Tweetie2",
        "com.burbn.instagram",
        "com.facebook.Facebook",
        "com.github.github",
        "com.github.stormbreaker.prod",
        "com.google.Gmail",
        "com.google.ios.youtube",
        "com.hammerandchisel.discord",
        "com.instructure.icanvas",
        "com.reddit.Reddit",
        "com.spotify.client",
        "com.toyopagroup.picaboo",
        "com.zhiliaoapp.musically",
        "net.whatsapp.WhatsApp",
        "org.telegram.messenger",
        "org.whispersystems.signal",
    };
    const std::filesystem::path dir = TETHER_NOTIFICATION_ICONS_DIR;

    for (const char* app_id : app_ids) {
        const auto candidates = icon_candidates(make(1, app_id));
        const auto bundled = std::find_if(
            candidates.begin(), candidates.end(), [](const std::string& n) { return n.rfind("tether-app-", 0) == 0; });
        ASSERT_NE(bundled, candidates.end()) << app_id;
        EXPECT_TRUE(std::filesystem::exists(dir / (*bundled + ".svg"))) << *bundled;
    }
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

// Measured on a live phone: ANCS UIDs are per-connection counters that restart
// low on every LE session -- eight retained notifications carried uids 3, 9, 12,
// 13, 16, 17, 18 and 29, with one notification present twice under 9 and 29. A
// dedupe set carried across the session boundary therefore reads a genuinely new
// notification as a replay and drops it, which is a phone that has stopped
// mirroring for no visible reason.
TEST(AncsRegistry, ANewSessionStopsRecycledUidsReadingAsReplays) {
    NotificationRegistry registry;

    SourceEvent first;
    first.uid = 9;
    EXPECT_EQ(registry.classify(first), Decision::Fetch);
    registry.remember(first);
    EXPECT_EQ(registry.classify(first), Decision::Ignore) << "same session, same uid: a replay";

    registry.begin_session();

    SourceEvent recycled;
    recycled.uid = 9; // a different notification, same counter value
    EXPECT_EQ(registry.classify(recycled), Decision::Fetch)
        << "a recycled uid from a new session was dropped as a replay";
}

// The dedupe reset must not empty the notification list: an LE blip shows up as
// the device path dropping and returning, and wiping the list there would clear
// the UI on every flap.
TEST(AncsRegistry, ANewSessionKeepsWhatIsOnScreen) {
    NotificationRegistry registry;
    registry.store(make(9, "com.burbn.instagram"));
    const uint64_t before = registry.session();

    registry.begin_session();

    ASSERT_NE(registry.find(9), nullptr) << "a session change emptied the list";
    EXPECT_EQ(registry.find(9)->session, before) << "the stored copy must keep the session that issued its uid";
    EXPECT_NE(registry.session(), before);
}

// A reconnect replays the backlog under fresh uids. Without replacing the stale
// copy, every reconnect would list each notification again.
TEST(AncsRegistry, AReplayUnderANewSessionReplacesTheStaleCopy) {
    NotificationRegistry registry;
    Notification raid = make(9, "com.nianticlabs.pokemongo");
    raid.title = "Raid Invitation";
    raid.received = 100;
    registry.store(raid);
    Notification card = make(3, "com.apple.Passbook");
    card.title = "Apple Card";
    card.received = 100;
    registry.store(card);

    registry.begin_session();

    Notification replay = raid;
    replay.uid = 29;
    registry.store(replay);

    EXPECT_EQ(registry.size(), 2u) << "the replay was listed twice";
    EXPECT_EQ(registry.find(9), nullptr) << "the copy kept must carry the live uid";
    EXPECT_NE(registry.find(29), nullptr);
    EXPECT_NE(registry.find(3), nullptr) << "a different notification from the old session was dropped";

    Notification twin = replay;
    twin.uid = 30;
    registry.store(twin);
    EXPECT_EQ(registry.size(), 3u) << "identical notifications within one session are both real";
}

// iOS does not flag every replayed entry pre-existing; a copy of one from an earlier session is still backlog.
TEST(AncsRegistry, StoreReportsACopyFromAnEarlierSession) {
    NotificationRegistry registry;
    Notification post = make(13, "com.burbn.instagram");
    post.title = "Guille";
    post.body = "Desde pequeñitos";
    post.received = 100;
    EXPECT_FALSE(registry.store(post));

    registry.begin_session();

    Notification replay = post;
    replay.uid = 61;
    EXPECT_TRUE(registry.store(replay)) << "an unflagged replay must read as backlog";

    Notification fresh = post;
    fresh.uid = 62;
    fresh.body = "Otro mensaje";
    fresh.received = 200;
    EXPECT_FALSE(registry.store(fresh)) << "a new notification from the same app is not a replay";

    Notification twin = replay;
    twin.uid = 63;
    EXPECT_FALSE(registry.store(twin)) << "only a copy from an earlier session counts";
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

TEST(BluetoothConfig, MutedAppsRoundTrip) {
    Config config;
    config.muted_apps["org.whispersystems.signal"] = "Signal";
    EXPECT_EQ(deserialize_config(serialize_config(config)).muted_apps, config.muted_apps);
    EXPECT_TRUE(deserialize_config("{}").muted_apps.empty());
}

TEST(BluetoothConfig, SerializeStampsTheVersion) {
    Config config;
    config.ancs_content_enabled = false;
    EXPECT_FALSE(deserialize_config(serialize_config(config)).ancs_content_enabled);
}

namespace {

    SourceEvent event(EventId id, uint32_t uid) {
        SourceEvent e;
        e.event = id;
        e.uid = uid;
        return e;
    }

    Notification held_notification(uint32_t uid, const std::string& app_id) {
        Notification n;
        n.uid = uid;
        n.app_id = app_id;
        n.app_name = derive_app_name(app_id);
        return n;
    }

} // namespace

TEST(AncsFetchOrder, AdditionsAreNewestFirst) {
    const auto out = fetch_order({event(EventId::Added, 1), event(EventId::Added, 3), event(EventId::Added, 2)});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].uid, 3u);
    EXPECT_EQ(out[1].uid, 2u);
    EXPECT_EQ(out[2].uid, 1u);
}

TEST(AncsFetchOrder, RemovalsAndModificationsKeepTheirPlaceAheadOfAdditions) {
    const auto out = fetch_order(
        {event(EventId::Added, 5), event(EventId::Removed, 9), event(EventId::Added, 7), event(EventId::Modified, 2)});
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0].event, EventId::Removed);
    EXPECT_EQ(out[1].event, EventId::Modified);
    EXPECT_EQ(out[2].uid, 7u);
    EXPECT_EQ(out[3].uid, 5u);
}

TEST(AncsFetchOrder, EmptyAndSingleAreUnchanged) {
    EXPECT_TRUE(fetch_order({}).empty());
    EXPECT_EQ(fetch_order({event(EventId::Added, 4)}).size(), 1u);
}

TEST(AncsHeldPopups, ReleasedWithTheRealNameWhenItArrives) {
    HeldPopups held;
    held.hold(held_notification(1, "com.alibaba.iAliexpress"), 100);
    held.hold(held_notification(2, "com.burbn.instagram"), 100);
    held.hold(held_notification(3, "com.alibaba.iAliexpress"), 101);

    const auto out = held.release_app("com.alibaba.iAliexpress", "AliExpress");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].uid, 1u);
    EXPECT_EQ(out[1].uid, 3u);
    EXPECT_EQ(out[0].app_name, "AliExpress");
    EXPECT_EQ(out[1].app_name, "AliExpress");
    EXPECT_FALSE(held.empty());
}

TEST(AncsHeldPopups, ReleasedWithTheGuessAfterTheWait) {
    HeldPopups held;
    held.hold(held_notification(1, "com.alibaba.iAliexpress"), 100);
    held.hold(held_notification(2, "com.burbn.instagram"), 101);

    EXPECT_TRUE(held.release_expired(100 + HeldPopups::WAIT_SECONDS - 1).empty());

    const auto first = held.release_expired(100 + HeldPopups::WAIT_SECONDS);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].uid, 1u);
    EXPECT_EQ(first[0].app_name, "IAliexpress");

    const auto second = held.release_expired(101 + HeldPopups::WAIT_SECONDS);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].uid, 2u);
    EXPECT_TRUE(held.empty());
}

TEST(AncsHeldPopups, ReleaseAllEmptiesInOrder) {
    HeldPopups held;
    held.hold(held_notification(5, "a.b.c"), 100);
    held.hold(held_notification(6, "d.e.f"), 100);
    const auto out = held.release_all();
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].uid, 5u);
    EXPECT_EQ(out[1].uid, 6u);
    EXPECT_TRUE(held.empty());
    EXPECT_TRUE(held.release_all().empty());
}

TEST(AncsHeldPopups, HoldingTheSameUidAgainReplacesIt) {
    HeldPopups held;
    held.hold(held_notification(1, "a.b.c"), 100);
    auto again = held_notification(1, "a.b.c");
    again.title = "second";
    held.hold(std::move(again), 101);

    const auto out = held.release_app("a.b.c", "C");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].title, "second");
}

TEST(AncsHeldPopups, ReleasingAnotherAppLeavesTheRest) {
    HeldPopups held;
    held.hold(held_notification(1, "a.b.c"), 100);
    EXPECT_TRUE(held.release_app("x.y.z", "X").empty());
    EXPECT_FALSE(held.empty());
}

TEST(AncsHeldPopups, DroppedIsNeverReleased) {
    HeldPopups held;
    held.hold(held_notification(1, "a.b.c"), 100);
    held.hold(held_notification(2, "a.b.c"), 100);
    held.drop(1);
    const auto out = held.release_app("a.b.c", "C");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].uid, 2u);
    held.drop(99);
    EXPECT_TRUE(held.empty());
}
