#include "tether/bluetooth/bearer_supervisor.hpp"
#include "tether/log.hpp"

#include <algorithm>

namespace tether::bluetooth {

    namespace {

        int grow_backoff(int current) {
            if (current <= 0)
                return BEARER_BACKOFF_MIN_SECONDS;
            return std::min(current * 2, BEARER_BACKOFF_MAX_SECONDS);
        }

        // BlueZ answers InProgress while one of its own connect operations is
        // still pending on the device. That clears on its own, unlike a refusal
        // from the phone, so backing off for minutes leaves the link down long
        // after the reason for it is gone.
        bool is_transient(const std::string& err) {
            return err.find("org.bluez.Error.InProgress") != std::string::npos;
        }

        int next_backoff(int current, const std::string& err) {
            return is_transient(err) ? BEARER_BACKOFF_MIN_SECONDS : grow_backoff(current);
        }

    } // namespace

    BearerSupervisor::BearerSupervisor(BearerOps& ops, bool ancs_enabled) : ops_(ops), ancs_enabled_(ancs_enabled) {}

    void BearerSupervisor::set_ancs_enabled(bool enabled) { ancs_enabled_ = enabled; }

    void BearerSupervisor::reset() {
        classic_connected_since_ = -1;
        next_classic_attempt_ = 0;
        next_le_attempt_ = 0;
        status_.classic_backoff = 0;
        status_.le_backoff = 0;
    }

    bool BearerSupervisor::tick(int64_t now) {
        BearerStatus previous = status_;

        status_.device_present = ops_.device_present();
        status_.device_paired = status_.device_present && ops_.device_paired();
        if (!status_.device_present || !status_.device_paired) {
            status_.classic_connected = false;
            status_.le_connected = false;
            status_.le_available = false;
            classic_connected_since_ = -1;
            status_.reason = status_.device_present ? "iPhone is not paired." : "iPhone not known to BlueZ.";
            return !(status_ == previous);
        }

        status_.classic_connected = ops_.classic_connected();
        status_.le_available = ops_.le_bearer_available();
        status_.le_connected = status_.le_available && ops_.le_connected();

        if (status_.classic_connected) {
            if (classic_connected_since_ < 0)
                classic_connected_since_ = now;
            status_.classic_backoff = 0;
        } else {
            // A dropped link invalidates the settle window and any LE state that
            // depended on it.
            classic_connected_since_ = -1;
            status_.le_connected = false;

            if (now >= next_classic_attempt_) {
                ops_.set_preferred_bearer("bredr");
                std::string err;
                if (ops_.connect_classic(err)) {
                    status_.classic_backoff = 0;
                    next_classic_attempt_ = 0;
                } else {
                    status_.classic_backoff = next_backoff(status_.classic_backoff, err);
                    next_classic_attempt_ = now + status_.classic_backoff;
                    debug::log(
                        WARN, "bluetooth: BR/EDR connect failed ({}), retrying in {}s", err, status_.classic_backoff);
                }
            }
            status_.reason = "Connecting to the iPhone over Bluetooth...";
            return !(status_ == previous);
        }

        // Classic is up. LE is only worth attempting once the ACL has settled.
        if (!ancs_enabled_) {
            status_.reason = "Connected. Notification mirroring is disabled.";
            return !(status_ == previous);
        }

        if (!status_.le_available) {
            status_.reason = "Connected. BlueZ is not exposing an LE bearer, so notifications are unavailable.";
            return !(status_ == previous);
        }

        if (!status_.le_connected) {
            const bool settled =
                classic_connected_since_ >= 0 && now - classic_connected_since_ >= BEARER_SETTLE_SECONDS;
            if (settled && now >= next_le_attempt_) {
                // Select LE only for this attempt, then hand the preference back
                // so LE is never left preferred while idle.
                ops_.set_preferred_bearer("le");
                std::string err;
                const bool ok = ops_.connect_le(err);
                ops_.set_preferred_bearer("bredr");

                if (ok) {
                    status_.le_backoff = 0;
                    next_le_attempt_ = 0;
                    status_.le_connected = true;
                } else {
                    status_.le_backoff = next_backoff(status_.le_backoff, err);
                    next_le_attempt_ = now + status_.le_backoff;
                    debug::log(WARN, "bluetooth: LE connect failed ({}), retrying in {}s", err, status_.le_backoff);
                }
            }
            if (!status_.le_connected) {
                status_.reason = "Connected. Bringing up the LE link for notifications...";
                return !(status_ == previous);
            }
        }

        status_.reason = "Connected over BR/EDR and LE.";
        return !(status_ == previous);
    }

} // namespace tether::bluetooth
