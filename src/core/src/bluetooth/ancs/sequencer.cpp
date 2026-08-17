#include "tether/bluetooth/ancs/sequencer.hpp"

namespace tether::bluetooth::ancs {

    namespace {

        void append_u32_le(std::vector<uint8_t>& out, uint32_t value) {
            out.push_back(static_cast<uint8_t>(value & 0xff));
            out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
            out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
            out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
        }

        void append_u16_le(std::vector<uint8_t>& out, uint16_t value) {
            out.push_back(static_cast<uint8_t>(value & 0xff));
            out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
        }

        uint16_t cap_for(NotificationAttributeId id) {
            return id == NotificationAttributeId::Message ? MAX_BODY_LENGTH : MAX_TITLE_LENGTH;
        }

    } // namespace

    ControlPointSequencer::ControlPointSequencer(WriteFn write, CompleteFn on_complete, FailFn on_fail)
        : write_(std::move(write)), on_complete_(std::move(on_complete)), on_fail_(std::move(on_fail)) {}

    bool ControlPointSequencer::submit(Request request) {
        if (queue_.size() >= MAX_QUEUED_REQUESTS)
            return false;
        queue_.push_back(std::move(request));
        return true;
    }

    void ControlPointSequencer::start_next(int64_t now) {
        while (!in_flight_ && !queue_.empty()) {
            current_ = std::move(queue_.front());
            queue_.pop_front();

            const bool expects_response = !current_.attribute_ids.empty();
            if (expects_response) {
                assembler_.expect(current_.command, current_.attribute_ids);
                in_flight_ = true;
                deadline_ = now + REQUEST_TIMEOUT_SECONDS;
            }

            if (!write_ || !write_(current_.payload)) {
                in_flight_ = false;
                assembler_.reset();
                if (on_fail_)
                    on_fail_(current_, "Could not write to the ANCS control point.");
                continue;
            }

            if (expects_response)
                return;

            if (on_complete_) {
                Response acknowledged;
                acknowledged.command = current_.command;
                acknowledged.uid = current_.uid;
                on_complete_(current_, acknowledged);
            }
        }
    }

    void ControlPointSequencer::tick(int64_t now) {
        if (in_flight_ && now >= deadline_) {
            Request timed_out = current_;
            in_flight_ = false;
            assembler_.reset();
            if (on_fail_)
                on_fail_(timed_out, "The phone did not answer within the timeout.");
        }
        start_next(now);
    }

    void ControlPointSequencer::on_data_source(const uint8_t* data, size_t length, int64_t now) {
        if (!in_flight_)
            return;

        Response response;
        std::string err;
        switch (assembler_.append(data, length, response, err)) {
        case DataSourceAssembler::Result::NeedMore:
            return;
        case DataSourceAssembler::Result::Complete: {
            Request done = current_;
            in_flight_ = false;
            if (on_complete_)
                on_complete_(done, response);
            break;
        }
        case DataSourceAssembler::Result::Error: {
            Request failed = current_;
            in_flight_ = false;
            if (on_fail_)
                on_fail_(failed, err);
            break;
        }
        }
        start_next(now);
    }

    void ControlPointSequencer::reset() {
        queue_.clear();
        in_flight_ = false;
        assembler_.reset();
    }

    Request build_notification_request(uint32_t uid, const std::vector<NotificationAttributeId>& attributes) {
        Request request;
        request.command = CommandId::GetNotificationAttributes;
        request.uid = uid;

        request.payload.push_back(static_cast<uint8_t>(CommandId::GetNotificationAttributes));
        append_u32_le(request.payload, uid);
        for (NotificationAttributeId id : attributes) {
            request.payload.push_back(static_cast<uint8_t>(id));
            // Only the three text attributes take a length, and for them it is
            // mandatory; sending one for the others corrupts the request.
            if (attribute_takes_length(id))
                append_u16_le(request.payload, cap_for(id));
            request.attribute_ids.push_back(static_cast<uint8_t>(id));
        }
        return request;
    }

    Request build_app_request(const std::string& app_id) {
        Request request;
        request.command = CommandId::GetAppAttributes;

        request.payload.push_back(static_cast<uint8_t>(CommandId::GetAppAttributes));
        request.payload.insert(request.payload.end(), app_id.begin(), app_id.end());
        request.payload.push_back(0);
        request.payload.push_back(static_cast<uint8_t>(AppAttributeId::DisplayName));
        request.attribute_ids.push_back(static_cast<uint8_t>(AppAttributeId::DisplayName));
        return request;
    }

    Request build_action_request(uint32_t uid, ActionId action) {
        Request request;
        request.command = CommandId::PerformNotificationAction;
        request.uid = uid;

        request.payload.push_back(static_cast<uint8_t>(CommandId::PerformNotificationAction));
        append_u32_le(request.payload, uid);
        request.payload.push_back(static_cast<uint8_t>(action));

        return request;
    }

} // namespace tether::bluetooth::ancs
