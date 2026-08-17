#pragma once

#include "tether/bluetooth/vcard.hpp"

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

        size_t size() const { return contacts_.size(); }
        size_t indexed() const { return by_key_.size(); }
        const std::vector<VCard>& contacts() const { return contacts_; }

    private:
        std::vector<VCard> contacts_;
        std::map<std::string, std::string> by_key_;
    };

    // Cached at ~/.local/share/tether/contacts.json so names survive a restart
    // and are available before PBAP reconnects.
    std::string contacts_path();
    bool save_contacts(const ContactStore& store);
    ContactStore load_contacts();

    std::string serialize_contacts(const ContactStore& store);
    ContactStore deserialize_contacts(const std::string& text);

    // Set by the daemon; command handlers read it to label threads.
    ContactStore& contact_store();

} // namespace tether::bluetooth
