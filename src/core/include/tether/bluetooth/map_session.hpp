#pragma once

#include "tether/bluetooth/messages.hpp"

#include <functional>
#include <gio/gio.h>
#include <memory>
#include <string>
#include <vector>

namespace tether::bluetooth {

    // One entry from MessageAccess1.ListMessages.
    struct MapListing {
        std::string handle;
        std::string subject;
        std::string sender_address;
        std::string sender_name;
        std::string timestamp;
        std::string type;
        std::string folder;
        bool read = false;
        bool sent = false;
    };

    // Converts a listing into a Message without a bMessage round trip.
    Message message_from_listing(const MapListing& listing);

    // Extracts the stable message id from an obexd object path. The path's
    // session number changes on every reconnect, so only the id can be used to
    // recognize a message we already have.
    std::string map_handle_from_path(const std::string& object_path);

    struct MapSessionState;

    // Drives org.bluez.obex.MessageAccess1 for one open OBEX session.
    class MapSession {
    public:
        // Called for each new or updated message.
        using MessageFn = std::function<void(const Message&)>;

        MapSession(GDBusConnection* session_bus, std::string session_path);
        ~MapSession();

        MapSession(const MapSession&) = delete;
        MapSession& operator=(const MapSession&) = delete;

        // Selects a MAP folder. SetFolder walks the hierarchy from the session
        // root, so this takes a path like "telecom/msg/inbox"
        bool set_folder(const std::string& folder, std::string& err);

        // Lists the current folder. `max` bounds the transfer
        std::vector<MapListing> list_messages(const std::string& folder, int max, std::string& err);

        // Fetches and parses the full bMessage for one handle
        bool fetch_bmessage(const std::string& message_path, BMessage& out, std::string& err);

        // Writes Message1.Read, which is reflected on the phone.
        bool set_read(const std::string& message_path, bool read, std::string& err);

        // Hands a built bMessage to the phone for delivery. `folder` is a MAP
        // path such as "telecom/msg/outbox".
        //
        // Success means the phone accepted the message, not that it was delivered.
        bool push_message(const std::string& folder, const std::string& bmessage, std::string& err);

        const std::string& path() const;

    private:
        std::unique_ptr<MapSessionState> state_;
    };

} // namespace tether::bluetooth
