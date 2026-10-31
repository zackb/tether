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

        bool device_present() const override { return present; }
        bool device_paired() const override { return paired; }
        bool classic_connected() const override { return classic; }
        bool le_bearer_available() const override { return le_available; }
        bool le_connected() const override { return le; }

        // BlueZ's default is "last-seen"; only the pin the Classic fallback
        // writes matters here.
        std::string bearer_pref = "last-seen";
        std::vector<std::string> bearer_writes;
        std::string preferred_bearer() const override { return bearer_pref; }
        void set_preferred_bearer(const std::string& bearer) override {
            bearer_pref = bearer;
            bearer_writes.push_back(bearer);
        }

        std::string classic_error = "br/edr refused";
        std::string le_error = "le refused";

        ConnectResult connect_classic(std::string& err) override {
            ++classic_attempts;
            if (!classic_succeeds) {
                err = classic_error;
                return ConnectResult::Failed;
            }
            classic = true;
            return ConnectResult::Requested;
        }

        ConnectResult connect_le(std::string& err) override {
            ++le_attempts;
            if (!le_succeeds) {
                err = le_error;
                return ConnectResult::Failed;
            }
            le = true;
            return ConnectResult::Requested;
        }

        // Models BlueZ holding the accept-list registration open. Tests that
        // care set this; the default of "nothing outstanding" keeps the older
        // cases reading as they did.
        bool outstanding = false;
        bool le_connect_outstanding() const override { return outstanding; }

        // The healthy case: the adapter accepted the solicitation. Only a test
        // about an adapter that will not advertise clears this.
        bool on_air = true;
        bool solicitation_on_air() const override { return on_air; }

        // Hands-free connected, as the bus shows it. Tests about the BR/EDR
        // cycle clear this.
        bool telephony = true;
        int classic_disconnects = 0;

        bool telephony_present() const override { return telephony; }

        bool disconnect_classic(std::string&) override {
            ++classic_disconnects;
            classic = false;
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
    EXPECT_FALSE(sup.status().le_connected) << "BlueZ accepting a connect is not a link";

    // Only an observed Connected is one.
    sup.tick(2 + BEARER_SETTLE_SECONDS);
    EXPECT_TRUE(sup.status().le_connected);
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
// InProgress means BlueZ is still working on the last connect, which it keeps
// doing long after a caller gives up. Retrying at the same cadence guarantees
// the next attempt collides with it too, so one stalled connect becomes a
// permanent storm. Standing further back is the only thing that clears it.
TEST(BearerSupervisor, InProgressWidensTheClassicWindow) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "GDBus.Error:org.bluez.Error.InProgress: In Progress";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    int previous = 0;
    for (int i = 0; i < 5; ++i) {
        sup.tick(now);
        EXPECT_GT(sup.status().classic_backoff, previous) << "collided again at the same cadence";
        previous = sup.status().classic_backoff;
        now += previous;
    }
    EXPECT_EQ(ops.classic_attempts, 5);
}

// One dial per daemon, then the advert owns the radio. Measured 2026-08-22: the
// solicitation went on air at t=127.668 and the iPhone opened the link inbound
// 1.3s later, while three captures totalling ~700s produced no link from a dial
// -- including one where the phone was advertising with a dial armed. reset()
// must not refund the dial: a Classic flap would otherwise take the advert back
// off air for the length of another attempt.
TEST(BearerSupervisor, DialsLeOnceThenLeavesItToTheSolicitation) {
    FakeBearer ops;
    ops.le_succeeds = false; // BlueZ answers, no link follows
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    for (; now < 400; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.le_attempts, 1) << "kept dialling past the one attempt";

    // A Classic flap, and the phone coming back, must not buy another.
    ops.classic = false;
    sup.tick(now++);
    sup.reset();
    ops.classic_succeeds = true;
    for (int i = 0; i < 400; ++i)
        sup.tick(now++);
    EXPECT_EQ(ops.le_attempts, 1) << "reset() refunded the dial and took the advert off air again";
}

