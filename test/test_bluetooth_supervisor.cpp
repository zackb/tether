#include <gtest/gtest.h>
#include <tether/bluetooth/bearer_supervisor.hpp>
#include <tether/bluetooth/profile_supervisor.hpp>

#include <string>
#include <vector>

using namespace tether::bluetooth;

namespace {

    class FakeBearer : public BearerOps {
    public:
        bool present = true;
        bool paired = true;
        bool classic = false;
        bool le_available = true;
        bool le = false;
        bool classic_succeeds = true;
        bool le_succeeds = true;

        int classic_attempts = 0;
        int le_attempts = 0;
        std::vector<std::string> preferred;

        bool device_present() const override { return present; }
        bool device_paired() const override { return paired; }
        bool classic_connected() const override { return classic; }
        bool le_bearer_available() const override { return le_available; }
        bool le_connected() const override { return le; }

        void set_preferred_bearer(const std::string& bearer) override { preferred.push_back(bearer); }

        std::string classic_error = "br/edr refused";
        std::string le_error = "le refused";

        bool connect_classic(std::string& err) override {
            ++classic_attempts;
            if (!classic_succeeds) {
                err = classic_error;
                return false;
            }
            classic = true;
            return true;
        }

        bool connect_le(std::string& err) override {
            ++le_attempts;
            if (!le_succeeds) {
                err = le_error;
                return false;
            }
            le = true;
            return true;
        }
    };

    class FakeProfiles : public ProfileOps {
    public:
        std::string map_error;
        std::string pbap_error;
        int map_attempts = 0;
        int pbap_attempts = 0;
        std::vector<std::string> removed;
        int session_counter = 0;

        std::string create_session(const std::string& target, std::string& err) override {
            const bool is_map = target == "map";
            (is_map ? map_attempts : pbap_attempts)++;
            const std::string& failure = is_map ? map_error : pbap_error;
            if (!failure.empty()) {
                err = failure;
                return {};
            }
            return "/session" + std::to_string(++session_counter);
        }

        void remove_session(const std::string& path) override { removed.push_back(path); }
    };

} // namespace

// BR/EDR must come up first; LE must wait out the settle window. Connecting LE
// too early leaves the bond half-connected and flapping.
TEST(BearerSupervisor, ConnectsClassicThenLeAfterSettling) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    EXPECT_EQ(ops.classic_attempts, 1);
    EXPECT_EQ(ops.le_attempts, 0) << "LE must not be attempted before Classic is up";

    // Classic is up at t=1, but the settle window has not elapsed.
    sup.tick(1);
    EXPECT_EQ(ops.le_attempts, 0);
    sup.tick(1 + BEARER_SETTLE_SECONDS - 1);
    EXPECT_EQ(ops.le_attempts, 0) << "LE attempted before the settle window elapsed";

    sup.tick(1 + BEARER_SETTLE_SECONDS);
    EXPECT_EQ(ops.le_attempts, 1);
    EXPECT_TRUE(sup.status().le_connected);
}

// PreferredBearer is an instruction for the next connection, not a durable
// setting. Leaving LE preferred while idle changes later reconnect behavior.
TEST(BearerSupervisor, NeverLeavesLePreferredWhileIdle) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);

    ASSERT_FALSE(ops.preferred.empty());
    EXPECT_EQ(ops.preferred.back(), "bredr");

    // Every "le" selection must be immediately followed by a return to "bredr".
    for (size_t i = 0; i < ops.preferred.size(); ++i) {
        if (ops.preferred[i] == "le") {
            ASSERT_LT(i + 1, ops.preferred.size()) << "left LE preferred at the end";
            EXPECT_EQ(ops.preferred[i + 1], "bredr");
        }
    }
}

