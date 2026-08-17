#include <gtest/gtest.h>
#include <tether/bluetooth/ancs/parser.hpp>

using namespace tether::bluetooth::ancs;

namespace {

    std::vector<uint8_t> source_packet(uint8_t event, uint8_t flags, uint8_t category, uint8_t count, uint32_t uid) {
        return {event,
                flags,
                category,
                count,
                static_cast<uint8_t>(uid & 0xff),
                static_cast<uint8_t>((uid >> 8) & 0xff),
                static_cast<uint8_t>((uid >> 16) & 0xff),
                static_cast<uint8_t>((uid >> 24) & 0xff)};
    }

    void append_attribute(std::vector<uint8_t>& out, uint8_t id, const std::string& value) {
        out.push_back(id);
        out.push_back(static_cast<uint8_t>(value.size() & 0xff));
        out.push_back(static_cast<uint8_t>((value.size() >> 8) & 0xff));
        out.insert(out.end(), value.begin(), value.end());
    }

    std::vector<uint8_t> notification_response(uint32_t uid) {
        std::vector<uint8_t> out{0x00,
                                 static_cast<uint8_t>(uid & 0xff),
                                 static_cast<uint8_t>((uid >> 8) & 0xff),
                                 static_cast<uint8_t>((uid >> 16) & 0xff),
                                 static_cast<uint8_t>((uid >> 24) & 0xff)};
        append_attribute(out, 0, "com.example.app");
        append_attribute(out, 1, "Title here");
        return out;
    }

    DataSourceAssembler::Result feed(DataSourceAssembler& assembler,
                                     const std::vector<uint8_t>& bytes,
                                     Response& out,
                                     std::string& err) {
        return assembler.append(bytes.data(), bytes.size(), out, err);
    }

} // namespace

// --- Notification Source -------------------------------------------------

TEST(AncsParser, ParsesASourceEvent) {
    SourceEvent event;
    auto packet = source_packet(0, FlagImportant | FlagPositiveAction, 4, 2, 0xDEADBEEF);

    ASSERT_TRUE(parse_source_event(packet.data(), packet.size(), event));
    EXPECT_EQ(event.event, EventId::Added);
    EXPECT_EQ(event.category, CategoryId::Social);
    EXPECT_EQ(event.category_count, 2);
    EXPECT_EQ(event.uid, 0xDEADBEEFu);
    EXPECT_TRUE(event.has_positive_action());
    EXPECT_FALSE(event.pre_existing());
}

// A truncated packet would leave fields to be invented, and the UID is what
// every later request is keyed on — so anything but eight bytes is refused.
TEST(AncsParser, RejectsSourcePacketsOfTheWrongLength) {
    SourceEvent event;
    auto packet = source_packet(0, 0, 0, 0, 1);

    EXPECT_FALSE(parse_source_event(packet.data(), 7, event));
    EXPECT_FALSE(parse_source_event(packet.data(), 9, event));
    EXPECT_FALSE(parse_source_event(packet.data(), 0, event));
    EXPECT_FALSE(parse_source_event(nullptr, 8, event));
}

TEST(AncsParser, RejectsUnknownEventIds) {
    SourceEvent event;
    auto packet = source_packet(9, 0, 0, 0, 1);
    EXPECT_FALSE(parse_source_event(packet.data(), packet.size(), event));
}

// --- Data Source reassembly ----------------------------------------------

TEST(AncsParser, ReassemblesACompleteResponse) {
    DataSourceAssembler assembler;
    assembler.expect(CommandId::GetNotificationAttributes, {0, 1});

    Response response;
    std::string err;
    ASSERT_EQ(feed(assembler, notification_response(42), response, err), DataSourceAssembler::Result::Complete) << err;
    EXPECT_EQ(response.uid, 42u);
    EXPECT_EQ(response.attribute(0), "com.example.app");
    EXPECT_EQ(response.attribute(1), "Title here");
    EXPECT_FALSE(assembler.expecting());
}

// Responses have no total length and arrive split across GATT notifications, so
// completion can only be recognized from the requested attribute sequence.
TEST(AncsParser, ReassemblesAcrossArbitraryFragmentBoundaries) {
    const auto full = notification_response(7);

    for (size_t split = 1; split < full.size(); ++split) {
        DataSourceAssembler assembler;
        assembler.expect(CommandId::GetNotificationAttributes, {0, 1});

        Response response;
        std::string err;
        std::vector<uint8_t> first(full.begin(), full.begin() + static_cast<long>(split));
        std::vector<uint8_t> second(full.begin() + static_cast<long>(split), full.end());

        ASSERT_EQ(feed(assembler, first, response, err), DataSourceAssembler::Result::NeedMore)
            << "split at " << split << ": " << err;
        ASSERT_EQ(feed(assembler, second, response, err), DataSourceAssembler::Result::Complete)
            << "split at " << split << ": " << err;
        EXPECT_EQ(response.attribute(1), "Title here");
    }
}

TEST(AncsParser, ReassemblesOneByteAtATime) {
    const auto full = notification_response(9);
    DataSourceAssembler assembler;
    assembler.expect(CommandId::GetNotificationAttributes, {0, 1});

    Response response;
    std::string err;
    for (size_t i = 0; i + 1 < full.size(); ++i)
        ASSERT_EQ(assembler.append(&full[i], 1, response, err), DataSourceAssembler::Result::NeedMore) << i;
    EXPECT_EQ(assembler.append(&full.back(), 1, response, err), DataSourceAssembler::Result::Complete) << err;
}

