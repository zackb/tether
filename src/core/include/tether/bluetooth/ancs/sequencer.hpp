#pragma once

#include "tether/bluetooth/ancs/parser.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace tether::bluetooth::ancs {

    // A response that never arrives must not wedge the queue forever.
    inline constexpr int REQUEST_TIMEOUT_SECONDS = 15;
    // Bounds how much work a burst of notifications can queue up.
    inline constexpr size_t MAX_QUEUED_REQUESTS = 64;

    struct Request {
        CommandId command = CommandId::GetNotificationAttributes;
        // The Control Point payload, already encoded.
        std::vector<uint8_t> payload;
        // The attribute sequence this request asked for, which is the only way
        // to know where its response ends.
        std::vector<uint8_t> attribute_ids;
        // Echoed back to the caller so a completion can be matched to what it
        // was for without ANCS providing any correlation of its own.
        uint32_t uid = 0;
    };

    // Serializes Control Point requests.
    //
    // ANCS responses carry no request identifier, so with two requests in flight
    // there is no way to tell which response belongs to which — the attributes
    // of one notification would be attached to another. Exactly one request is
    // outstanding at a time, and the rest wait in a queue.
    class ControlPointSequencer {
    public:
        // Returns false when the write could not be issued, which fails the
        // request rather than leaving it silently outstanding.
        using WriteFn = std::function<bool(const std::vector<uint8_t>&)>;
        using CompleteFn = std::function<void(const Request&, const Response&)>;
        using FailFn = std::function<void(const Request&, const std::string& reason)>;

        ControlPointSequencer(WriteFn write, CompleteFn on_complete, FailFn on_fail);

        // Dropped when the queue is full; a caller cannot make the daemon grow
        // without bound by generating notifications.
        bool submit(Request request);

        // Drive with a monotonic clock. Starts the next request when idle and
        // times out one that has gone unanswered.
        void tick(int64_t now);

        // Feeds a Data Source fragment.
        void on_data_source(const uint8_t* data, size_t length, int64_t now);

        // Clears everything after a disconnect: any outstanding response belongs
        // to a session that no longer exists.
        void reset();

        bool in_flight() const { return in_flight_; }
        size_t queued() const { return queue_.size(); }

    private:
        void start_next(int64_t now);

        WriteFn write_;
        CompleteFn on_complete_;
        FailFn on_fail_;

        std::deque<Request> queue_;
        Request current_;
        bool in_flight_ = false;
        int64_t deadline_ = 0;
        DataSourceAssembler assembler_;
    };

    // --- Request builders ---------------------------------------------------

    // Asks for the attributes of one notification. Title, subtitle and message
    // are length-capped; the caller chooses which attributes to request, because
    // asking for content at all is a privacy decision.
    Request build_notification_request(uint32_t uid, const std::vector<NotificationAttributeId>& attributes);

    // Asks for an app's display name.
    Request build_app_request(const std::string& app_id);

    // Performs a notification's positive or negative action. ANCS offers only
    // these two; there is no free-text reply, so no reply affordance can be
    // built on a mirrored notification.
    Request build_action_request(uint32_t uid, ActionId action);

} // namespace tether::bluetooth::ancs