// The advert must not go up while a dial is in flight -- that is what aborted
// the connect once per cycle (2026-08-20). le_dialling gates the solicitation,
// so it stays true until the reply lands and false forever after.
TEST(BearerSupervisor, SolicitationIsHeldOffOnlyWhileTheOneDialIsInFlight) {
    class ListeningBearer : public FakeBearer {
    public:
        ConnectResult connect_le(std::string&) override {
            ++le_attempts;
            outstanding = true;
            return ConnectResult::Requested;
        }
    } ops;

    BearerSupervisor sup(ops, true);
    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    ASSERT_EQ(ops.le_attempts, 1);
    EXPECT_TRUE(sup.status().le_dialling) << "solicited on top of a dial still in flight";

    ops.outstanding = false;
    sup.tick(++now);
    EXPECT_FALSE(sup.status().le_dialling);
    for (int i = 0; i < 500; ++i) {
        sup.tick(++now);
        ASSERT_FALSE(sup.status().le_dialling) << "took the solicitation off air again at t=" << now;
    }
}

// A dial outstanding when BR/EDR drops must not latch the solicitation off air.
// Nothing else on the Classic-down path re-reads it, and the advert is the only
// thing that brings LE back -- so a stale `true` here is a permanent outage on a
// host whose Classic link never returns (2026-08-23).
TEST(BearerSupervisor, ClassicDroppingUnderADialDoesNotLatchTheSolicitationOffAir) {
    class ListeningBearer : public FakeBearer {
    public:
        ConnectResult connect_le(std::string&) override {
            ++le_attempts;
            outstanding = true;
            return ConnectResult::Requested;
        }
    } ops;

    BearerSupervisor sup(ops, true);
    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    ASSERT_EQ(ops.le_attempts, 1);
    ASSERT_TRUE(sup.status().le_dialling);

    // BR/EDR goes and does not come back, which is what a host with no locally
    // connectable profile looks like.
    ops.classic = false;
    ops.classic_succeeds = false;
    sup.tick(++now);
    EXPECT_TRUE(sup.status().le_dialling) << "the dial really is still in flight";

    // BlueZ answers. Nothing re-enters the LE branch, so this is the only place
    // the flag can be cleared.
    ops.outstanding = false;
    for (int i = 0; i < 500; ++i)
        sup.tick(++now);
    EXPECT_FALSE(sup.status().le_dialling)
        << "a finished dial kept the advert off air for the whole time BR/EDR was down";

    BearerStatus bearer = sup.status();
    bearer.le_available = true;
    EXPECT_TRUE(should_solicit_ancs(true, bearer, now, -1)) << "the advert is what brings LE back";
}

