#include "tether/bluetooth/ancs/parser.hpp"

namespace tether::bluetooth::ancs {

    std::string Response::attribute(uint8_t id) const {
        for (const auto& attribute : attributes) {
            if (attribute.id == id)
                return attribute.value;
        }
        return {};
    }

    bool parse_source_event(const uint8_t* data, size_t length, SourceEvent& out) {
        // exactly eight bytes
        if (!data || length != 8)
            return false;

        if (data[0] > static_cast<uint8_t>(EventId::Removed))
            return false;

        out.event = static_cast<EventId>(data[0]);
        out.flags = data[1];
        out.category = static_cast<CategoryId>(data[2]);
        out.category_count = data[3];
        out.uid = static_cast<uint32_t>(data[4]) | (static_cast<uint32_t>(data[5]) << 8) |
                  (static_cast<uint32_t>(data[6]) << 16) | (static_cast<uint32_t>(data[7]) << 24);
        return true;
    }

    void DataSourceAssembler::expect(CommandId command, std::vector<uint8_t> attribute_ids) {
        expecting_ = true;
        command_ = command;
        attribute_ids_ = std::move(attribute_ids);
        buffer_.clear();
    }

    void DataSourceAssembler::reset() {
        expecting_ = false;
        attribute_ids_.clear();
        buffer_.clear();
    }

    DataSourceAssembler::Result
        DataSourceAssembler::append(const uint8_t* data, size_t length, Response& out, std::string& err) {
        if (!expecting_) {
            err = "Data Source fragment arrived with no request in flight.";
            return Result::Error;
        }
        if (data && length)
            buffer_.insert(buffer_.end(), data, data + length);

        if (buffer_.size() > MAX_RESPONSE_BYTES) {
            err = "Data Source response exceeded the size limit.";
            reset();
            return Result::Error;
        }

        Result result = parse(out, err);
        if (result == Result::Complete) {
            expecting_ = false;
            attribute_ids_.clear();
            buffer_.clear();
        } else if (result == Result::Error) {
            reset();
        }
        return result;
    }

    DataSourceAssembler::Result DataSourceAssembler::parse(Response& out, std::string& err) {
        size_t at = 0;
        const size_t size = buffer_.size();

        if (size < 1)
            return Result::NeedMore;
        if (buffer_[0] != static_cast<uint8_t>(command_)) {
            err = "Data Source response does not echo the requested command.";
            return Result::Error;
        }
        at = 1;

        Response response;
        response.command = command_;

        if (command_ == CommandId::GetNotificationAttributes) {
            if (size < at + 4)
                return Result::NeedMore;
            response.uid = static_cast<uint32_t>(buffer_[at]) | (static_cast<uint32_t>(buffer_[at + 1]) << 8) |
                           (static_cast<uint32_t>(buffer_[at + 2]) << 16) |
                           (static_cast<uint32_t>(buffer_[at + 3]) << 24);
            at += 4;
        } else if (command_ == CommandId::GetAppAttributes) {
            // A NUL-terminated identifier, which may still be arriving.
            size_t nul = at;
            while (nul < size && buffer_[nul] != 0)
                ++nul;
            if (nul >= size)
                return Result::NeedMore;
            response.app_id.assign(buffer_.begin() + static_cast<long>(at), buffer_.begin() + static_cast<long>(nul));
            at = nul + 1;
        } else {
            err = "Data Source response for a command that returns none.";
            return Result::Error;
        }

        // Attributes come back in the order they were requested, so the count is
        // what marks the end of a response that declares no length.
        for (uint8_t expected_id : attribute_ids_) {
            if (size < at + 3)
                return Result::NeedMore;

            const uint8_t id = buffer_[at];
            const uint16_t declared =
                static_cast<uint16_t>(buffer_[at + 1]) | static_cast<uint16_t>(buffer_[at + 2] << 8);
            at += 3;

            if (id != expected_id) {
                err = "Data Source returned attributes out of order.";
                return Result::Error;
            }
            if (declared > MAX_ATTRIBUTE_LENGTH) {
                err = "Data Source attribute claimed an implausible length.";
                return Result::Error;
            }
            if (size < at + declared)
                return Result::NeedMore;

            Attribute attribute;
            attribute.id = id;
            attribute.value.assign(buffer_.begin() + static_cast<long>(at),
                                   buffer_.begin() + static_cast<long>(at + declared));
            response.attributes.push_back(std::move(attribute));
            at += declared;
        }

        if (at != size) {
            err = "Data Source response had unexpected trailing bytes.";
            return Result::Error;
        }

        out = std::move(response);
        return Result::Complete;
    }

} // namespace tether::bluetooth::ancs
