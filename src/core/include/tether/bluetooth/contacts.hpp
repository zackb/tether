#pragma once

#include "tether/bluetooth/vcard.hpp"
#include "tether/secret_store.hpp"

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace tether::bluetooth {

    // Resolves a thread key to a display name.
    class ContactStore {
    public:
        void set(std::vector<VCard> contacts);

        // Empty when the address is unknown. Keys are the same namespaced form
        // messages use, e.g. "tel:+15551234567" or "email:someone@example.com".
        std::string name_for(const std::string& thread_key) const;

        // Every address a display name maps to. Returns all matches.
        std::vector<std::string> addresses_for_name(const std::string& name) const;

        // Contacts whose name or any of their addresses contains `needle`, case- and accent-insensitively.
        // Favorites first, then by name.
        std::vector<VCard> search(const std::string& needle, size_t limit) const;

        size_t size() const { return contacts_.size(); }
        size_t indexed() const { return by_key_.size(); }
        const std::vector<VCard>& contacts() const { return contacts_; }

    private:
        std::vector<VCard> contacts_;
        std::map<std::string, std::string> by_key_;
        // Last ten digits of each contact number, for the national-vs-international fallback in name_for.
        std::map<std::string, std::string> by_tel_suffix_;
    };

    // Flags each contact that shares a phone number or email with a card from
    // the phone's Favorites list, or its name when that card has neither.
    void mark_favorites(std::vector<VCard>& contacts, const std::vector<VCard>& favorites);

    std::string contacts_path(Retention mode);
    std::string contacts_path();

    // Rewrites the cache from one mode's file into the other's and removes the
    // source. No-op unless the source exists and the destination does not.
    bool migrate_contacts(Retention from, Retention to);

    bool save_contacts(const ContactStore& store);
    ContactStore load_contacts();

    std::string serialize_contacts(const ContactStore& store);
    ContactStore deserialize_contacts(const std::string& text);

    // Set by the daemon; command handlers read it to label threads.
    ContactStore& contact_store();

} // namespace tether::bluetooth
