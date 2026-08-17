#pragma once

#include "tether/bluetooth/ancs/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tether::bluetooth::ancs {

    // No attribute we ask for can legitimately be this large; anything claiming
    // more is malformed or hostile and the response is dropped.
    inline constexpr uint16_t MAX_ATTRIBUTE_LENGTH = 1024;
    // Hard ceiling on one reassembled response, so a peer cannot make us buffer
    // without bound by never completing one.
    inline constexpr size_t MAX_RESPONSE_BYTES = 4096;

    // Parses one Notification Source packet. Returns false unless the packet is
    // exactly the eight bytes the specification defines — a short read here
    // would otherwise be interpreted as a different notification.
    bool parse_source_event(const uint8_t* data, size_t length, SourceEvent& out);

    // Reassembles a Data Source response.
    //
    // Data Source responses carry no total length and arrive split across GATT
    // notifications, so the only way to know a response has ended is to know
    // exactly what was asked for: the echoed command byte plus the attribute
    // sequence of the request in flight. That is why the sequencer allows only
    // one request at a time.
    class DataSourceAssembler {
    public:
        enum class Result { NeedMore, Complete, Error };

        // Declares the request whose response is now expected. Resets any
        // partial buffer, since a new request invalidates it.
        void expect(CommandId command, std::vector<uint8_t> attribute_ids);
        void reset();
        bool expecting() const { return expecting_; }

        // Feeds one fragment. On Complete, `out` holds the response and the
        // assembler is ready for the next request. On Error the buffer is
        // dropped: a response that cannot be trusted must not be half-applied.
        Result append(const uint8_t* data, size_t length, Response& out, std::string& err);

    private:
        Result parse(Response& out, std::string& err);

        bool expecting_ = false;
        CommandId command_ = CommandId::GetNotificationAttributes;
        std::vector<uint8_t> attribute_ids_;
        std::vector<uint8_t> buffer_;
    };

} // namespace tether::bluetooth::ancs
