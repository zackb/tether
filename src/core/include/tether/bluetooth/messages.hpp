#pragma once

#include "tether/bluetooth/bmessage.hpp"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace tether::bluetooth {

    struct Message {
        std::string handle;
        std::string object_path;
        std::string thread_key;
        std::string peer_address;
        // display only
        std::string peer_name;
        std::string body;
        int64_t timestamp = 0;
        bool outgoing = false;
        bool read = false;
        std::string folder;

        bool operator==(const Message&) const = default;
    };

    struct Thread {
        std::string key;
        // best display name seen for this peer, falling back to the address.
        std::string display_name;
        std::string peer_address;
        std::string last_body;
        int64_t last_timestamp = 0;
        int unread = 0;
        int count = 0;
    };

    // In-memory conversation store: dedupes by handle, groups into threads, and
    // tracks read state.
    class MessageStore {
    public:
        explicit MessageStore(size_t max_messages = 10000);

        // Returns false when the handle was already known, so callers can avoid
        // re-announcing a message the phone replayed.
        bool add(const Message& message);

        // Returns false when the handle is unknown or the state already matched,
        // so an echo of our own change does not loop.
        bool set_read(const std::string& handle, bool read);

        const Message* find(const std::string& handle) const;

        // Newest activity first.
        std::vector<Thread> threads() const;
        // Oldest first within a thread, limited to the most recent `limit`.
        std::vector<Message> messages(const std::string& thread_key, size_t limit = 200) const;

        size_t size() const { return by_handle_.size(); }

    private:
        void trim();

        size_t max_messages_;
        std::map<std::string, Message> by_handle_;
        // Insertion order, used to evict the oldest when the cap is reached.
        std::vector<std::string> order_;
    };

    nlohmann::json to_json(const Message& message);
    nlohmann::json to_json(const Thread& thread);

    // Builds a Message from a parsed bMessage plus the metadata MAP supplies
    // alongside it. `handle` and `folder` come from the MAP listing.
    Message message_from_bmessage(const BMessage& parsed, const std::string& handle, int64_t timestamp);

} // namespace tether::bluetooth
