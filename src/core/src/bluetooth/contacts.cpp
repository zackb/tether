#include "tether/bluetooth/contacts.hpp"
#include "tether/bluetooth/bmessage.hpp"
#include "tether/log.hpp"
#include "tether/paths.hpp"
#include "tether/secret_store.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <unistd.h>

namespace tether::bluetooth {

    void ContactStore::set(std::vector<VCard> contacts) {
        contacts_ = std::move(contacts);
        by_key_.clear();
        by_tel_suffix_.clear();

        for (const auto& card : contacts_) {
            if (card.name.empty())
                continue;
            for (const auto& tel : card.tels) {
                std::string normalized = normalize_phone(tel);
                if (normalized.empty())
                    continue;
                by_key_.emplace("tel:" + normalized, card.name);

                if (std::string suffix = tel_suffix(normalized); !suffix.empty()) {
                    auto [at, fresh] = by_tel_suffix_.emplace(suffix, card.name);
                    if (!fresh && at->second != card.name)
                        at->second.clear();
                }
            }
            for (const auto& email : card.emails) {
                std::string normalized = normalize_email(email);
                if (!normalized.empty())
                    by_key_.emplace("email:" + normalized, card.name);
            }
        }
    }

    std::string ContactStore::name_for(const std::string& thread_key) const {
        if (auto it = by_key_.find(thread_key); it != by_key_.end())
            return it->second;

        if (thread_key.rfind("tel:", 0) == 0) {
            if (std::string suffix = tel_suffix(thread_key.substr(4)); !suffix.empty()) {
                if (auto it = by_tel_suffix_.find(suffix); it != by_tel_suffix_.end())
                    return it->second;
            }
        }
        return {};
    }

    std::vector<std::string> ContactStore::addresses_for_name(const std::string& name) const {
        std::vector<std::string> out;
        if (name.empty())
            return out;

        std::string wanted = name;
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        for (const auto& card : contacts_) {
            std::string candidate = card.name;
            std::transform(candidate.begin(), candidate.end(), candidate.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (candidate != wanted)
                continue;

            // A phone number is what Messages routes on, so it wins when a
            // contact has both.
            for (const auto& tel : card.tels) {
                std::string normalized = normalize_phone(tel);
                if (!normalized.empty())
                    out.push_back("tel:" + normalized);
            }
            for (const auto& email : card.emails) {
                std::string normalized = normalize_email(email);
                if (!normalized.empty())
                    out.push_back("email:" + normalized);
            }
        }
        return out;
    }

    static std::string fold(const std::string& text) {
        gchar* normalized = g_utf8_normalize(text.c_str(), -1, G_NORMALIZE_ALL);
        gchar* folded = g_utf8_casefold(normalized ? normalized : text.c_str(), -1);
        std::string out = folded ? folded : "";
        g_free(normalized);
        g_free(folded);
        return out;
    }

    std::vector<VCard> ContactStore::search(const std::string& needle, size_t limit) const {
        const std::string wanted = fold(needle);
        std::vector<VCard> out;
        for (const auto& card : contacts_) {
            if (out.size() >= limit)
                break;
            if (wanted.empty()) {
                out.push_back(card);
                continue;
            }
            std::string haystack = fold(card.name);
            for (const auto& tel : card.tels)
                haystack += " " + fold(tel) + " " + normalize_phone(tel);
            for (const auto& email : card.emails)
                haystack += " " + fold(email);
            if (haystack.find(wanted) != std::string::npos)
                out.push_back(card);
        }
        std::sort(out.begin(), out.end(), [](const VCard& a, const VCard& b) { return fold(a.name) < fold(b.name); });
        return out;
    }

    std::string contacts_path(Retention mode) {
        const std::filesystem::path dir = paths::data_dir();
        if (dir.empty() || mode == Retention::None)
            return {};
        return (dir / (mode == Retention::Encrypted ? "contacts.json.enc" : "contacts.json")).string();
    }

    std::string contacts_path() { return contacts_path(secret::retention()); }

    std::string serialize_contacts(const ContactStore& store) {
        nlohmann::json j;
        j["contacts"] = nlohmann::json::array();
        for (const auto& card : store.contacts()) {
            nlohmann::json c;
            c["name"] = card.name;
            c["tels"] = card.tels;
            c["emails"] = card.emails;
            j["contacts"].push_back(std::move(c));
        }

        return secret::seal(j.dump(2));
    }

    ContactStore deserialize_contacts(const std::string& text) {
        ContactStore store;
        std::vector<VCard> cards;
        std::string plain;
        if (!secret::unseal(text, plain))
            return store;
        try {
            auto j = nlohmann::json::parse(plain);
            for (const auto& c : j.value("contacts", nlohmann::json::array())) {
                VCard card;
                card.name = c.value("name", "");
                card.tels = c.value("tels", std::vector<std::string>{});
                card.emails = c.value("emails", std::vector<std::string>{});
                if (!card.empty())
                    cards.push_back(std::move(card));
            }
        } catch (const std::exception&) {
            // A corrupt cache costs display names, not correctness. Threads fall
            // back to raw addresses until PBAP refreshes.
            return store;
        }
        store.set(std::move(cards));
        return store;
    }

    ContactStore load_contacts() {
        const Retention mode = secret::retention();
        if (mode == Retention::None || !secret::have_key())
            return {};

        // No-op unless a cache written under the other mode is still there.
        migrate_contacts(mode == Retention::Encrypted ? Retention::Plaintext : Retention::Encrypted, mode);

        std::string path = contacts_path(mode);
        if (path.empty())
            return {};
        std::ifstream in(path);
        if (!in.is_open())
            return {};
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return deserialize_contacts(text);
    }

    bool save_contacts(const ContactStore& store) {
        const Retention mode = secret::retention();
        if (mode == Retention::None)
            return false;

        const std::string body = serialize_contacts(store);

        if (body.empty())
            return false;

        std::string path = contacts_path(mode);
        if (path.empty())
            return false;

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

        // Write-then-rename, matching how the Bluetooth config is persisted.
        std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out.is_open()) {
                debug::log(ERR, "bluetooth: cannot write {}", tmp);
                return false;
            }
            out << body;
            if (!out) {
                debug::log(ERR, "bluetooth: failed writing {}", tmp);
                return false;
            }
        }