TEST(BearerSupervisor, ClassicBackoffGrowsAndCaps) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    EXPECT_EQ(ops.classic_attempts, 1);
    EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS);

    // Ticking inside the backoff window must not retry.
    sup.tick(BEARER_BACKOFF_MIN_SECONDS - 1);
    EXPECT_EQ(ops.classic_attempts, 1) << "retried before the backoff expired";

    sup.tick(BEARER_BACKOFF_MIN_SECONDS);
    EXPECT_EQ(ops.classic_attempts, 2);
    EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS * 2);

    // Drive many failures and confirm the cap holds.
    int64_t now = BEARER_BACKOFF_MIN_SECONDS;
    for (int i = 0; i < 20; ++i) {
        now += sup.status().classic_backoff;
        sup.tick(now);
    }
    EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MAX_SECONDS);
}

// InProgress means BlueZ is still finishing an operation of its own, which
// clears without help. Treating it like a refusal pushed the retry out to
// minutes and left LE — and so ANCS — down long after the cause was gone.
TEST(BearerSupervisor, InProgressDoesNotGrowTheClassicBackoff) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "GDBus.Error:org.bluez.Error.InProgress: In Progress";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    for (int i = 0; i < 10; ++i) {
        sup.tick(now);
        EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS);
        now += BEARER_BACKOFF_MIN_SECONDS;
    }
    EXPECT_EQ(ops.classic_attempts, 10);
}

TEST(BearerSupervisor, InProgressDoesNotGrowTheLeBackoff) {
    FakeBearer ops;
    ops.le_succeeds = false;
    ops.le_error = "GDBus.Error:org.bluez.Error.InProgress: In Progress";
    BearerSupervisor sup(ops, true);

    // Classic is observed connected on the tick after it is dialed, and the
    // settle window runs from there.
    sup.tick(0);
    sup.tick(1);
    int64_t now = 1 + BEARER_SETTLE_SECONDS;
    for (int i = 0; i < 10; ++i) {
        sup.tick(now);
        EXPECT_EQ(sup.status().le_backoff, BEARER_BACKOFF_MIN_SECONDS);
        now += BEARER_BACKOFF_MIN_SECONDS;
    }
    EXPECT_EQ(ops.le_attempts, 10);
}

// A refusal is different: the phone said no, and hammering it was observed to
// hold the bond in a half-connected flapping state.
TEST(BearerSupervisor, RefusalStillGrowsTheLeBackoff) {
    FakeBearer ops;
    ops.le_succeeds = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);
    EXPECT_EQ(sup.status().le_backoff, BEARER_BACKOFF_MIN_SECONDS);

    sup.tick(1 + BEARER_SETTLE_SECONDS + BEARER_BACKOFF_MIN_SECONDS);
    EXPECT_EQ(sup.status().le_backoff, BEARER_BACKOFF_MIN_SECONDS * 2);
}

TEST(BearerSupervisor, LeBackoffDoesNotBlockClassic) {
    FakeBearer ops;
    ops.le_succeeds = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);
    EXPECT_EQ(ops.le_attempts, 1);
    EXPECT_GT(sup.status().le_backoff, 0);
    EXPECT_TRUE(sup.status().classic_connected) << "an LE failure must not tear down Classic";
}

TEST(BearerSupervisor, RecoversAfterDisconnect) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);
    ASSERT_TRUE(sup.status().le_connected);

    // The phone walks out of range.
    ops.classic = false;
    ops.le = false;
    sup.tick(100);
    EXPECT_FALSE(sup.status().classic_connected);
    EXPECT_FALSE(sup.status().le_connected) << "LE cannot be up while Classic is down";
    EXPECT_EQ(ops.classic_attempts, 2);

    // ...and comes back.
    sup.tick(101);
    sup.tick(101 + BEARER_SETTLE_SECONDS);
    EXPECT_TRUE(sup.status().classic_connected);
    EXPECT_TRUE(sup.status().le_connected);
}

// reset() models a resume: recovery should start at once, not after waiting out
// a backoff accumulated before the machine slept.
TEST(BearerSupervisor, ResetClearsBackoffForImmediateRetry) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(BEARER_BACKOFF_MIN_SECONDS);
    ASSERT_EQ(ops.classic_attempts, 2);

    sup.reset();
    ops.classic_succeeds = true;
    sup.tick(BEARER_BACKOFF_MIN_SECONDS + 1);
    EXPECT_EQ(ops.classic_attempts, 3) << "reset must allow an immediate retry";
}

