#include "tether/bluetooth/bearer_supervisor.hpp"
#include "tether/log.hpp"
#include <tether/i18n.hpp>

#include <algorithm>

namespace tether::bluetooth {

    namespace {

        int grow_backoff(int current, int ceiling = BEARER_BACKOFF_MAX_SECONDS) {
            if (current <= 0)
                return BEARER_BACKOFF_MIN_SECONDS;
            return std::min(current * 2, ceiling);
        }

        // Failures that clear on their own, unlike a refusal from the phone.
        bool is_transient(const std::string& err) {
            static const char* const transient[] = {
                "org.bluez.Error.NotReady",
                "connection-abort",
                "Software caused connection abort",
                "br-connection-canceled",
                "le-connection-abort-by-local",
                "br-connection-refused",
            };
            for (const char* needle : transient) {
                if (err.find(needle) != std::string::npos)
                    return true;
            }
            return false;
        }

        // BlueZ is already working on a connect, or the link is already up.
        // Neither is a failure, and retrying at the minimum backoff is what turns
        // one stalled connect into a permanent InProgress storm.
        bool is_already_running(const std::string& err) {
            return err.find("InProgress") != std::string::npos ||
                   err.find("already in progress") != std::string::npos ||
                   err.find("AlreadyConnected") != std::string::npos ||
                   err.find("already-connected") != std::string::npos;
        }

        // Collapses an error a synchronous caller reported into the same shape an
        // asynchronous one returns, so both take one code path.
        ConnectResult classify(ConnectResult reported, const std::string& err) {
            return reported == ConnectResult::Failed && is_already_running(err) ? ConnectResult::Busy : reported;
        }

        // The phone did not answer the page: out of range or Bluetooth off.
        bool is_absent(const std::string& err) {
            static const char* const absent[] = {
                "page-timeout",
                "page timeout",
                "Host is down",
                "Connection timed out",
            };
            for (const char* needle : absent) {
                if (err.find(needle) != std::string::npos)
                    return true;
            }
            return false;
        }

        int next_backoff(int current, const std::string& err, int ceiling = BEARER_BACKOFF_MAX_SECONDS) {
            if (is_transient(err))
                return BEARER_BACKOFF_MIN_SECONDS;
            if (is_absent(err))
                return grow_backoff(current, std::min(ceiling, ABSENT_CLASSIC_BACKOFF_MAX_SECONDS));
            return grow_backoff(current, ceiling);
        }

        // Marked here, translated where it is read: gettext cannot run at namespace scope.
        constexpr const char* NO_LOCAL_PROFILE_ADVICE =
            N_("Messages and contacts are connected. BlueZ reports the Bluetooth link down because this computer "
               "offers the iPhone no profile to connect to — usual on a machine with no audio stack, or one where "
               "the a2dp_sink and hfp_hf roles are turned off. Nothing is missing and nothing needs changing on "
               "the iPhone.");

        constexpr const char* LE_NEVER_SOLICITED_ADVICE =
            N_("This computer is not putting the notification request on air, so the iPhone is never asked for "
               "the service. Nothing on the iPhone changes this and re-pairing will not either. Check the "
               "controller with tether --bt-status; another one can be selected with tether --bt-adapter.");

        constexpr const char* LE_BOND_PINNED_ADVICE =
            N_("This computer had pinned the bond to BR/EDR, which refuses the LE link the iPhone opens to carry "
               "notifications. That is being undone now; LE should come up within a minute. Nothing on the iPhone "
               "changes this and re-pairing will not either.");

        constexpr const char* LE_PHONE_SILENT_ADVICE =
            N_("The iPhone is not answering on LE. Its Bluetooth is wedged on its own side: turn Bluetooth off "
               "and back on on the iPhone, which clears this even when Share System Notifications is already on. "
               "Messages and contacts are unaffected.");

    } // namespace

    BearerSupervisor::BearerSupervisor(BearerOps& ops, bool ancs_enabled) : ops_(ops), ancs_enabled_(ancs_enabled) {}

