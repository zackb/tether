#pragma once

#include <cstdint>
#include <string>

namespace tether::bluetooth {

    inline constexpr int BEARER_POLL_SECONDS = 5;
    inline constexpr int BEARER_SETTLE_SECONDS = 3;
    inline constexpr int BEARER_BACKOFF_MIN_SECONDS = 5;
    inline constexpr int BEARER_BACKOFF_MAX_SECONDS = 300;

    // The BlueZ operations the supervisor performs.
    class BearerOps {
    public:
        virtual ~BearerOps() = default;

        virtual bool device_present() const = 0;
        virtual bool device_paired() const = 0;
        virtual bool classic_connected() const = 0;
        virtual bool le_bearer_available() const = 0;
        virtual bool le_connected() const = 0;

        virtual void set_preferred_bearer(const std::string& bearer) = 0;

        virtual bool connect_classic(std::string& err) = 0;
        virtual bool connect_le(std::string& err) = 0;
    };

    struct BearerStatus {
        bool device_present = false;
        bool device_paired = false;
        bool classic_connected = false;
        bool le_connected = false;
        bool le_available = false;
        int classic_backoff = 0;
        int le_backoff = 0;
        std::string reason;

        bool operator==(const BearerStatus&) const = default;
    };

    // Keeps the Classic and LE halves of one bond connected.
    //
    // Order is important: BR/EDR first, a settling interval, then LE
    class BearerSupervisor {
    public:
        BearerSupervisor(BearerOps& ops, bool ancs_enabled);

        // Drive once per BEARER_POLL_SECONDS with a monotonic clock. Returns true
        // when the reported status changed, so callers only broadcast on change.
        bool tick(int64_t now);

        // Clears connection state and backoff after a disconnect or a resume, so
        // recovery starts immediately instead of waiting out a stale backoff.
        void reset();

        void set_ancs_enabled(bool enabled);
        const BearerStatus& status() const { return status_; }

    private:
        BearerOps& ops_;
        bool ancs_enabled_ = true;

        BearerStatus status_;
        int64_t classic_connected_since_ = -1;
        int64_t next_classic_attempt_ = 0;
        int64_t next_le_attempt_ = 0;
    };

} // namespace tether::bluetooth