// A dialled LE link can come up carrying no ANCS at all, and while it reads
// connected the solicitation stays off air -- so nothing ever asks the phone for
// the service, and notifications never arrive (2026-08-23).
TEST(AncsSolicitation, GoesBackOnAirOverAnLeLinkThatCarriesNoAncs) {
    BearerStatus bearer;
    bearer.le_available = true;
    bearer.le_connected = true;

    // Offered, or not yet timed: the link is doing its job, leave it alone.
    EXPECT_FALSE(should_solicit_ancs(true, bearer, 10'000, -1));
    EXPECT_FALSE(should_solicit_ancs(true, bearer, 100, 100)) << "solicited before discovery could finish";
    EXPECT_FALSE(should_solicit_ancs(true, bearer, 100 + ANCS_ABSENT_GRACE_SECONDS - 1, 100));

    EXPECT_TRUE(should_solicit_ancs(true, bearer, 100 + ANCS_ABSENT_GRACE_SECONDS, 100))
        << "left the advert off air over a link that carries no ANCS";

    // A dial in flight still owns the radio: the advert aborts it (2026-08-20).
    bearer.le_dialling = true;
    EXPECT_FALSE(should_solicit_ancs(true, bearer, 10'000, 100));
    bearer.le_dialling = false;

    // LE down is the original case, and the timer is irrelevant to it.
    bearer.le_connected = false;
    EXPECT_TRUE(should_solicit_ancs(true, bearer, 10'000, -1));

    // Nothing goes on air for a disabled feature or an adapter without LE.
    EXPECT_FALSE(should_solicit_ancs(false, bearer, 10'000, -1));
    bearer.le_available = false;
    EXPECT_FALSE(should_solicit_ancs(true, bearer, 10'000, -1));
}

// A phone that is answering answers in about a second. A long silence with the
// advert up is the iPhone having gone quiet, and only cycling Bluetooth on the
// phone clears that -- so the status has to say so rather than "waiting" forever.
TEST(BearerSupervisor, NamesThePhoneCycleAfterALongSilence) {
    FakeBearer ops;
    ops.le_succeeds = false;
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    EXPECT_NE(sup.status().reason.find("Waiting for the iPhone to open the LE link"), std::string::npos);

    sup.tick(now + LE_SILENT_SECONDS);
    EXPECT_NE(sup.status().reason.find("not answering on LE"), std::string::npos);
    EXPECT_NE(sup.status().reason.find("turn Bluetooth off"), std::string::npos);
}

// Same silence, but the adapter never put the solicitation on air, so the phone
// was never asked for the service. Sending that user to cycle Bluetooth on the
// iPhone is the #128 dead end: nothing on the phone is wrong.
TEST(BearerSupervisor, BlamesTheAdapterWhenNothingWasEverSolicited) {
    FakeBearer ops;
    ops.le_succeeds = false;
    ops.on_air = false;
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);

    sup.tick(now + LE_SILENT_SECONDS);
    EXPECT_NE(sup.status().reason.find("not putting the notification request on air"), std::string::npos);
    EXPECT_EQ(sup.status().reason.find("turn Bluetooth off"), std::string::npos) << "blames the phone";
}

// #128: the Classic fallback pins the bond to bredr, and a bond left pinned
// refuses the inbound LE link the iPhone opens. Nothing used to hand it back, so
// the pin outlived every re-pair and LE stayed down for good.
TEST(BearerSupervisor, HandsTheBearerPreferenceBackToLeOnceClassicSettles) {
    FakeBearer ops;
    ops.le_succeeds = false;
    ops.bearer_pref = "bredr";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    EXPECT_TRUE(ops.bearer_writes.empty()) << "wrote before the Classic link settled";

    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    EXPECT_EQ(ops.bearer_writes, std::vector<std::string>{"le"});

    // One write per Classic session, not one per poll.
    sup.tick(++now);
    sup.tick(++now);
    EXPECT_EQ(ops.bearer_writes.size(), 1u);
}

// Writing the property under a dial in flight is what cancelled the connect as
// le-connection-abort-by-local.
TEST(BearerSupervisor, DoesNotWriteTheBearerPreferenceOverADialInFlight) {
    FakeBearer ops;
    ops.le_succeeds = false;
    ops.bearer_pref = "bredr";
    ops.outstanding = true;
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    EXPECT_TRUE(ops.bearer_writes.empty()) << "wrote over an outstanding connect";

    ops.outstanding = false;
    sup.tick(++now);
    EXPECT_EQ(ops.bearer_writes, std::vector<std::string>{"le"});
}

// A bond still reading bredr after the silence is this computer's doing. Sending
// that user to cycle Bluetooth on the iPhone is the other #128 dead end.
TEST(BearerSupervisor, BlamesThePinnedBondRatherThanThePhone) {
    FakeBearer ops;
    ops.le_succeeds = false;
    ops.bearer_pref = "bredr";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    ops.bearer_pref = "bredr"; // the write has not taken effect on the bus yet

    sup.tick(now + LE_SILENT_SECONDS);
    EXPECT_NE(sup.status().reason.find("pinned the bond to BR/EDR"), std::string::npos);
    EXPECT_EQ(sup.status().reason.find("turn Bluetooth off"), std::string::npos) << "blames the phone";
}

// The advert is legitimately off air while a dial owns the radio. One tick of
// that must not turn into "your adapter is broken".
TEST(BearerSupervisor, OneMomentOffAirDoesNotBlameTheAdapter) {
    FakeBearer ops;
    ops.le_succeeds = false;
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);

    ops.on_air = false;
    sup.tick(now + LE_SILENT_SECONDS);
    EXPECT_NE(sup.status().reason.find("not answering on LE"), std::string::npos);
}

TEST(BearerSupervisor, RecoversAfterDisconnect) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);
    sup.tick(2 + BEARER_SETTLE_SECONDS);
    ASSERT_TRUE(sup.status().le_connected);

    // The phone walks out of range.
    ops.classic = false;
    ops.le = false;
    sup.tick(100);
    EXPECT_FALSE(sup.status().classic_connected);
    EXPECT_FALSE(sup.status().le_connected);
    EXPECT_EQ(ops.classic_attempts, 2);

    // ...and comes back, with the iPhone answering the solicitation as it does
    // on real hardware -- the daemon spent its one dial long ago.
    sup.tick(101);
    ops.le = true;
    sup.tick(102 + BEARER_SETTLE_SECONDS);
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

// With bluetooth.disable_ertm set, obexd's L2CAP connect fails locally and is
// reported as a missing record; blaming the phone toggle sends the user nowhere.
TEST(ObexErrors, DisabledErtmIsNamedInsteadOfThePhone) {
    const std::string advice = obex_error_advice(ObexError::NoRecord, "pbap", true);
    EXPECT_NE(advice.find("disable_ertm"), std::string::npos);
    EXPECT_EQ(advice.find("Sync Contacts"), std::string::npos);
    EXPECT_EQ(advice.find("another computer"), std::string::npos);
}

TEST(ObexErrors, ClassifiesUnavailableAndOther) {
    EXPECT_EQ(classify_obex_error("Host is down"), ObexError::Unavailable);
    EXPECT_EQ(classify_obex_error("Page Timeout"), ObexError::Unavailable);
    EXPECT_EQ(classify_obex_error(""), ObexError::None);
    EXPECT_EQ(classify_obex_error("something entirely new"), ObexError::Other);
}

TEST(ObexErrors, MissingObexdIsNotBlamedOnThePhone) {
    // Verbatim from a machine where bluez-obex had been uninstalled.
    const std::string real = "GDBus.Error:org.freedesktop.DBus.Error.ServiceUnknown: The name is not activatable";
    EXPECT_EQ(classify_obex_error(real), ObexError::NoDaemon);

    const std::string advice = obex_error_advice(ObexError::NoDaemon, "map");
    EXPECT_NE(advice.find("bluez-obex"), std::string::npos);
    // Nothing about this is fixable on the phone, so the advice must not say so.
    EXPECT_EQ(advice.find("iPhone's Settings"), std::string::npos);
    EXPECT_EQ(advice.find("re-pair"), std::string::npos);
}

TEST(ObexErrors, AdviceNamesThePermissionToggle) {
    const std::string map_advice = obex_error_advice(ObexError::Forbidden, "map");
    EXPECT_NE(map_advice.find("Show Message Notifications"), std::string::npos);
    // Must not send the user re-pairing over a permission.
    EXPECT_EQ(map_advice.find("re-pair"), std::string::npos);

    EXPECT_NE(obex_error_advice(ObexError::Forbidden, "pbap").find("Sync Contacts"), std::string::npos);
    EXPECT_NE(obex_error_advice(ObexError::Busy, "map").find("another computer"), std::string::npos);
}

TEST(ProfileSupervisor, OpensBothSessionsWhenTheDeviceIsReady) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_TRUE(sup.status().pbap_open);
    EXPECT_FALSE(sup.map_session().empty());
    EXPECT_FALSE(sup.pbap_session().empty());
}

TEST(ProfileSupervisor, DoesNothingUntilTheDeviceIsReady) {
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

// The iPhone serves one MAP session at a time, so a supervisor that is dropped
// without releasing its sessions blocks the next one until obexd is restarted.
TEST(ProfileSupervisor, DestructionReleasesSessions) {
    FakeProfiles ops;
    {
        ProfileSupervisor sup(ops);
        sup.tick(0, true);
        ASSERT_TRUE(sup.status().map_open);
        ASSERT_TRUE(sup.status().pbap_open);
    }
    EXPECT_EQ(ops.removed.size(), 2u);
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

TEST(BearerSupervisor, TransientFailuresDoNotEscalateTheBackoff) {
    for (const char* message : {"org.bluez.Error.NotReady: Resource Not Ready",
                                "org.bluez.Error.Failed: Software caused connection abort",
                                "org.bluez.Error.Failed: le-connection-abort-by-local",
                                "org.bluez.Error.Failed: br-connection-refused"}) {
        FakeBearer ops;
        ops.classic_succeeds = false;
        ops.classic_error = message;
        BearerSupervisor sup(ops, true);

        int64_t now = 0;
        for (int attempt = 0; attempt < 6; ++attempt) {
            sup.tick(now);
            EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS) << message;
            now += sup.status().classic_backoff;
        }
    }
}

// A phone that walked out of the room, or has Bluetooth off, answers with a page
// timeout. Paging it every 5s forever is what wedges bluetoothd; the five-minute
// ceiling would leave the link down long after it is back on the desk.
TEST(BearerSupervisor, AbsentPhoneBacksOffToTheAbsentCeiling) {
    for (const char* message : {"org.bluez.Error.Failed: br-connection-page-timeout",
                                "org.bluez.Error.Failed: Host is down",
                                "org.bluez.Error.Failed: Connection timed out"}) {
        FakeBearer ops;
        ops.classic_succeeds = false;
        ops.classic_error = message;
        BearerSupervisor sup(ops, true);

        int64_t now = 0;
        sup.tick(now);
        const int first = sup.status().classic_backoff;
        for (int attempt = 0; attempt < 10; ++attempt) {
            now += sup.status().classic_backoff;
            sup.tick(now);
        }
        EXPECT_GT(sup.status().classic_backoff, first) << message;
        EXPECT_EQ(sup.status().classic_backoff, ABSENT_CLASSIC_BACKOFF_MAX_SECONDS) << message;
    }
}

// A refusal from the phone is a real answer, and repeating it every five seconds
// helps nobody.
TEST(BearerSupervisor, PersistentFailuresStillEscalate) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "org.bluez.Error.AuthenticationFailed";
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    const int first = sup.status().classic_backoff;
    sup.tick(first);
    EXPECT_GT(sup.status().classic_backoff, first);
}

// reset() is what a caller reaches for when the phone reappears. The existing
// coverage stops at a minimum backoff; the case that actually bites is a backoff
// that grew to minutes while the phone was out of range.
TEST(BearerSupervisor, ResetClearsAnEscalatedBackoff) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "org.bluez.Error.AuthenticationFailed";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    now += sup.status().classic_backoff;
    sup.tick(now);
    ASSERT_GT(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS);
    const int attempts = ops.classic_attempts;

    sup.reset();
    EXPECT_EQ(sup.status().classic_backoff, 0);
    sup.tick(++now);
    EXPECT_EQ(ops.classic_attempts, attempts + 1) << "reset() left the grown backoff in place";
}

// The Forbidden retry exists to drop a stale session left by a previous run.
// Removing the empty string that the failed CreateSession just wrote drops
// nothing at all.
TEST(ProfileSupervisor, ForbiddenRetryRemovesThePreviousSession) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    ASSERT_TRUE(sup.status().map_open);
    const std::string stale = sup.map_session();
    ASSERT_FALSE(stale.empty());

    // The session goes sour; the next attempt is refused.
    sup.drop_map();
    ops.removed.clear();
    ops.map_error = "org.bluez.obex.Error.Forbidden";
    sup.tick(PROFILE_STEADY_POLL_SECONDS * 2, true);

    EXPECT_EQ(sup.status().map_error, ObexError::Forbidden);
    EXPECT_EQ(ops.map_attempts, 3) << "the one extra retry did not happen";
}

// A MAP session can die while the Classic link stays up. Nothing else notices,
// because tick() short-circuits once map_open is set.
TEST(ProfileSupervisor, DropMapReopensWithoutTouchingPbap) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    ASSERT_TRUE(sup.status().map_open);
    ASSERT_TRUE(sup.status().pbap_open);
    const std::string first_map = sup.map_session();
    const std::string pbap = sup.pbap_session();

    sup.drop_map();
    EXPECT_FALSE(sup.status().map_open);
    EXPECT_TRUE(sup.status().pbap_open) << "contacts were dropped along with messages";
    ASSERT_EQ(ops.removed.size(), 1u);
    EXPECT_EQ(ops.removed.front(), first_map);

    // And reopens on the very next tick rather than waiting out the steady poll.
    sup.tick(1, true);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_NE(sup.map_session(), first_map);
    EXPECT_EQ(sup.pbap_session(), pbap) << "the surviving PBAP session was reopened needlessly";
}

// A held profile frees the iPhone's single MAP session for another computer, so
// it must be removed and stay closed until the hold lifts.
TEST(ProfileSupervisor, HeldMapIsRemovedAndNotReopened) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    const std::string first_map = sup.map_session();
    const std::string pbap = sup.pbap_session();

    sup.set_held(true, false, false);
    ASSERT_EQ(ops.removed.size(), 1u);
    EXPECT_EQ(ops.removed.front(), first_map);
    EXPECT_FALSE(sup.status().map_open);
    EXPECT_TRUE(sup.status().map_held);

    sup.tick(100, true);
    EXPECT_EQ(ops.map_attempts, 1) << "a held MAP session was reopened";
    EXPECT_EQ(sup.pbap_session(), pbap);
    EXPECT_NE(sup.status().reason.find("released"), std::string::npos);

    // Lifting the hold reopens on the next tick, without waiting out the poll.
    sup.set_held(false, false, false);
    sup.tick(101, true);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_EQ(ops.map_attempts, 2);
}

// Re-applying the same hold every tick must not churn sessions.
TEST(ProfileSupervisor, RepeatedHoldIsIdempotent) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    sup.set_held(true, true, true);
    sup.set_held(true, true, true);
    EXPECT_EQ(ops.removed.size(), 2u);
    sup.tick(1, true);
    EXPECT_EQ(ops.map_attempts + ops.pbap_attempts, 2);
    EXPECT_NE(sup.status().reason.find("idle"), std::string::npos);
}

// iOS routinely keeps an LE link while letting BR/EDR lapse. ANCS rides LE
// alone, so notification mirroring keeps working there — reporting LE as down
// because BR/EDR is would tear down a GATT session that is still delivering.
TEST(BearerSupervisor, LeStaysReportedWhenOnlyClassicIsDown) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);
    sup.tick(2 + BEARER_SETTLE_SECONDS);
    ASSERT_TRUE(sup.status().classic_connected);
    ASSERT_TRUE(sup.status().le_connected);

    // BR/EDR lapses; the LE link is untouched.
    ops.classic = false;
    ops.classic_succeeds = false;
    sup.tick(100);

    EXPECT_FALSE(sup.status().classic_connected);
    EXPECT_TRUE(sup.status().le_connected) << "an LE link that is still up must not be reported as down";
    EXPECT_GT(ops.classic_attempts, 1) << "BR/EDR must still be retried";
}