TEST(BearerSupervisor, SkipsLeWhenAncsDisabled) {
    FakeBearer ops;
    BearerSupervisor sup(ops, false);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS + 10);
    EXPECT_EQ(ops.le_attempts, 0);
    EXPECT_TRUE(sup.status().classic_connected);
}

TEST(BearerSupervisor, SkipsLeWhenBearerUnavailable) {
    FakeBearer ops;
    ops.le_available = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS + 10);
    EXPECT_EQ(ops.le_attempts, 0);
    EXPECT_FALSE(sup.status().le_available);
}

TEST(BearerSupervisor, ReportsMissingDevice) {
    FakeBearer ops;
    ops.present = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    EXPECT_FALSE(sup.status().device_present);
    EXPECT_EQ(ops.classic_attempts, 0) << "must not dial a device BlueZ does not know";
}

// A known-but-unpaired device happens after an unpair or after the phone forgets
// this computer. Dialing it collides with the pairing transaction, and BlueZ
// answers both attempts with br-connection-busy.
TEST(BearerSupervisor, StandsDownWhenDeviceIsUnpaired) {
    FakeBearer ops;
    ops.paired = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(BEARER_POLL_SECONDS * 20);
    EXPECT_TRUE(sup.status().device_present);
    EXPECT_FALSE(sup.status().device_paired);
    EXPECT_EQ(ops.classic_attempts, 0) << "must not dial an unpaired device";
    EXPECT_EQ(ops.le_attempts, 0);
    EXPECT_TRUE(ops.preferred.empty()) << "must not touch PreferredBearer before a bond exists";
}

// Forbidden and Busy look similar but call for opposite advice. Reporting a
// permissions problem as a busy session sends the user off re-pairing for
// nothing, and vice versa.
TEST(ObexErrors, DistinguishesForbiddenFromBusy) {
    EXPECT_EQ(classify_obex_error("Forbidden"), ObexError::Forbidden);
    EXPECT_EQ(classify_obex_error("org.bluez.obex.Error.Forbidden: Forbidden"), ObexError::Forbidden);
    EXPECT_EQ(classify_obex_error("OBEX response 0x43"), ObexError::Forbidden);

    EXPECT_EQ(classify_obex_error("Connection refused (111)"), ObexError::Busy);
    EXPECT_EQ(classify_obex_error("connection refused"), ObexError::Busy);
}

// Observed on real hardware with the iPhone bonded, connected, and all three
// permission toggles verified on. Classifying it as Forbidden would send the
// user to settings that are already correct.
TEST(ObexErrors, ServiceRecordMissingIsNotAPermissionProblem) {
    const std::string real = "GDBus.Error:org.bluez.obex.Error.Failed: Unable to find service record";
    EXPECT_EQ(classify_obex_error(real), ObexError::NoRecord);
    EXPECT_NE(classify_obex_error(real), ObexError::Forbidden);

    // obexd reports a missing record whenever its SDP fetch is refused, so the
    // advice must name both plausible causes and must not send the user
    // re-pairing, which fixes neither.
    const std::string advice = obex_error_advice(ObexError::NoRecord, "map");
    EXPECT_NE(advice.find("another computer"), std::string::npos);
    EXPECT_NE(advice.find("Show Message Notifications"), std::string::npos);
    EXPECT_NE(advice.find("re-pairing is not the fix"), std::string::npos);
}

TEST(ObexErrors, ClassifiesUnavailableAndOther) {
    EXPECT_EQ(classify_obex_error("Host is down"), ObexError::Unavailable);
    EXPECT_EQ(classify_obex_error("Page Timeout"), ObexError::Unavailable);
    EXPECT_EQ(classify_obex_error(""), ObexError::None);
    EXPECT_EQ(classify_obex_error("something entirely new"), ObexError::Other);
}

TEST(ObexErrors, AdviceNamesThePermissionToggle) {
    const std::string map_advice = obex_error_advice(ObexError::Forbidden, "map");
    EXPECT_NE(map_advice.find("Show Message Notifications"), std::string::npos);
    // Must not send the user re-pairing over a permission.
    EXPECT_EQ(map_advice.find("re-pair"), std::string::npos);

    EXPECT_NE(obex_error_advice(ObexError::Forbidden, "pbap").find("Sync Contacts"), std::string::npos);
    EXPECT_NE(obex_error_advice(ObexError::Busy, "map").find("another computer"), std::string::npos);
}

