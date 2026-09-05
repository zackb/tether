#include "tether/bluetooth/journal.hpp"
#include "tether/log.hpp"
#include "tether/paths.hpp"
#include "tether/secret_store.hpp"

#include <algorithm>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace tether::bluetooth {

    namespace {

        void restrict_permissions(const std::string& path) {
            std::error_code ec;
            std::filesystem::permissions(path,
                                         std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::replace,
                                         ec);
        }

        // Flushes a file to disk before anything is renamed over or unlinked.
        void sync_path(const std::string& path) {
            const int fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0)
                return;
            ::fsync(fd);
            ::close(fd);
        }

        Retention other_mode(Retention mode) {
            return mode == Retention::Encrypted ? Retention::Plaintext : Retention::Encrypted;
        }

    } // namespace

    std::string journal_path(Retention mode) {
        const std::filesystem::path dir = paths::data_dir();
        if (dir.empty() || mode == Retention::None)
            return {};
        return (dir / (mode == Retention::Encrypted ? "messages.ndjson.enc" : "messages.ndjson")).string();
    }

    std::string journal_path() { return journal_path(secret::retention()); }

    std::string serialize_message_line(const Message& message) {
        return secret::seal(to_json(message).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }

    bool deserialize_message_line(const std::string& line, Message& out) {
        std::string text;
        if (!secret::unseal(line, text))
            return false;
        try {
            auto j = nlohmann::json::parse(text);
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

    bool migrate_journal(Retention from, Retention to) {
        const std::string source = journal_path(from);
        const std::string destination = journal_path(to);
        if (source.empty() || destination.empty() || source == destination)
            return false;
        if (!std::filesystem::exists(source))
            return false;

        std::ifstream in(source);
        if (!in.is_open())
            return false;

        std::vector<std::string> records;
        std::string line;
        size_t unreadable = 0;
        while (std::getline(in, line)) {
            if (line.empty())
                continue;
            std::string text;
            if (secret::unseal(line, text, from))
                records.push_back(std::move(text));
            else
                ++unreadable;
        }
        in.close();

        if (unreadable) {
            debug::log(WARN,
                       "bluetooth: {} has {} unreadable line(s); leaving it in place rather than migrating part of it",
                       source,
                       unreadable);
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(destination).parent_path(), ec);

        // A destination already in place means an earlier migration was interrupted between
        // the rename and the unlink.
        const bool merging = std::filesystem::exists(destination);
        const std::string tmp = destination + ".tmp";
        const std::string target = merging ? destination : tmp;
        bool failed = false;
        {
            std::ofstream out(target, merging ? std::ios::app : std::ios::trunc);
            if (!out.is_open()) {
                debug::log(ERR, "bluetooth: cannot write {}", target);
                return false;
            }
            for (const auto& record : records) {
                const std::string sealed = secret::seal(record, to);
                if (sealed.empty()) {
                    debug::log(ERR, "bluetooth: cannot seal {}; leaving {} in place", destination, source);
                    failed = true;
                    break;
                }
                out << sealed << "\n";
            }
            if (!failed && !out) {
                debug::log(ERR, "bluetooth: failed writing {}", target);
                failed = true;
            }
        }
        if (failed) {
            if (!merging)
                std::filesystem::remove(tmp, ec);
            return false;
        }
        restrict_permissions(target);

        if (merging) {
            sync_path(destination);
            std::filesystem::remove(source, ec);
            debug::log(
                INFO, "bluetooth: folded {} message(s) from {} into the existing journal", records.size(), source);
            return true;
        }

        // The source is deleted immediately after, so the replacement has to be on disk first.
        sync_path(tmp);
        std::filesystem::rename(tmp, destination, ec);
        if (ec) {
            debug::log(ERR, "bluetooth: cannot replace {}: {}", destination, ec.message());
            std::filesystem::remove(tmp, ec);
            return false;
        }
        sync_path(std::filesystem::path(destination).parent_path().string());

        std::filesystem::remove(source, ec);
        debug::log(INFO, "bluetooth: migrated {} message(s) to {} retention", records.size(), to_string(to));
        return true;
    }

    bool MessageJournal::open() {
        const Retention mode = secret::retention();
        if (mode == Retention::None)
            return false;
        // Encrypted set but a locked or unreachable wallet: stay live-only
        if (!secret::have_key())
            return false;

        path_ = journal_path(mode);
        if (path_.empty())
            return false;

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);

        migrate_journal(other_mode(mode), mode);

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

        const std::string line = serialize_message_line(message);
        if (line.empty())
            return;

        out_ << line << "\n";
        out_.flush();
    }

    std::vector<Message> MessageJournal::load(int64_t now) const {
        std::vector<Message> messages;
        skipped_ = 0;
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
        skipped_ = skipped;

        return apply_retention(std::move(messages), now);
    }

    bool MessageJournal::compact(const std::vector<Message>& messages) {
        const std::string path = path_.empty() ? journal_path() : path_;
        if (path.empty())
            return false;

        std::error_code ec;

        if (skipped_) {
            debug::log(WARN, "bluetooth: refusing to rewrite {} after a partial read", path);
            return false;
        }

        if (messages.empty() && std::filesystem::file_size(path, ec) > 0 && !ec) {
            debug::log(WARN, "bluetooth: refusing to empty a populated journal");
            return false;
        }

        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out.is_open()) {
                debug::log(ERR, "bluetooth: cannot write {}", tmp);
                return false;
            }
            for (const auto& message : messages) {
                const std::string line = serialize_message_line(message);
                if (line.empty()) {
                    debug::log(ERR, "bluetooth: cannot seal the journal; leaving {} as it was", path);
                    return false;
                }
                out << line << "\n";
            }
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
