#pragma once

#include "tether/bluetooth/bmessage.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace tether::bluetooth {

    // Handles minted locally for a message sent from here, before the phone has
    // listed its own copy. The prefix is what tells the two apart.
    inline constexpr const char* LOCAL_HANDLE_PREFIX = "local-";

    inline bool is_local_handle(const std::string& handle) { return handle.rfind(LOCAL_HANDLE_PREFIX, 0) == 0; }

    // How far apart the local record and the phone's copy of the same sent
    // message may be before they stop being treated as the same message.
    inline constexpr int64_t LOCAL_ECHO_WINDOW_SECONDS = 300;

    // True for a MAP folder holding messages the phone sent.
    bool is_outgoing_folder(const std::string& folder);

    // The other party's address as the UI should show it: a normalized number or
    // email, or the raw sender ID when it is neither.
    std::string display_address(const VCardParty& party);

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
        bool replied = false;
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

        // Messages `visible` rejects are left out before anything is counted.
        // Empty means all of them.
        using Visible = std::function<bool(const Message&)>;

        // Newest activity first.
        std::vector<Thread> threads(const Visible& visible = {}) const;
        // Oldest first within a thread, limited to the most recent `limit`.
        std::vector<Message>
            messages(const std::string& thread_key, size_t limit = 200, const Visible& visible = {}) const;

        size_t size() const { return by_handle_.size(); }

        // Timestamp of the newest message held, or empty when there are none.
        std::optional<int64_t> newest_timestamp() const;

    private:
        void trim();
        // Removes the placeholder written when the user sent `arrived` from here,
        // now that the phone has listed its own copy of it.
        void drop_local_echo(const Message& arrived);

        // The key every message of `thread_key`'s peer is filed under
        const std::string& canonical_key(const std::string& thread_key);

        size_t max_messages_;
        std::map<std::string, Message> by_handle_;
        // Insertion order, used to evict the oldest when the cap is reached.
        std::vector<std::string> order_;
        std::map<std::string, std::string> representative_;
    };

    nlohmann::json to_json(const Message& message);
    nlohmann::json to_json(const Thread& thread);

    // Builds a Message from a parsed bMessage plus the metadata MAP supplies
    // alongside it. `handle` and `folder` come from the MAP listing.
    Message message_from_bmessage(const BMessage& parsed, const std::string& handle, int64_t timestamp);

} // namespace tether::bluetooth