    void BearerSupervisor::set_ancs_enabled(bool enabled) { ancs_enabled_ = enabled; }

    void BearerSupervisor::set_calls_enabled(bool enabled) { calls_enabled_ = enabled; }

    void BearerSupervisor::reset() {
        // hfp_cycle_spent_ deliberately survives
        classic_failures_ = 0;
        telephony_absent_since_ = -1;
        le_down_since_ = -1;
        solicited_since_le_down_ = false;
        classic_connected_since_ = -1;
        bearer_handed_back_ = false;
        next_classic_attempt_ = 0;
        status_.classic_backoff = 0;
        status_.le_dialling = ops_.le_connect_outstanding();
    }

    bool BearerSupervisor::tick(int64_t now, bool obex_up) {
        BearerStatus previous = status_;

        status_.device_present = ops_.device_present();
        status_.device_paired = status_.device_present && ops_.device_paired();
        if (!status_.device_present || !status_.device_paired) {
            status_.classic_connected = false;
            status_.le_connected = false;
            status_.le_available = false;
            status_.le_dialling = ops_.le_connect_outstanding();
            classic_connected_since_ = -1;
            status_.reason = status_.device_present ? "iPhone is not paired." : "iPhone not known to BlueZ.";
            return !(status_ == previous);
        }

        status_.classic_connected = ops_.classic_connected();
        status_.le_available = ops_.le_bearer_available();
        status_.le_connected = status_.le_available && ops_.le_connected();
        if (status_.le_connected) {
            le_down_since_ = -1;
            solicited_since_le_down_ = false;
        } else if (le_down_since_ < 0) {
            le_down_since_ = now;
        }
        if (!status_.le_connected && ops_.solicitation_on_air())
            solicited_since_le_down_ = true;

        if (status_.le_connected)
            status_.le_dialling = false;

        // The phone opening the LE link proves it is in range and answering
        if (status_.le_connected && !previous.le_connected) {
            status_.classic_backoff = 0;
            next_classic_attempt_ = 0;
            classic_failures_ = 0;
        }

        if (status_.classic_connected) {
            if (classic_connected_since_ < 0)
                classic_connected_since_ = now;
            status_.classic_backoff = 0;
            classic_failures_ = 0;
        } else {
            // A dropped link invalidates the settle window, so no LE attempt is
            // made until BR/EDR is back and has settled. An LE link that is
            // already up is a separate matter and is reported as it is: ANCS
            // rides LE alone, so calling it down here would switch notification
            // mirroring off for a phone that is still delivering notifications.
            classic_connected_since_ = -1;
            bearer_handed_back_ = false;

            const int ceiling = status_.le_connected ? LE_UP_CLASSIC_BACKOFF_MAX_SECONDS : BEARER_BACKOFF_MAX_SECONDS;

            if (now >= next_classic_attempt_) {
                std::string err;
                switch (classify(ops_.connect_classic(err), err)) {
                case ConnectResult::Requested:
                    status_.classic_backoff = 0;
                    next_classic_attempt_ = 0;
                    classic_failures_ = 0;
                    break;
                case ConnectResult::Busy:
                    // Not a refusal and not an attempt. Standing back is the
                    // whole remedy.
                    status_.classic_backoff = grow_backoff(status_.classic_backoff, ceiling);
                    next_classic_attempt_ = now + status_.classic_backoff;
                    break;
                case ConnectResult::Failed:
                    ++classic_failures_;
                    status_.classic_backoff = next_backoff(status_.classic_backoff, err, ceiling);
                    next_classic_attempt_ = now + status_.classic_backoff;
                    debug::log(
                        WARN, "bluetooth: BR/EDR connect failed ({}), retrying in {}s", err, status_.classic_backoff);
                    break;
                }
            }

            status_.le_dialling = ops_.le_connect_outstanding();

            // An iPhone that keeps refusing while unlocked, with its Bluetooth
            // permissions already on, is wedged on its own side: nothing here
            // clears it, and saying "connecting" forever hides that. A host with
            // no connectable BR/EDR profile refuses identically, so an OBEX
            // session the phone is serving right now rules the phone out: after
            // this many consecutive refusals the transient decline is over, and
            // what is left is this side having nothing to connect.
            const bool refused = classic_failures_ >= CLASSIC_FAILURES_BEFORE_ADVICE;
            status_.reason = refused && obex_up ? _(NO_LOCAL_PROFILE_ADVICE)
                             : refused && !status_.le_connected
                                 ? _("The iPhone keeps refusing the Bluetooth connection. Turn Bluetooth off and "
                                     "back on on the iPhone — that clears it even when the permissions are already "
                                     "on. Re-pairing is not the fix.")
                             : status_.le_connected
                                 ? _("Notifications are connected. Still reaching the iPhone for messages and "
                                     "contacts...")
                                 : _("Connecting to the iPhone over Bluetooth...");
            return !(status_ == previous);
        }

        // Classic is up.
        if (calls_enabled_ && !ops_.telephony_present()) {
            if (telephony_absent_since_ < 0)
                telephony_absent_since_ = now;
            if (!hfp_cycle_spent_ && now - telephony_absent_since_ >= HFP_ABSENT_SECONDS) {
                hfp_cycle_spent_ = true;
                std::string err;
                if (!ops_.disconnect_classic(err))
                    debug::log(WARN, "bluetooth: BR/EDR cycle for hands-free failed ({})", err);
            }
        } else {
            telephony_absent_since_ = -1;
            hfp_cycle_spent_ = false;
        }

        // LE is only worth attempting once the ACL has settled.
        if (!ancs_enabled_) {
            status_.reason = _("Connected. Notification mirroring is disabled.");
            return !(status_ == previous);
        }

        if (!status_.le_available) {
            status_.reason = _("Connected. BlueZ is not exposing an LE bearer, so notifications are unavailable.");
            return !(status_ == previous);
        }

        if (!status_.le_connected) {
            // solicitation owns the radio; the dial gets one shot per daemon and then stands aside.
            const bool settled =
                classic_connected_since_ >= 0 && now - classic_connected_since_ >= BEARER_SETTLE_SECONDS;

            const bool pinned = ops_.preferred_bearer() == "bredr";
            if (pinned && settled && !bearer_handed_back_ && !ops_.le_connect_outstanding()) {
                ops_.set_preferred_bearer("le");
                bearer_handed_back_ = true;
                debug::log(INFO, "bluetooth: bond was pinned to BR/EDR; preference handed back to LE");
            }

            if (!le_dial_spent_ && settled && !ops_.le_connect_outstanding()) {
                std::string err;
                const ConnectResult result = classify(ops_.connect_le(err), err);
                le_dial_spent_ = true;
                if (result == ConnectResult::Failed)
                    debug::log(WARN, "bluetooth: LE connect failed ({}), leaving it to the solicitation", err);
            }

            // read after the attempt, never before
            status_.le_dialling = ops_.le_connect_outstanding();

            const bool le_silent = le_down_since_ >= 0 && now - le_down_since_ >= LE_SILENT_SECONDS;

            status_.reason = le_silent && !solicited_since_le_down_ ? _(LE_NEVER_SOLICITED_ADVICE)
                             : le_silent && pinned                  ? _(LE_BOND_PINNED_ADVICE)
                             : le_silent ? _(LE_PHONE_SILENT_ADVICE)
                                         : _("Connected. Waiting for the iPhone to open the LE link that carries "
                                             "notifications. If it does not, open Settings > Bluetooth > (i) on the "
                                             "iPhone and check Share System Notifications.");
            return !(status_ == previous);
        }

        status_.reason = _("Connected over BR/EDR and LE.");
        return !(status_ == previous);
    }

} // namespace tether::bluetooth