TEST(AncsParser, ParsesAppAttributeResponses) {
    DataSourceAssembler assembler;
    assembler.expect(CommandId::GetAppAttributes, {0});

    std::vector<uint8_t> bytes{0x01};
    const std::string app = "com.apple.MobileSMS";
    bytes.insert(bytes.end(), app.begin(), app.end());
    bytes.push_back(0);
    append_attribute(bytes, 0, "Messages");

    Response response;
    std::string err;
    ASSERT_EQ(feed(assembler, bytes, response, err), DataSourceAssembler::Result::Complete) << err;
    EXPECT_EQ(response.app_id, "com.apple.MobileSMS");
    EXPECT_EQ(response.attribute(0), "Messages");
}

TEST(AncsParser, WaitsForAnUnterminatedAppIdentifier) {
    DataSourceAssembler assembler;
    assembler.expect(CommandId::GetAppAttributes, {0});

    std::vector<uint8_t> bytes{0x01, 'c', 'o', 'm'};
    Response response;
    std::string err;
    EXPECT_EQ(feed(assembler, bytes, response, err), DataSourceAssembler::Result::NeedMore);
}

// --- Hostile input -------------------------------------------------------

TEST(AncsParser, RejectsAResponseThatDoesNotEchoTheCommand) {
    DataSourceAssembler assembler;
    assembler.expect(CommandId::GetNotificationAttributes, {0});

    std::vector<uint8_t> bytes{0x07, 0, 0, 0, 0};
    Response response;
    std::string err;
    EXPECT_EQ(feed(assembler, bytes, response, err), DataSourceAssembler::Result::Error);
    EXPECT_FALSE(err.empty());
}

TEST(AncsParser, RejectsAttributesReturnedOutOfOrder) {
    DataSourceAssembler assembler;
    assembler.expect(CommandId::GetNotificationAttributes, {0, 1});

    std::vector<uint8_t> bytes{0x00, 1, 0, 0, 0};
    append_attribute(bytes, 3, "wrong attribute");

    Response response;
    std::string err;
    EXPECT_EQ(feed(assembler, bytes, response, err), DataSourceAssembler::Result::Error);
}

// A length field is two bytes and can claim far more than we ever ask for.
// Believing it would let a peer make us buffer for as long as it likes.
TEST(AncsParser, RejectsAnImplausibleAttributeLength) {
    DataSourceAssembler assembler;
    assembler.expect(CommandId::GetNotificationAttributes, {0});

    std::vector<uint8_t> bytes{0x00, 1, 0, 0, 0, 0, 0xff, 0xff};
    Response response;
    std::string err;
    EXPECT_EQ(feed(assembler, bytes, response, err), DataSourceAssembler::Result::Error);
}

TEST(AncsParser, RejectsTrailingBytesAfterACompleteResponse) {
    DataSourceAssembler assembler;
    assembler.expect(CommandId::GetNotificationAttributes, {0, 1});

    auto bytes = notification_response(3);
    bytes.push_back(0xAA);
    bytes.push_back(0xBB);

    Response response;
    std::string err;
    EXPECT_EQ(feed(assembler, bytes, response, err), DataSourceAssembler::Result::Error);
}

TEST(AncsParser, RejectsAResponseThatGrowsWithoutEnding) {
    DataSourceAssembler assembler;
    assembler.expect(CommandId::GetNotificationAttributes, {0, 1, 2, 3});

    Response response;
    std::string err;
    std::vector<uint8_t> flood(1024, 0);
    flood[0] = 0x00;

    DataSourceAssembler::Result result = DataSourceAssembler::Result::NeedMore;
    for (int i = 0; i < 16 && result == DataSourceAssembler::Result::NeedMore; ++i)
        result = feed(assembler, flood, response, err);

    EXPECT_EQ(result, DataSourceAssembler::Result::Error) << "an endless response must be cut off";
}

TEST(AncsParser, RefusesFragmentsWithNoRequestInFlight) {
    DataSourceAssembler assembler;
    Response response;
    std::string err;

    auto bytes = notification_response(1);
    EXPECT_EQ(feed(assembler, bytes, response, err), DataSourceAssembler::Result::Error);
}

// A rejected response must not leave half-parsed state behind for the next one
// to inherit.
TEST(AncsParser, RecoversAfterAnError) {
    DataSourceAssembler assembler;
    assembler.expect(CommandId::GetNotificationAttributes, {0, 1});

    Response response;
    std::string err;
    std::vector<uint8_t> garbage{0x09, 0x09};
    ASSERT_EQ(feed(assembler, garbage, response, err), DataSourceAssembler::Result::Error);
    EXPECT_FALSE(assembler.expecting());

    assembler.expect(CommandId::GetNotificationAttributes, {0, 1});
    EXPECT_EQ(feed(assembler, notification_response(5), response, err), DataSourceAssembler::Result::Complete) << err;
    EXPECT_EQ(response.uid, 5u);
}

TEST(AncsParser, HandlesEmptyAttributeValues) {
    DataSourceAssembler assembler;
    assembler.expect(CommandId::GetNotificationAttributes, {0, 1});

    std::vector<uint8_t> bytes{0x00, 4, 0, 0, 0};
    append_attribute(bytes, 0, "com.example.app");
    append_attribute(bytes, 1, "");

    Response response;
    std::string err;
    ASSERT_EQ(feed(assembler, bytes, response, err), DataSourceAssembler::Result::Complete) << err;
    EXPECT_EQ(response.attribute(1), "") << "an absent attribute is still a tuple";
    EXPECT_EQ(response.attributes.size(), 2u);
}