// An iPhone that refuses BR/EDR while unlocked with its permissions on is wedged
// on its own side. Only cycling Bluetooth on the phone clears it, so the status
// has to say that rather than claiming to still be connecting.
TEST(BearerSupervisor, RepeatedRefusalNamesTheRemedy) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "org.bluez.Error.Failed: br-connection-refused";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    EXPECT_NE(sup.status().reason.find("Connecting"), std::string::npos)
        << "one refusal is not yet evidence of anything";

    while (ops.classic_attempts < CLASSIC_FAILURES_BEFORE_ADVICE) {
        now += sup.status().classic_backoff;
        sup.tick(now);
    }

    EXPECT_NE(sup.status().reason.find("Turn Bluetooth off and back on"), std::string::npos)
        << "the status never names the one thing that clears this";
    EXPECT_NE(sup.status().reason.find("Re-pairing is not the fix"), std::string::npos);

    // And it goes away the moment the phone accepts. The backoff has to elapse
    // first, or no attempt is made and the advice rightly still stands.
    ops.classic_succeeds = true;
    now += sup.status().classic_backoff;
    sup.tick(now);
    EXPECT_EQ(sup.status().reason.find("Turn Bluetooth off"), std::string::npos);
    sup.tick(++now);
    EXPECT_TRUE(sup.status().classic_connected);
}

