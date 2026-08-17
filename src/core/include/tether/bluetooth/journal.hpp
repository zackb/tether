#pragma once

#include "tether/bluetooth/messages.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace tether::bluetooth {

    inline constexpr size_t JOURNAL_MAX_MESSAGES = 10000;
    inline constexpr int64_t JOURNAL_MAX_AGE_SECONDS = 90LL * 24 * 3600;

    std::string journal_path();

    // Append-only newline-delimited JSON at ~/.local/share/tether/messages.ndjson.
    class MessageJournal {
    public:
        // Creates the directory and opens the file for appending, mode 0600
        bool open();
        bool is_open() const { return out_.is_open(); }
        void close();

        void append(const Message& message);

        // Malformed lines are skipped rather than aborting the load
        std::vector<Message> load(int64_t now) const;

        // Rewrites the file to exactly `messages`, atomically.
        bool compact(const std::vector<Message>& messages);

    private:
        std::ofstream out_;
        std::string path_;
    };

    std::string serialize_message_line(const Message& message);
    bool deserialize_message_line(const std::string& line, Message& out);

    // Drops anything older than max_age, then keeps the newest max_messages.
    std::vector<Message> apply_retention(std::vector<Message> messages,
                                         int64_t now,
                                         size_t max_messages = JOURNAL_MAX_MESSAGES,
                                         int64_t max_age = JOURNAL_MAX_AGE_SECONDS);

} // namespace tether::bluetooth
