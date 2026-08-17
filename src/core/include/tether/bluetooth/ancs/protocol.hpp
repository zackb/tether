#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Apple Notification Center Service. Constants and wire types from Apple's
// published ANCS specification.
namespace tether::bluetooth::ancs {

    inline constexpr const char* UUID_SERVICE = "7905f431-b5ce-4e99-a40f-4b1e122d00d0";
    inline constexpr const char* UUID_NOTIFICATION_SOURCE = "9fbf120d-6301-42d9-8c58-25e699a21dbd";
    inline constexpr const char* UUID_CONTROL_POINT = "69d1d8f3-45e1-49a8-9821-9bbdfdaad9d9";
    inline constexpr const char* UUID_DATA_SOURCE = "22eac6e9-24d6-4bb5-be44-b36ace7c7bfb";

    // Messages already arrive over MAP with proper read-state sync, so ANCS
    // copies of them must never produce a second desktop popup.
    inline constexpr const char* APP_ID_MESSAGES = "com.apple.MobileSMS";

    enum class EventId : uint8_t { Added = 0, Modified = 1, Removed = 2 };

    enum EventFlags : uint8_t {
        FlagSilent = 1 << 0,
        FlagImportant = 1 << 1,
        // Set on notifications that were already on the phone when we
        // subscribed. Delivering these would replay the lock screen as a burst
        // of desktop popups.
        FlagPreExisting = 1 << 2,
        FlagPositiveAction = 1 << 3,
        FlagNegativeAction = 1 << 4,
    };

    enum class CategoryId : uint8_t {
        Other = 0,
        IncomingCall = 1,
        MissedCall = 2,
        Voicemail = 3,
        Social = 4,
        Schedule = 5,
        Email = 6,
        News = 7,
        HealthAndFitness = 8,
        BusinessAndFinance = 9,
        Location = 10,
        Entertainment = 11,
    };

    enum class CommandId : uint8_t {
        GetNotificationAttributes = 0,
        GetAppAttributes = 1,
        PerformNotificationAction = 2,
    };

    enum class NotificationAttributeId : uint8_t {
        AppIdentifier = 0,
        Title = 1,
        Subtitle = 2,
        Message = 3,
        MessageSize = 4,
        Date = 5,
        PositiveActionLabel = 6,
        NegativeActionLabel = 7,
    };

    enum class AppAttributeId : uint8_t { DisplayName = 0 };

    enum class ActionId : uint8_t { Positive = 0, Negative = 1 };

    // Title, Subtitle and Message are the only attributes that take a maximum
    // length, and it is mandatory for them.
    inline constexpr bool attribute_takes_length(NotificationAttributeId id) {
        return id == NotificationAttributeId::Title || id == NotificationAttributeId::Subtitle ||
               id == NotificationAttributeId::Message;
    }

    // Bodies are capped rather than requested whole. The cap also fixes the
    // prefix that Phase 9 correlates a group message against.
    inline constexpr uint16_t MAX_BODY_LENGTH = 256;
    inline constexpr uint16_t MAX_TITLE_LENGTH = 128;

    // One Notification Source packet, which is always exactly eight bytes.
    struct SourceEvent {
        EventId event = EventId::Added;
        uint8_t flags = 0;
        CategoryId category = CategoryId::Other;
        uint8_t category_count = 0;
        uint32_t uid = 0;

        bool pre_existing() const { return (flags & FlagPreExisting) != 0; }
        bool silent() const { return (flags & FlagSilent) != 0; }
        bool has_positive_action() const { return (flags & FlagPositiveAction) != 0; }
        bool has_negative_action() const { return (flags & FlagNegativeAction) != 0; }

        bool operator==(const SourceEvent&) const = default;
    };

    struct Attribute {
        uint8_t id = 0;
        std::string value;

        bool operator==(const Attribute&) const = default;
    };

    // A reassembled Data Source response.
    struct Response {
        CommandId command = CommandId::GetNotificationAttributes;
        // Set for GetNotificationAttributes.
        uint32_t uid = 0;
        // Set for GetAppAttributes.
        std::string app_id;
        std::vector<Attribute> attributes;

        // Empty when the attribute was absent or the phone withheld it.
        std::string attribute(uint8_t id) const;

        bool operator==(const Response&) const = default;
    };

} // namespace tether::bluetooth::ancs
