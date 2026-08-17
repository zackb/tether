#include "tether/bluetooth/journal.hpp"
#include "tether/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <pwd.h>
#include <unistd.h>

namespace tether::bluetooth {

    namespace {

        std::string home_dir() {
            if (const char* home = getenv("HOME"); home && *home)
                return home;
            if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir)
                return pw->pw_dir;
            return {};
        }

        void restrict_permissions(const std::string& path) {
            std::error_code ec;
            std::filesystem::permissions(path,
                                         std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::replace,
                                         ec);
        }

    } // namespace

    std::string journal_path() {
        std::string home = home_dir();
        if (home.empty())
            return {};
        return (std::filesystem::path(home) / ".local" / "share" / "tether" / "messages.ndjson").string();
    }

    std::string serialize_message_line(const Message& message) {
        return to_json(message).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    bool deserialize_message_line(const std::string& line, Message& out) {
        try {
            auto j = nlohmann::json::parse(line);
            if (!j.is_object())
                return false;
            Message message;
            message.handle = j.value("handle", "");
            message.thread_key = j.value("thread", "");
            message.peer_address = j.value("address", "");
            message.peer_name = j.value("name", "");
            message.body = j.value("body", "");
            message.timestamp = j.value("timestamp", static_cast<int64_t>(0));
            message.outgoing = j.value("outgoing", false);
            message.read = j.value("read", false);
            message.folder = j.value("folder", "");
            // A message with no handle cannot be deduped and would replay forever.
            if (message.handle.empty() || message.thread_key.empty())
                return false;
            out = std::move(message);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    std::vector<Message>
        apply_retention(std::vector<Message> messages, int64_t now, size_t max_messages, int64_t max_age) {
        {
            std::map<std::string, size_t> newest;
            for (size_t i = 0; i < messages.size(); ++i)
                newest[messages[i].handle] = i;
            std::vector<Message> unique;
            unique.reserve(newest.size());
            for (size_t i = 0; i < messages.size(); ++i) {
                if (newest[messages[i].handle] == i)
                    unique.push_back(std::move(messages[i]));
            }
            messages = std::move(unique);
        }

        if (max_age > 0) {
            const int64_t cutoff = now - max_age;
            // zero timestamp means the phone gave us nothing usable
            std::erase_if(messages, [&](const Message& m) { return m.timestamp > 0 && m.timestamp < cutoff; });
        }

        if (messages.size() > max_messages) {
            std::stable_sort(messages.begin(), messages.end(), [](const Message& a, const Message& b) {
                return a.timestamp < b.timestamp;
            });
            messages.erase(messages.begin(), messages.begin() + static_cast<long>(messages.size() - max_messages));
        }
        return messages;
    }

    bool MessageJournal::open() {
        path_ = journal_path();
        if (path_.empty())
            return false;

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);

        out_.open(path_, std::ios::app);
        if (!out_.is_open()) {
            debug::log(ERR, "bluetooth: cannot open journal {}", path_);
            return false;
        }
        restrict_permissions(path_);
        return true;
    }

    void MessageJournal::close() {
        if (out_.is_open())
            out_.close();
    }

    void MessageJournal::append(const Message& message) {
        if (!out_.is_open())
            return;
        out_ << serialize_message_line(message) << "\n";
        out_.flush();
    }

    std::vector<Message> MessageJournal::load(int64_t now) const {
        std::vector<Message> messages;
        const std::string path = path_.empty() ? journal_path() : path_;
        if (path.empty())
            return messages;

        std::ifstream in(path);
        if (!in.is_open())
            return messages;

        std::string line;
        size_t skipped = 0;
        while (std::getline(in, line)) {
            if (line.empty())
                continue;
            Message message;
            if (deserialize_message_line(line, message))
                messages.push_back(std::move(message));
            else
                ++skipped;
        }
        if (skipped)
            debug::log(WARN, "bluetooth: skipped {} unreadable journal line(s)", skipped);

        return apply_retention(std::move(messages), now);
    }

    bool MessageJournal::compact(const std::vector<Message>& messages) {
        const std::string path = path_.empty() ? journal_path() : path_;
        if (path.empty())
            return false;

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out.is_open()) {
                debug::log(ERR, "bluetooth: cannot write {}", tmp);
                return false;
            }
            for (const auto& message : messages)
                out << serialize_message_line(message) << "\n";
            if (!out) {
                debug::log(ERR, "bluetooth: failed writing {}", tmp);
                return false;
            }
        }
        restrict_permissions(tmp);

        const bool was_open = out_.is_open();
        if (was_open)
            out_.close();

        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            debug::log(ERR, "bluetooth: cannot replace {}: {}", path, ec.message());
            std::filesystem::remove(tmp, ec);
        }

        if (was_open) {
            out_.open(path, std::ios::app);
            restrict_permissions(path);
        }
        return !ec;
    }

} // namespace tether::bluetooth
