#include "tether/bluetooth/contacts.hpp"
#include "tether/bluetooth/bmessage.hpp"
#include "tether/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

    } // namespace

    void ContactStore::set(std::vector<VCard> contacts) {
        contacts_ = std::move(contacts);
        by_key_.clear();

        for (const auto& card : contacts_) {
            if (card.name.empty())
                continue;
            for (const auto& tel : card.tels) {
                std::string normalized = normalize_phone(tel);
                if (!normalized.empty())
                    by_key_.emplace("tel:" + normalized, card.name);
            }
            for (const auto& email : card.emails) {
                std::string normalized = normalize_email(email);
                if (!normalized.empty())
                    by_key_.emplace("email:" + normalized, card.name);
            }
        }
    }

    std::string ContactStore::name_for(const std::string& thread_key) const {
        auto it = by_key_.find(thread_key);
        return it == by_key_.end() ? std::string{} : it->second;
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

    std::string contacts_path() {
        std::string home = home_dir();
        if (home.empty())
            return {};
        return (std::filesystem::path(home) / ".local" / "share" / "tether" / "contacts.json").string();
    }

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
        return j.dump(2);
    }

    ContactStore deserialize_contacts(const std::string& text) {
        ContactStore store;
        std::vector<VCard> cards;
        try {
            auto j = nlohmann::json::parse(text);
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
        std::string path = contacts_path();
        if (path.empty())
            return {};
        std::ifstream in(path);
        if (!in.is_open())
            return {};
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return deserialize_contacts(text);
    }

    bool save_contacts(const ContactStore& store) {
        std::string path = contacts_path();
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
            out << serialize_contacts(store);
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

    ContactStore& contact_store() {
        static ContactStore store;
        return store;
    }

} // namespace tether::bluetooth