// Walking back into range must not be held behind the backoff the absence grew.
TEST(BearerSupervisor, LeComingUpClearsTheClassicBackoff) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    ops.classic_succeeds = false;
    ops.classic_error = "br-connection-unknown";

    // Out of range: BR/EDR fails until the backoff reaches its ceiling.
    int64_t now = 0;
    while (now < 1200) {
        sup.tick(now);
        now += BEARER_POLL_SECONDS;
    }
    ASSERT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MAX_SECONDS)
        << "expected the absence to grow the backoff to its ceiling";
    const int attempts_while_away = ops.classic_attempts;

    // Back in range: the phone sees the solicitation and opens LE inbound.
    ops.le = true;
    ops.classic_succeeds = true;
    sup.tick(now);

    EXPECT_EQ(ops.classic_attempts, attempts_while_away + 1)
        << "LE came up and BR/EDR still sat out the backoff grown while the phone was away";
    EXPECT_EQ(sup.status().classic_backoff, 0);
}

// The "phone keeps refusing" advice must not survive a phone that was merely away.
TEST(BearerSupervisor, LeComingUpClearsTheRefusalAdvice) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    ops.classic_succeeds = false;
    ops.classic_error = "br-connection-unknown";

    int64_t now = 0;
    for (int i = 0; i < 40; ++i) {
        sup.tick(now);
        now += BEARER_POLL_SECONDS;
    }
    ASSERT_NE(sup.status().reason.find("keeps refusing"), std::string::npos);

    ops.le = true;
    sup.tick(now);
    EXPECT_EQ(sup.status().reason.find("keeps refusing"), std::string::npos)
        << "blamed the phone for refusing after it had only been out of range";
}

