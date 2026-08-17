#pragma once

#include <string>

namespace tether::bluetooth {

    // Which pairing transaction to run.
    //
    // ConnectFirst calls Device1.Connect() on the unpaired device and lets iOS
    // initiate authentication. A Linux-initiated Pair() can
    // yield a BR/EDR-only bond, which can never carry ANCS.
    //
    // ExplicitPair calls Device1.Pair() directly. Some controllers cancel the
    // Connect-first transaction, and headless callers cannot show a confirmation
    // prompt, so it stays available as a workaround.
    enum class AuthStrategy { ConnectFirst, ExplicitPair };

    // Persisted at ~/.config/tether/bluetooth.json.
    struct Config {
        // Address of the selected iPhone, e.g. "81:71:C8:30:6A:F3".
        std::string device_address;
        AuthStrategy auth_strategy = AuthStrategy::ConnectFirst;
        // Cleared when pairing resolves to compatibility mode, so later runs do
        // not keep trying to bring up an LE bearer the phone will not answer.
        bool ancs_enabled = true;
        // Whether to request notification contents rather than only the app
        // they came from. Off by default.
        bool ancs_content_enabled = false;
        // Group replies are off until deliberately enabled
        bool group_messages_enabled = false;

        bool operator==(const Config&) const = default;
    };

    const char* to_string(AuthStrategy strategy);
    AuthStrategy auth_strategy_from_string(const std::string& value);

    std::string config_path();

    Config load_config();
    bool save_config(const Config& config);

    // exposed for tests without touching fs
    std::string serialize_config(const Config& config);
    Config deserialize_config(const std::string& text);

} // namespace tether::bluetooth
