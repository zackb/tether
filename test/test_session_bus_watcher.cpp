#include "scoped_env.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <gio/gio.h>
#include <gtest/gtest.h>
#include <string>
#include <tether/session_bus_watcher.hpp>

namespace {

    // A private dbus-daemon for one test, standing in for the user's session bus.
    class SessionBusWatcherTest : public ::testing::Test {
    protected:
        void SetUp() override {
            gchar* daemon = g_find_program_in_path("dbus-daemon");
            if (!daemon)
                GTEST_SKIP() << "dbus-daemon is not installed";
            g_free(daemon);
            bus_ = g_test_dbus_new(G_TEST_DBUS_NONE);
            g_test_dbus_up(bus_); // points DBUS_SESSION_BUS_ADDRESS at it
        }

        void TearDown() override {
            if (bus_) {
                g_test_dbus_down(bus_);
                g_object_unref(bus_);
            }
        }

        // g_test_dbus_up/down replace and then unset the variable; put back what the run had.
        const char* previous_ = g_getenv("DBUS_SESSION_BUS_ADDRESS");
        tether::testing::ScopedEnv restore_{"DBUS_SESSION_BUS_ADDRESS", std::string(previous_ ? previous_ : "")};
        GTestDBus* bus_ = nullptr;
    };

} // namespace

TEST_F(SessionBusWatcherTest, ReportsTheBusGoingAway) {
    std::promise<void> closed;
    auto fired = closed.get_future();
    std::atomic<int> calls{0};

    tether::SessionBusWatcher watcher([&] {
        if (calls++ == 0)
            closed.set_value();
    });
    ASSERT_TRUE(watcher.start());

    g_test_dbus_stop(bus_); // the bus daemon goes away, as at a GNOME logout
    ASSERT_EQ(fired.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(calls.load(), 1);
}

TEST_F(SessionBusWatcherTest, StaysQuietWhenItIsDestroyedFirst) {
    std::atomic<int> calls{0};
    {
        tether::SessionBusWatcher watcher([&] { ++calls; });
        ASSERT_TRUE(watcher.start());
    }
    EXPECT_EQ(calls.load(), 0);
}

TEST(SessionBusWatcher, WatchesNothingWithoutABus) {
    tether::testing::ScopedEnv env("DBUS_SESSION_BUS_ADDRESS", std::string("unix:path=/nonexistent/tether-test-bus"));
    std::atomic<int> calls{0};
    tether::SessionBusWatcher watcher([&] { ++calls; });
    EXPECT_FALSE(watcher.start());
    EXPECT_EQ(calls.load(), 0);
}