// A live LE link is standing proof the phone is present, not a one-off event.
TEST(BearerSupervisor, ClassicBackoffStaysShortWhileLeIsUp) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    ops.classic_succeeds = false;
    ops.classic_error = "br-connection-unknown";
    ops.le = true; // phone answered the solicitation; BR/EDR still will not page

    int64_t now = 0;
    while (now < 1200) {
        sup.tick(now);
        now += BEARER_POLL_SECONDS;
        EXPECT_LE(sup.status().classic_backoff, LE_UP_CLASSIC_BACKOFF_MAX_SECONDS)
            << "backed off for minutes against a phone that was answering on LE";
    }
}

// Telling the user to cycle Bluetooth would drop the LE link carrying notifications.
TEST(BearerSupervisor, DoesNotBlameThePhoneWhileItAnswersOnLe) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    ops.classic_succeeds = false;
    ops.le = true;

    int64_t now = 0;
    while (now < 1200) {
        sup.tick(now);
        now += BEARER_POLL_SECONDS;
    }
    EXPECT_EQ(sup.status().reason.find("keeps refusing"), std::string::npos)
        << "told the user to cycle Bluetooth while notifications were mirroring over LE";
}

// BlueZ reports a Classic link up only while some local profile is connected, so a
// host that offers none refuses Device1.Connect forever while the phone answers OBEX
// perfectly well. Sending that user to their phone wastes their time.
TEST(BearerSupervisor, NamesTheHostWhenObexIsUpAndClassicKeepsRefusing) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    ops.classic_succeeds = false;
    ops.classic_error = "br-connection-unknown";

    int64_t now = 0;
    sup.tick(now, true);
    EXPECT_EQ(sup.status().reason.find("offers the iPhone no profile"), std::string::npos)
        << "named the host on the first refusal, before a transient decline could clear";

    while (now < 1200) {
        now += BEARER_POLL_SECONDS;
        sup.tick(now, true);
    }
    EXPECT_NE(sup.status().reason.find("offers the iPhone no profile"), std::string::npos);
    EXPECT_EQ(sup.status().reason.find("keeps refusing"), std::string::npos)
        << "blamed the phone while it was serving messages and contacts";
}