TEST(ProfileSupervisor, OpensBothSessionsWhenLinkReady) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_TRUE(sup.status().pbap_open);
    EXPECT_FALSE(sup.map_session().empty());
    EXPECT_FALSE(sup.pbap_session().empty());
}

TEST(ProfileSupervisor, DoesNothingUntilLinkIsReady) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, false);
    EXPECT_EQ(ops.map_attempts, 0);
    EXPECT_EQ(ops.pbap_attempts, 0);
}

// A Forbidden may be a stale obexd session from a previous run, so exactly one
// retry is allowed. It must never escalate to restarting obexd.
TEST(ProfileSupervisor, RetriesOnceAfterForbidden) {
    FakeProfiles ops;
    ops.map_error = "Forbidden";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    EXPECT_EQ(ops.map_attempts, 2) << "expected exactly one retry after Forbidden";
    EXPECT_FALSE(sup.status().map_open);
    EXPECT_EQ(sup.status().map_error, ObexError::Forbidden);

    // PBAP opened, so the supervisor is on the steady cadence even though MAP is
    // still failing. The extra Forbidden retry is spent, so this poll makes
    // exactly one attempt — a persistent permission problem must not turn into a
    // retry burst against obexd.
    sup.tick(PROFILE_STEADY_POLL_SECONDS, true);
    EXPECT_EQ(ops.map_attempts, 3);
}

// While nothing has opened yet, the fast cadence applies so a user flipping the
// permission toggles is picked up promptly.
TEST(ProfileSupervisor, UsesFastCadenceUntilSomethingOpens) {
    FakeProfiles ops;
    ops.map_error = "Forbidden";
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    const int attempts = ops.map_attempts;

    sup.tick(PROFILE_INITIAL_POLL_SECONDS - 1, true);
    EXPECT_EQ(ops.map_attempts, attempts);

    sup.tick(PROFILE_INITIAL_POLL_SECONDS, true);
    EXPECT_GT(ops.map_attempts, attempts);
}

TEST(ProfileSupervisor, PartialSuccessKeepsRetryingTheOtherProfile) {
    FakeProfiles ops;
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_FALSE(sup.status().pbap_open);

    const int map_attempts_after_open = ops.map_attempts;
    ops.pbap_error.clear();
    sup.tick(PROFILE_STEADY_POLL_SECONDS, true);
    EXPECT_TRUE(sup.status().pbap_open);
    EXPECT_EQ(ops.map_attempts, map_attempts_after_open) << "must not reopen an already-open session";
}

TEST(ProfileSupervisor, RespectsPollInterval) {
    FakeProfiles ops;
    ops.map_error = "Connection refused (111)";
    ops.pbap_error = "Connection refused (111)";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    const int attempts = ops.map_attempts;
    sup.tick(1, true);
    EXPECT_EQ(ops.map_attempts, attempts) << "retried before the poll interval elapsed";

    sup.tick(PROFILE_INITIAL_POLL_SECONDS, true);
    EXPECT_GT(ops.map_attempts, attempts);
}

TEST(ProfileSupervisor, ResetRemovesSessions) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    ASSERT_TRUE(sup.status().map_open);

    sup.reset();
    EXPECT_EQ(ops.removed.size(), 2u);
    EXPECT_FALSE(sup.status().map_open);
    EXPECT_TRUE(sup.map_session().empty());

    // And reopens on the next tick.
    sup.tick(100, true);
    EXPECT_TRUE(sup.status().map_open);
}

TEST(ProfileSupervisor, BusySessionKeepsPollingWithoutRetryBurst) {
    FakeProfiles ops;
    ops.map_error = "Connection refused (111)";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    // Busy is not Forbidden, so it gets no extra immediate retry.
    EXPECT_EQ(ops.map_attempts, 1);
    EXPECT_EQ(sup.status().map_error, ObexError::Busy);
}
