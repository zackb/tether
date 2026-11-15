#include "tether/bluetooth/messages.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace tether::bluetooth {

    bool is_outgoing_folder(const std::string& folder) {
        return folder.find("sent") != std::string::npos || folder.find("outbox") != std::string::npos;
    }

    namespace {

        // Every run of whitespace becomes one space, and the ends are dropped.
        std::string collapse_whitespace(const std::string& text) {
            std::string out;
            bool pending = false;
            for (unsigned char c : text) {
                if (std::isspace(c)) {
                    pending = !out.empty();
                    continue;
                }
                if (pending) {
                    out += ' ';
                    pending = false;
                }
                out += static_cast<char>(c);
            }
            return out;
        }

        // Which of two spellings of one address to file the conversation under.
        bool better_key(const std::string& candidate, const std::string& current) {
            const std::string a = candidate.substr(candidate.find(':') + 1);
            const std::string b = current.substr(current.find(':') + 1);
            const bool a_plus = !a.empty() && a.front() == '+';
            const bool b_plus = !b.empty() && b.front() == '+';
            if (a_plus != b_plus)
                return a_plus;
            if (a.size() != b.size())
                return a.size() > b.size();
            return a < b;
        }

    } // namespace

    std::string display_address(const VCardParty& party) {
        if (party.tel.empty())
            return normalize_email(party.email);
        std::string tel = normalize_phone(party.tel);
        return tel.empty() ? collapse_whitespace(party.tel) : tel;
    }

    MessageStore::MessageStore(size_t max_messages) : max_messages_(max_messages) {}

    const std::string& MessageStore::canonical_key(const std::string& thread_key) {
        const std::string bucket = thread_bucket(thread_key);
        auto [at, fresh] = representative_.emplace(bucket, thread_key);
        if (fresh || at->second == thread_key)
            return at->second;
        if (!better_key(thread_key, at->second))
            return at->second;

        // A better spelling arrived; move the conversation to it so every
        // consumer sees one key.
        const std::string previous = at->second;
        at->second = thread_key;
        for (auto& [handle, stored] : by_handle_) {
            if (stored.thread_key == previous)
                stored.thread_key = thread_key;
        }
        return at->second;
    }

    bool MessageStore::add(const Message& message) {
        if (message.handle.empty())
            return false;
        if (auto it = by_handle_.find(message.handle); it != by_handle_.end()) {
            // re-listing after reconnect has a fresh object path for the same message
            if (!message.object_path.empty())
                it->second.object_path = message.object_path;
            return false;
        }

        Message stored = message;
        if (!stored.thread_key.empty())
            stored.thread_key = canonical_key(stored.thread_key);

        if (stored.outgoing && !is_local_handle(stored.handle))
            drop_local_echo(stored);

        order_.push_back(stored.handle);
        by_handle_.emplace(stored.handle, std::move(stored));
        trim();
        return true;
    }

    // Linear scan. Only a handful of local sends are ever waiting to be
    // matched, and the prefix check rejects everything else before any string is
    // compared. Index by thread if the store ever grows enough for this to show.
    void MessageStore::drop_local_echo(const Message& arrived) {
        for (auto it = by_handle_.begin(); it != by_handle_.end(); ++it) {
            const Message& existing = it->second;
            if (!existing.outgoing || !is_local_handle(existing.handle))
                continue;
            // The phone's copy is not byte-identical to what was typed. MAP
            // listing reports the body with its line breaks flattened to
            // spaces, so only the text itself can be compared.
            if (existing.thread_key != arrived.thread_key ||
                collapse_whitespace(existing.body) != collapse_whitespace(arrived.body))
                continue;
            if (std::llabs(existing.timestamp - arrived.timestamp) > LOCAL_ECHO_WINDOW_SECONDS)
                continue;
            order_.erase(std::remove(order_.begin(), order_.end(), existing.handle), order_.end());
            by_handle_.erase(it);
            return;
        }
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

    std::optional<int64_t> MessageStore::newest_timestamp() const {
        std::optional<int64_t> newest;
        for (const auto& [handle, message] : by_handle_)
            if (!newest || message.timestamp > *newest)
                newest = message.timestamp;
        return newest;
    }

    void MessageStore::trim() {
        while (order_.size() > max_messages_) {
            by_handle_.erase(order_.front());
            order_.erase(order_.begin());
        }
    }

    std::vector<Thread> MessageStore::threads(const Visible& visible) const {
        std::map<std::string, Thread> grouped;

        for (const auto& [handle, message] : by_handle_) {
            if (message.thread_key.empty() || (visible && !visible(message)))
                continue;
            Thread& thread = grouped[message.thread_key];
            thread.key = message.thread_key;
            thread.peer_address = message.peer_address;
            ++thread.count;
            thread.replied = thread.replied || message.outgoing;

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

    std::vector<Message>
        MessageStore::messages(const std::string& thread_key, size_t limit, const Visible& visible) const {
        // Matched on the bucket, not the string
        const std::string bucket = thread_bucket(thread_key);
        std::vector<Message> result;
        for (const auto& [handle, message] : by_handle_) {
            if (thread_bucket(message.thread_key) == bucket && (!visible || visible(message)))
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
            {"preview", collapse_whitespace(thread.last_body)},
            {"timestamp", thread.last_timestamp},
            {"unread", thread.unread},
            {"count", thread.count},
        };
    }

} // namespace tether::bluetooth