// A phone-initiated reconnect brings BR/EDR up without hands-free, and nothing
// in BlueZ retries it. One bearer cycle is the remedy, and it must stay one: the
// drop it causes runs back through reset(), which is where a naive flag clears
// and the cycle repeats forever.
TEST(BearerSupervisor, CyclesClassicOnceWhenHandsFreeIsAbsent) {
    FakeBearer ops;
    ops.telephony = false;
    BearerSupervisor sup(ops, true);
    sup.set_calls_enabled(true);

    sup.tick(0); // connects Classic
    sup.tick(1);
    EXPECT_EQ(ops.classic_disconnects, 0) << "cycled before the grace window elapsed";

    sup.tick(1 + HFP_ABSENT_SECONDS - 1);
    EXPECT_EQ(ops.classic_disconnects, 0);

    sup.tick(1 + HFP_ABSENT_SECONDS);
    EXPECT_EQ(ops.classic_disconnects, 1);

    // The link the cycle dropped comes back, still with no hands-free.
    sup.reset();
    for (int64_t now = 1 + HFP_ABSENT_SECONDS; now <= 4 * HFP_ABSENT_SECONDS; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.classic_disconnects, 1) << "hands-free never arrived, so the phone must be asked only once";
}

// Once hands-free is connected the cycle is armed again, so the next reconnect
// that comes up without it is recovered too.
TEST(BearerSupervisor, ArmsAnotherCycleAfterHandsFreeConnects) {
    FakeBearer ops;
    ops.telephony = false;
    BearerSupervisor sup(ops, true);
    sup.set_calls_enabled(true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + HFP_ABSENT_SECONDS);
    EXPECT_EQ(ops.classic_disconnects, 1);

    ops.telephony = true;
    sup.reset();
    sup.tick(100); // reconnects Classic
    sup.tick(101); // Classic up with hands-free on it: arms the next cycle

    ops.telephony = false;
    sup.tick(102);
    sup.tick(102 + HFP_ABSENT_SECONDS);
    EXPECT_EQ(ops.classic_disconnects, 2);
}

