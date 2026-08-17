#pragma once

#include "tether/bluetooth/vcard.hpp"

#include <gio/gio.h>
#include <memory>
#include <string>
#include <vector>

namespace tether::bluetooth {

    // Pulling a large phonebook is a real OBEX transfer.
    inline constexpr int PBAP_CALL_TIMEOUT_MS = 60000;
    inline constexpr int PBAP_TRANSFER_TIMEOUT_SECONDS = 60;
    inline constexpr int PBAP_FILE_GRACE_SECONDS = 3;
    inline constexpr int PBAP_MAX_CONTACTS = 5000;

    struct PbapSessionState;

    // Drives org.bluez.obex.PhonebookAccess1 for one open OBEX session.
    class PbapSession {
    public:
        PbapSession(GDBusConnection* session_bus, std::string session_path);
        ~PbapSession();

        PbapSession(const PbapSession&) = delete;
        PbapSession& operator=(const PbapSession&) = delete;

        // Selects the phonebook. This is PBAP's Select("int", "pb")
        // (not MAP's SetFolder)
        bool select_phonebook(std::string& err,
                              const std::string& location = "int",
                              const std::string& phonebook = "pb");

        // Pulls the selected phonebook and parses it.
        //
        // Uses PullAll's MaxCount filter. MaxListCount is MAP's option and makes a
        // PBAP transfer fail or return nothing at all.
        std::vector<VCard> pull_all(std::string& err, int max = PBAP_MAX_CONTACTS);

        const std::string& path() const;

    private:
        std::unique_ptr<PbapSessionState> state_;
    };

} // namespace tether::bluetooth
