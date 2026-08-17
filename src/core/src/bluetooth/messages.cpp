#include "tether/bluetooth/messages.hpp"

#include <algorithm>

namespace tether::bluetooth {

    namespace {

        bool is_outgoing_folder(const std::string& folder) {
            return folder.find("sent") != std::string::npos || folder.find("outbox") != std::string::npos;
        }

        std::string display_address(const VCardParty& party) {
            if (!party.tel.empty())
                return normalize_phone(party.tel);
            return normalize_email(party.email);
        }

    } // namespace

    MessageStore::MessageStore(size_t max_messages) : max_messages_(max_messages) {}

    bool MessageStore::add(const Message& message) {
        if (message.handle.empty())
            return false;
        if (auto it = by_handle_.find(message.handle); it != by_handle_.end()) {
            // re-listing after reconnect has a fresh object path for the same message
            if (!message.object_path.empty())
                it->second.object_path = message.object_path;
            return false;
        }

        by_handle_.emplace(message.handle, message);
        order_.push_back(message.handle);
        trim();
        return true;
    }

    bool MessageStore::set_read(const std::string& handle, bool read) {
        auto it = by_handle_.find(handle);
        if (it == by_handle_.end() || it->second.read == read)
            return false;
        it->second.read = read;
        return true;
    }

    const Message* MessageStore::find(const std::string& handle) const {
        auto it = by_handle_.find(handle);
        return it == by_handle_.end() ? nullptr : &it->second;
    }

    void MessageStore::trim() {
        while (order_.size() > max_messages_) {
            by_handle_.erase(order_.front());
            order_.erase(order_.begin());
        }
    }

    std::vector<Thread> MessageStore::threads() const {
        std::map<std::string, Thread> grouped;

        for (const auto& [handle, message] : by_handle_) {
            if (message.thread_key.empty())
                continue;
            Thread& thread = grouped[message.thread_key];
            thread.key = message.thread_key;
            thread.peer_address = message.peer_address;
            ++thread.count;

            if (thread.display_name.empty() ||
                (!message.peer_name.empty() && thread.display_name == thread.peer_address))
                if (!message.peer_name.empty())
                    thread.display_name = message.peer_name;

            if (!message.read && !message.outgoing)
                ++thread.unread;

            if (message.timestamp >= thread.last_timestamp) {
                thread.last_timestamp = message.timestamp;
                thread.last_body = message.body;
            }
        }

        std::vector<Thread> result;
        result.reserve(grouped.size());
        for (auto& [key, thread] : grouped) {
            if (thread.display_name.empty())
                thread.display_name = thread.peer_address;
            result.push_back(thread);
        }

        std::sort(result.begin(), result.end(), [](const Thread& a, const Thread& b) {
            if (a.last_timestamp != b.last_timestamp)
                return a.last_timestamp > b.last_timestamp;
            return a.key < b.key;
        });
        return result;
    }

    std::vector<Message> MessageStore::messages(const std::string& thread_key, size_t limit) const {
        std::vector<Message> result;
        for (const auto& [handle, message] : by_handle_) {
            if (message.thread_key == thread_key)
                result.push_back(message);
        }

        std::sort(result.begin(), result.end(), [](const Message& a, const Message& b) {
            if (a.timestamp != b.timestamp)
                return a.timestamp < b.timestamp;
            return a.handle < b.handle;
        });

        if (result.size() > limit)
            result.erase(result.begin(), result.end() - static_cast<long>(limit));
        return result;
    }

    Message message_from_bmessage(const BMessage& parsed, const std::string& handle, int64_t timestamp) {
        Message message;
        message.handle = handle;
        message.timestamp = timestamp;
        message.read = parsed.read;
        message.folder = parsed.folder;
        message.body = parsed.body;
        message.outgoing = is_outgoing_folder(parsed.folder);

        const VCardParty& peer =
            message.outgoing && !parsed.recipients.empty() ? parsed.recipients.front() : parsed.originator;

        message.thread_key = thread_key_for(peer);
        message.peer_address = display_address(peer);
        message.peer_name = peer.name;
        return message;
    }

    nlohmann::json to_json(const Message& message) {
        return {
            {"handle", message.handle},
            {"thread", message.thread_key},
            {"address", message.peer_address},
            {"name", message.peer_name},
            {"body", message.body},
            {"timestamp", message.timestamp},
            {"outgoing", message.outgoing},
            {"read", message.read},
            {"folder", message.folder},
        };
    }

    nlohmann::json to_json(const Thread& thread) {
        return {
            {"thread", thread.key},
            {"name", thread.display_name},
            {"address", thread.peer_address},
            {"preview", thread.last_body},
            {"timestamp", thread.last_timestamp},
            {"unread", thread.unread},
            {"count", thread.count},
        };
    }

} // namespace tether::bluetooth