// Cycling BR/EDR costs the OBEX sessions on top of it, so it must never happen
// for a feature the user has switched off.
TEST(BearerSupervisor, NeverCyclesClassicWhenCallsAreDisabled) {
    FakeBearer ops;
    ops.telephony = false;
    BearerSupervisor sup(ops, true);

    for (int64_t now = 0; now <= 4 * HFP_ABSENT_SECONDS; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.classic_disconnects, 0);
}

// When PipeWire owns hands-free, a telephony object exists on its bus and the
// link is healthy. Cycling BR/EDR would drop the OBEX sessions to recover
// something that is not missing.
TEST(BearerSupervisor, NeverCyclesClassicWhileHandsFreeIsPresent) {
    FakeBearer ops;
    ops.telephony = true;
    BearerSupervisor sup(ops, true);
    sup.set_calls_enabled(true);

    for (int64_t now = 0; now <= 4 * HFP_ABSENT_SECONDS; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.classic_disconnects, 0);
}

namespace {

    // A phone that is bonded and reachable on neither bearer.
    BearerStatus gone_bearer() {
        BearerStatus s;
        s.device_present = true;
        s.device_paired = true;
        return s;
    }

} // namespace

TEST(AwayLock, LocksOnceTheGraceHasPassedAfterASupervisionTimeout) {
    const BearerStatus gone = gone_bearer();
    EXPECT_TRUE(should_lock_on_away(true, gone, false, REASON_TIMEOUT, 30, 0, 30));
}

TEST(AwayLock, WaitsOutTheGraceBeforeLocking) {
    const BearerStatus gone = gone_bearer();
    EXPECT_FALSE(should_lock_on_away(true, gone, false, REASON_TIMEOUT, 29, 0, 30));
}

// LE flaps on its own while BR/EDR carries on; that is not an absence.
TEST(AwayLock, NeverLocksWhileEitherBearerIsUp) {
    BearerStatus bearer = gone_bearer();
    bearer.classic_connected = true;
    EXPECT_FALSE(should_lock_on_away(true, bearer, false, REASON_TIMEOUT, 300, 0, 30));

    bearer.classic_connected = false;
    bearer.le_connected = true;
    EXPECT_FALSE(should_lock_on_away(true, bearer, false, REASON_TIMEOUT, 300, 0, 30));
}

// An open MAP or PBAP session reaches the phone whatever BlueZ reports.
TEST(AwayLock, NeverLocksWhileAnObexSessionIsOpen) {
    const BearerStatus gone = gone_bearer();
    EXPECT_FALSE(should_lock_on_away(true, gone, true, REASON_TIMEOUT, 300, 0, 30));
}

// Only a supervision timeout means the phone left. Local covers the daemon's own
// disconnects and the adapter powering off, Remote the phone's Bluetooth being
// switched off, Suspend this machine going to sleep.
TEST(AwayLock, LocksOnNoOtherDisconnectReason) {
    const BearerStatus gone = gone_bearer();
    for (const char* reason : {"org.bluez.Reason.Local",
                               "org.bluez.Reason.Remote",
                               "org.bluez.Reason.Suspend",
                               "org.bluez.Reason.Authentication",
                               "org.bluez.Reason.Unknown",
                               ""})
        EXPECT_FALSE(should_lock_on_away(true, gone, false, reason, 300, 0, 30)) << reason;
}

// An unpaired or removed device is not a phone that walked away.
TEST(AwayLock, NeverLocksForADeviceThatIsNoLongerPaired) {
    BearerStatus bearer = gone_bearer();
    bearer.device_paired = false;
    EXPECT_FALSE(should_lock_on_away(true, bearer, false, REASON_TIMEOUT, 300, 0, 30));
}

TEST(AwayLock, NeverLocksWhenTheSettingIsOff) {
    const BearerStatus gone = gone_bearer();
    EXPECT_FALSE(should_lock_on_away(false, gone, false, REASON_TIMEOUT, 300, 0, 30));
}

// Nothing has gone down yet, so there is no absence to time.
TEST(AwayLock, NeverLocksBeforeAnythingHasGoneDown) {
    const BearerStatus gone = gone_bearer();
    EXPECT_FALSE(should_lock_on_away(true, gone, false, REASON_TIMEOUT, 300, -1, 30));
}