        // The phonebook is personal data, so it gets the same mode as the key.
        std::filesystem::permissions(tmp,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace,
                                     ec);
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            debug::log(ERR, "bluetooth: cannot replace {}: {}", path, ec.message());
            std::filesystem::remove(tmp, ec);
            return false;
        }
        return true;
    }

    bool migrate_contacts(Retention from, Retention to) {
        const std::string source = contacts_path(from);
        const std::string destination = contacts_path(to);
        if (source.empty() || destination.empty() || source == destination)
            return false;
        if (!std::filesystem::exists(source))
            return false;

        // A destination already there means an earlier migration was interrupted between
        // the rename and the unlink.
        if (std::filesystem::exists(destination)) {
            std::error_code ec;
            std::filesystem::remove(source, ec);
            debug::log(INFO, "bluetooth: removed {}, left over from an interrupted migration", source);
            return !ec;
        }

        std::ifstream in(source);
        if (!in.is_open())
            return false;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        std::string plain;
        if (!secret::unseal(text, plain, from)) {
            debug::log(WARN, "bluetooth: cannot read {} to migrate it; leaving it in place", source);
            return false;
        }
        const std::string sealed = secret::seal(plain, to);
        if (sealed.empty())
            return false;

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(destination).parent_path(), ec);

        const std::string tmp = destination + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out.is_open())
                return false;
            out << sealed;
            if (!out)
                return false;
        }
        std::filesystem::permissions(tmp,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace,
                                     ec);

        if (const int fd = ::open(tmp.c_str(), O_RDONLY); fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
        std::filesystem::rename(tmp, destination, ec);
        if (ec) {
            debug::log(ERR, "bluetooth: cannot replace {}: {}", destination, ec.message());
            std::filesystem::remove(tmp, ec);
            return false;
        }
        std::filesystem::remove(source, ec);
        debug::log(INFO, "bluetooth: migrated the contact cache to {} retention", to_string(to));
        return true;
    }

    ContactStore& contact_store() {
        static ContactStore store;
        return store;
    }

} // namespace tether::bluetooth
