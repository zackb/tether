#pragma once

#include <functional>
#include <gio/gio.h>
#include <memory>
#include <string>

namespace tether::bluetooth {

    // Services Tether uses. Anything else the phone asks us to authorize during
    // pairing is refused: an accessory should not be granted profiles it has no
    // reason to consume.
    //   0x112E PBAP client, 0x112F PBAP server,
    //   0x1132 MAP server,  0x1133 MAP notification server, 0x1134 MAP client
    bool is_authorized_service(const std::string& uuid);

    // BlueZ hands passkeys over as a plain integer; Apple shows six digits with
    // leading zeros, and a mismatch here reads to the user as a failed pairing.
    std::string format_passkey(uint32_t passkey);

    struct AgentState;

    // Implements org.bluez.Agent1 for exactly one device.
    //
    // Numeric comparison only: this agent never invents, displays, or accepts a
    // PIN or passkey, and it refuses every request naming a device other than its
    // target. Confirmation is delegated to the out-of-process dialog so the
    // daemon never blocks on a user decision.
    class PairingAgent {
    public:
        // Called with the six-digit comparison code; returns true to accept.
        // Runs on the thread dispatching the agent's callbacks and may block
        // while the user decides, stalls BlueZ monitoring for the duration
        // of the prompt, which is harmless during a foreground pairing.
        using ConfirmHandler = std::function<bool(const std::string& code)>;

        PairingAgent(GDBusConnection* connection, std::string device_path, ConfirmHandler on_confirm);
        ~PairingAgent();

        PairingAgent(const PairingAgent&) = delete;
        PairingAgent& operator=(const PairingAgent&) = delete;

        // Two-phase, and the phases must run on different threads
        // export_object() binds callbacks to the calling thread's context
        // register_with_bluez() blocks on BlueZ and so must not run on that same thread.
        bool export_object();
        bool register_with_bluez();
        void unregister_with_bluez();
        void unexport_object();

        const std::string& path() const;

    private:
        std::unique_ptr<AgentState> state_;
    };

} // namespace tether::bluetooth
