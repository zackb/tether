#pragma once

#include <cstdint>
#include <string>

namespace tether::bluetooth {

    inline constexpr int BEARER_POLL_SECONDS = 5;
    inline constexpr int BEARER_SETTLE_SECONDS = 3;
    inline constexpr int BEARER_BACKOFF_MIN_SECONDS = 5;
    inline constexpr int BEARER_BACKOFF_MAX_SECONDS = 300;

    // Ceiling for the BR/EDR backoff while the phone does not answer pages.
    // Paging an absent phone every 5s can wedge bluetoothd in the kernel's
    // l2cap connect; a minute still finds it soon after it returns without LE.
    inline constexpr int ABSENT_CLASSIC_BACKOFF_MAX_SECONDS = 60;

    // Ceiling for the BR/EDR backoff while LE is connected.
    inline constexpr int LE_UP_CLASSIC_BACKOFF_MAX_SECONDS = 30;

    // How many refused attempts before the status stops saying "connecting"
    inline constexpr int CLASSIC_FAILURES_BEFORE_ADVICE = 6;

    // How long LE may stay down, with the solicitation broadcasting, before the
    // status stops saying "waiting" and names the phone-side Bluetooth cycle.
    // A phone that is answering does so in about a second, so silence this far
    // past that is the iPhone having gone quiet, not a slow connect.
    inline constexpr int LE_SILENT_SECONDS = 180;

    // How long an ANCS session is held across an LE outage before the path is
    // dropped. The bearer flaps faster than a GATT subscription can be rebuilt,
    // so a blip must not reset discovery, while a genuine loss still clears.
    inline constexpr int ANCS_BEARER_GRACE_SECONDS = 30;

    // How long BR/EDR may be up with no hands-free profile before the bearer is
    // cycled once to make BlueZ connect it.
    inline constexpr int HFP_ABSENT_SECONDS = 20;

    // How long an LE link may read connected while the phone offers no ANCS
    // service before the solicitation goes back on air over it. Long enough for
    // GATT discovery on a freshly opened link, which runs to about a minute.
    inline constexpr int ANCS_ABSENT_GRACE_SECONDS = 75;

    // How long every bearer must stay down, after a link supervision timeout,
    // before the phone counts as gone rather than flapping.
    inline constexpr int AWAY_LOCK_GRACE_SECONDS = 30;

    // org.bluez Disconnected reasons.
    inline constexpr const char* REASON_TIMEOUT = "org.bluez.Reason.Timeout";

    // What BlueZ did with a request to bring a bearer up. Accepting a request is
    // not a link: only an observed Connected is that.
    enum class ConnectResult {
        Requested, // BlueZ took the request; a link may or may not follow
        Busy,      // a connect is already running, so do not start another
        Failed,    // refused outright
    };

    // The BlueZ operations the supervisor performs.
    class BearerOps {
    public:
        virtual ~BearerOps() = default;

        virtual bool device_present() const = 0;
        virtual bool device_paired() const = 0;
        virtual bool classic_connected() const = 0;
        virtual bool le_bearer_available() const = 0;
        virtual bool le_connected() const = 0;
        // Device1.PreferredBearer, or empty when BlueZ does not expose it.
        virtual std::string preferred_bearer() const = 0;
        // Writes Device1.PreferredBearer. Never while a connect is in flight:
        // that cancels it as le-connection-abort-by-local.
        virtual void set_preferred_bearer(const std::string& bearer) = 0;

        virtual ConnectResult connect_classic(std::string& err) = 0;
        // Must not block: BlueZ keeps working on a connect after a synchronous
        // call gives up, and every later attempt then collides with it.
        virtual ConnectResult connect_le(std::string& err) = 0;
        // Whether a connect request is still outstanding
        virtual bool le_connect_outstanding() const = 0;
        // Whether the ANCS solicitation is registered AND BlueZ is advertising.
        virtual bool solicitation_on_air() const = 0;

        // Whether BlueZ exports a telephony object for the device.
        virtual bool telephony_present() const = 0;
        // Drops the BR/EDR bearer on its own, leaving LE up. Never Device1.Disconnect().
        virtual bool disconnect_classic(std::string& err) = 0;
    };

    struct BearerStatus {
        bool device_present = false;
        bool device_paired = false;
        bool classic_connected = false;
        bool le_connected = false;
        bool le_available = false;
        bool le_dialling = false;
        int classic_backoff = 0;
        std::string reason;

        bool operator==(const BearerStatus&) const = default;
    };

    // Whether the ANCS solicitation belongs on air. A live LE link normally means
    // the phone has been reached and the advert is not needed, but a link that
    // carries no ANCS is, for notifications, no link at all and the advert is
    // the only thing that asks the phone for the service. `ancs_absent_since` is
    // when the link was first seen up with no ANCS offered, or -1 when it is
    // offered or the link is down. Never solicit over a dial still in flight:
    // that is what aborted the connect once per cycle.
    inline bool
        should_solicit_ancs(bool ancs_enabled, const BearerStatus& bearer, int64_t now, int64_t ancs_absent_since) {
        const bool link_dead = ancs_absent_since >= 0 && now - ancs_absent_since >= ANCS_ABSENT_GRACE_SECONDS;
        return ancs_enabled && bearer.le_available && !bearer.le_dialling && (!bearer.le_connected || link_dead);
    }

    // Whether the phone has been gone long enough, and for the right reason, to
    // lock the session.
    inline bool should_lock_on_away(bool enabled,
                                    const BearerStatus& bearer,
                                    bool obex_up,
                                    const std::string& reason,
                                    int64_t now,
                                    int64_t gone_since,
                                    int grace_seconds) {
        if (!enabled || gone_since < 0 || obex_up)
            return false;
        if (!bearer.device_paired || bearer.classic_connected || bearer.le_connected)
            return false;
        if (reason != REASON_TIMEOUT)
            return false;
        return now - gone_since >= grace_seconds;
    }

    // Keeps the Classic and LE halves of one bond connected.
    //
    // Order is important: BR/EDR first, a settling interval, then LE
    class BearerSupervisor {
    public:
        BearerSupervisor(BearerOps& ops, bool ancs_enabled);

        // Drive once per BEARER_POLL_SECONDS with a monotonic clock. Returns true
        // when the reported status changed, so callers only broadcast on change.
        // `obex_up` is an open MAP or PBAP session, which is proof BR/EDR reaches
        // the phone whatever BlueZ reports about the link.
        bool tick(int64_t now, bool obex_up = false);

        // Clears connection state and backoff after a disconnect or a resume, so
        // recovery starts immediately instead of waiting out a stale backoff.
        void reset();

        void set_ancs_enabled(bool enabled);
        void set_calls_enabled(bool enabled);
        const BearerStatus& status() const { return status_; }

    private:
        BearerOps& ops_;
        bool ancs_enabled_ = true;
        bool calls_enabled_ = false;

        BearerStatus status_;
        int classic_failures_ = 0;
        bool le_dial_spent_ = false;
        bool hfp_cycle_spent_ = false;
        int64_t telephony_absent_since_ = -1;
        int64_t classic_connected_since_ = -1;
        int64_t le_down_since_ = -1;
        // Whether the solicitation has been observed on air since LE went down.
        bool solicited_since_le_down_ = false;
        // Whether the bearer preference has been handed back to LE this Classic session.
        bool bearer_handed_back_ = false;
        int64_t next_classic_attempt_ = 0;
    };

} // namespace tether::bluetooth
