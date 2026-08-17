#pragma once

#include "tether/bluetooth/config.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace tether::bluetooth {

    class BluezMonitor;

    struct PairResult {
        bool success = false;
        // Machine-readable outcome: "paired", "already_paired", "not_found",
        // "rejected", "timeout", "busy", "error".
        std::string status;
        std::string message;
        std::string device_path;
        std::string device_address;
        // True when the resulting bond covers LE as well as BR/EDR.
        bool dual_bond = false;
    };

    // Reports progress steps so the CLI and GTK app can show what is happening
    // during a transaction that legitimately takes tens of seconds.
    using ProgressFn = std::function<void(const std::string& step, const std::string& detail)>;

    // Asks the user to confirm a six-digit numeric comparison code. Returns true
    // to accept. Runs on the monitor's GLib thread.
    using ConfirmFn = std::function<bool(const std::string& code)>;

    // Runs the full pairing transaction against `address`, blocking until it
    // resolves. Intended to be called from a worker thread: GDBusConnection is
    // thread-safe for calls, while the agent's callbacks land on the monitor's
    // GLib thread.
    PairResult pair_device(BluezMonitor& monitor,
                           const std::string& address,
                           AuthStrategy strategy,
                           const ProgressFn& progress,
                           const ConfirmFn& confirm);

    // Removes the bond so a clean pairing can be retried.
    PairResult unpair_device(BluezMonitor& monitor, const std::string& address);

    nlohmann::json to_json(const PairResult& result);

    bool confirm_with_dialog(const std::string& device_name, const std::string& code);

} // namespace tether::bluetooth
