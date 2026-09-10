#include "verbs.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <strings.h>
#include <sys/ioctl.h>
#include <tether/bluetooth/config.hpp>
#include <tether/client.hpp>
#include <tether/core.hpp>
#include <tether/crypto.hpp>
#include <tether/discovery.hpp>
#include <tether/extension_host.hpp>
#include <tether/i18n.hpp>
#include <tether/log.hpp>
#include <tether/packaging.hpp>
#include <tether/service.hpp>
#include <tether/version.hpp>
#include <thread>
#include <unistd.h>
#include <vector>

void run_native_messaging_host(tether::Client& client) {
    debug::log(INFO, "Starting Native Messaging Host loop...\n");

    // When the browser tears down the port, stdout becomes a closed pipe. Ignore
    // SIGPIPE so a write to it fails gracefully.
    signal(SIGPIPE, SIG_IGN);

    // Subscribe to daemon broadcast events
    client.send("{\"command\":\"subscribe\"}\n");

    std::atomic<bool> running{true};

    // read asynchronous events/responses from tetherd and send to browser
    std::thread reader_thread([&]() {
        std::string buffer;
        char buf[8192];
        while (running) {
            ssize_t n = client.read(buf, sizeof(buf));
            if (n <= 0) {
                running = false;
                break;
            }
            buffer.append(buf, n);

            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string msg = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);

                if (msg.empty())
                    continue;

                uint32_t out_length = msg.length();
                std::cout.write(reinterpret_cast<const char*>(&out_length), sizeof(out_length));
                std::cout.write(msg.data(), out_length);
                std::cout.flush();
                // Browser pipe closed: stop cleanly instead of spinning on a dead fd.
                if (!std::cout) {
                    running = false;
                    break;
                }
            }
        }
    });

    // main thread reads commands from browser and forwards to tetherd
    while (running) {
        uint32_t length = 0;
        if (!std::cin.read(reinterpret_cast<char*>(&length), sizeof(length))) {
            running = false;
            break;
        }

        if (length == 0 || length > 10 * 1024 * 1024) {
            running = false;
            break;
        }

        std::vector<char> buffer(length);
        if (!std::cin.read(buffer.data(), length)) {
            running = false;
            break;
        }

        std::string payload(buffer.begin(), buffer.end());
        if (payload.empty() || payload.back() != '\n') {
            payload += "\n";
        }

        if (!client.send(payload)) {
            running = false;
            break;
        }
    }

    client.disconnect(); // unblock reader thread
    if (reader_thread.joinable()) {
        reader_thread.join();
    }
}

static bool parse_int(const char* text, int& out) {
    const char* end = text + std::strlen(text);
    int value = 0;
    auto [stop, ec] = std::from_chars(text, end, value);
    if (ec != std::errc{} || stop != end)
        return false;
    out = value;
    return true;
}

static size_t term_width() {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20)
        return ws.ws_col;
    return 80;
}

// greedy wrap on spaces. Scripts that do not space-separate words
// (CJK) yield one long line and fall through to the terminal's own soft wrap.
static std::vector<std::string> wrap(const std::string& text, size_t width) {
    std::vector<std::string> out;
    std::istringstream words(text);
    std::string word, line;
    while (words >> word) {
        if (!line.empty() && tether::display_width(line) + 1 + tether::display_width(word) > width) {
            out.push_back(line);
            line.clear();
        }
        line += line.empty() ? word : " " + word;
    }
    if (!line.empty())
        out.push_back(line);
    return out;
}

static const char* yn(bool v) { return v ? _("yes") : _("no"); }

// Label/value rows aligned on a column derived from the widest translated label
using Field = std::pair<std::string, std::string>;

static void print_fields(const std::vector<Field>& rows) {
    size_t col = 0;
    for (const auto& r : rows)
        col = std::max(col, tether::display_width(r.first));

    const std::string indent(col + 3, ' ');
    for (const auto& r : rows) {
        const std::string pad(col - tether::display_width(r.first), ' ');
        std::istringstream lines(r.second);
        std::string line;
        bool first = true;
        while (std::getline(lines, line)) {
            if (first) {
                fprintf(stdout, "%s:%s  %s\n", r.first.c_str(), pad.c_str(), line.c_str());
                first = false;
            } else {
                fprintf(stdout, "%s%s\n", indent.c_str(), line.c_str());
            }
        }
        if (first)
            fprintf(stdout, "%s:\n", r.first.c_str());
    }
}

struct Opt {
    const char* flags;
    const char* desc;
};

static const Opt kOptions[] = {
    {"-h, --help", N_("Show this help message.")},
    {"-v, --version", N_("Print the tether version.")},
    {"-g, --get-clipboard", N_("Retrieve the current Wayland clipboard text.")},
    {"-s, --set-clipboard", N_("Take string input and copy it to the local Wayland clipboard.")},
    {"-f, --send-file", N_("Send a file through tether.")},
    {"-n, --native-host", N_("Start the browser Native Messaging Host proxy loop.")},
    {"-d, --discover", N_("Scan the local network for tetherd instances via mDNS.")},
    {"--host <ip>", N_("Connect over TCP to daemon ip instead of UNIX Socket.")},
    {"--port <num>", N_("Connect over TCP port (default 5134).")},
    {"--timeout <ms>", N_("Discovery scan duration in milliseconds (default 3000).")},
    {"--list-devices", N_("List all paired connection devices.")},
    {"--bt-setup", N_("Show the one-time system setup Bluetooth still needs.")},
    {"--bt-status", N_("Show Bluetooth adapter capability and delivery mode.")},
    {"--bt-devices", N_("List Bluetooth devices known to BlueZ.")},
    {"--bt-connection", N_("Show Bluetooth link and profile connection state.")},
    {"--bt-airpods", N_("Show AirPods and case battery.")},
    {"--bt-airpods-enable", N_("Manage AirPods from Tether at all: on or off.")},
    {"--bt-airpods-mode", N_("Set the AirPods listening mode: off, anc, transparency, adaptive.")},
    {"--bt-airpods-pause", N_("Pause local playback when an AirPod is removed: never, one-removed, both-removed.")},
    {"--bt-airpods-handoff", N_("Hand the AirPods to the iPhone during a call: on or off.")},
    {"--bt-lock-on-away", N_("Lock the session when the iPhone goes out of range: on or off.")},
    {"--bt-threads", N_("List iPhone message conversations.")},
    {"--bt-messages <thread>", N_("Show messages in one conversation.")},
    {"--bt-send <thread> <text>", N_("Reply in one conversation.")},
    {"--bt-contacts [query]", N_("List iPhone contacts, or search them by name or number.")},
    {"--bt-pair <addr> [--explicit-pair]", N_("Pair with an iPhone over Bluetooth.")},
    {"--explicit-pair",
     N_("Ask the iPhone to authenticate directly instead of connecting first. Tether falls back to this on "
        "its own; use it to skip straight there.")},
    {"--bt-unpair <addr>", N_("Remove a Bluetooth bond.")},
    {"--bt-solicit",
     N_("Re-advertise for ANCS so the iPhone shows its permission toggles again, without removing "
        "the bond.")},
    {"--bt-enable <on|off>", N_("Connect to the iPhone over Bluetooth, or stop.")},
    {"--bt-ancs <on|off>", N_("Turn notification mirroring on or off.")},
    {"--bt-ancs-content <on|off>", N_("Mirror notification titles and bodies, not just the app.")},
    {"--bt-retention <encrypted|plaintext|none>",
     N_("How message history and contacts are kept on disk. Encrypted uses a key from the desktop keyring; "
        "none deletes what is stored and keeps nothing further.")},
    {"--bt-adapter <hciN|auto>", N_("Choose which Bluetooth controller to use.")},
    {"--bt-notifications", N_("List mirrored iPhone notifications.")},
    {"--bt-calls", N_("List calls on the iPhone.")},
    {"--bt-call <number>", N_("Place a call from the iPhone.")},
    {"--bt-answer", N_("Answer the ringing call.")},
    {"--bt-hangup", N_("Hang up every call.")},
    {"--bt-calls-enable <on|off>",
     N_("Turn call control on or off. Calls run over Bluetooth Hands-Free; the audio stays on the iPhone.")},
    {"--bt-call-audio <on|off>",
     N_("Play call audio on this computer, or leave it on the iPhone. Needs PipeWire to be the stack holding "
        "Hands-Free; off applies from the next call.")},
    {"--bt-diagnostics", N_("Print a redacted Bluetooth report for a bug report.")},
    {"--install-extension-host",
     N_("Install the browser and mail extension's native messaging manifests for this user. Needed only for "
        "portable builds; the distro packages install them system-wide.")},
    {"--install-btclass-unit",
     N_("Write the tether-btclass@.service unit, which sets the Bluetooth adapter class iOS requires. Needs "
        "root, and never enables it. Needed only for portable builds; the distro packages install it.")},
    {"--install-service",
     N_("Write the tetherd systemd user service for this user, and print how to enable it. Needed only for "
        "portable builds; the distro packages install it.")},
    {"--uninstall-service", N_("Remove the tetherd systemd user service this user installed.")},
    {"--status", N_("Show what the daemon is doing: devices, links, and recent transfers.")},
    {"--pending", N_("List Wi-Fi pairing requests waiting to be accepted.")},
    {"--accept <fingerprint>", N_("Accept a pending pairing request locally.")},
    {"--forget <fingerprint>", N_("Remove a Wi-Fi pairing and drop its session.")},
    {"--pair", N_("Send a pair_request over TCP to the daemon.")},
};

// The option row a subcommand stands for, matched on the long flag so the two
// lists cannot drift apart.
static const char* describe_flag(const std::string& flag) {
    for (const auto& opt : kOptions) {
        const std::string flags = opt.flags;
        const auto at = flags.find(flag);
        if (at == std::string::npos)
            continue;
        // "--pair" must not match "--bt-pair".
        const auto end = at + flag.size();
        if ((at == 0 || flags[at - 1] == ' ') && (end == flags.size() || flags[end] == ' ' || flags[end] == ','))
            return opt.desc;
    }
    return "";
}

void print_help() {
    fprintf(stdout, "%s\n\n%s\n", _("tether - Wayland companion CLI"), _("Options:"));

    size_t col = 0;
    for (const auto& opt : kOptions)
        col = std::max(col, tether::display_width(opt.flags));

    const size_t total = term_width();
    const size_t desc_width = total > col + 8 ? total - col - 4 : 40;
    for (const auto& opt : kOptions) {
        const std::string pad(col - tether::display_width(opt.flags), ' ');
        const auto lines = wrap(_(opt.desc), desc_width);
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string prefix = i == 0 ? opt.flags + pad : std::string(col, ' ');
            fprintf(stdout, "  %s  %s\n", prefix.c_str(), lines[i].c_str());
        }
    }

    fprintf(stdout, "\n%s\n", _("Commands:"));

    size_t verb_col = 0;
    for (size_t i = 0; i < tether::cli::kVerbCount; ++i)
        verb_col = std::max(verb_col, tether::display_width(tether::cli::kVerbs[i].usage));

    const size_t verb_desc_width = total > verb_col + 8 ? total - verb_col - 4 : 40;
    for (size_t i = 0; i < tether::cli::kVerbCount; ++i) {
        const auto& verb = tether::cli::kVerbs[i];
        const std::string pad(verb_col - tether::display_width(verb.usage), ' ');
        // One description per capability: a subcommand borrows its flag's.
        // gettext("") would hand back the catalog header, so ask only for text.
        const char* desc = describe_flag(verb.flag);
        auto lines = wrap(*desc ? _(desc) : "", verb_desc_width);
        if (lines.empty())
            lines.emplace_back();
        for (size_t line = 0; line < lines.size(); ++line) {
            const std::string prefix = line == 0 ? verb.usage + pad : std::string(verb_col, ' ');
            fprintf(stdout, "  %s  %s\n", prefix.c_str(), lines[line].c_str());
        }
    }

    // Literal invocations: never translated.
    fprintf(stdout,
            "  %-*s  %s\n",
            static_cast<int>(verb_col),
            "tether bt <name> ...",
            _("Any --bt-<name> flag, as a subcommand."));

    fprintf(stdout,
            "\n%s\n"
            "  tether paste\n"
            "  tether paste --host 127.0.0.1\n"
            "  echo \"pipe\" | tether copy\n"
            "  tether discover --timeout 5000\n"
            "  tether send ./report.pdf\n"
            "  tether accept 9a4f21...\n"
            "  tether bt pair AA:BB:CC:DD:EE:FF\n",
            _("Examples:"));
}

static int print_extension_host_install() {
    const auto result = tether::install_extension_host();

    for (const auto& path : result.written)
        fprintf(stdout, _("Installed %s\n"), path.c_str());

    for (const auto& err : result.errors)
        debug::log(ERR, "{}\n", err);

    if (!result.errors.empty())
        return 1;

    if (result.written.empty()) {
        fprintf(stdout,
                _("No browser or mail client profile was found for this user, so nothing was installed. Start "
                  "Firefox, Thunderbird, or Chromium once and run this again.\n"));
        return 1;
    }

    fprintf(stdout, _("\nRestart the browser or mail client to pick this up.\n"));
    return 0;
}

// The unit the distro packages install. A portable (AppImage, Flatpak) build has no way to put it there, so the
// binary gives the commands to run.
static int install_btclass_unit() {
    static constexpr const char* kPath = "/etc/systemd/system/tether-btclass@.service";

    if (geteuid() != 0) {
        debug::log(ERR, _("Writing {} needs root. Run this with sudo.\n"), kPath);
        return 1;
    }

    std::ofstream out(kPath, std::ios::trunc);
    out << tether::packaging::BTCLASS_UNIT;
    out.close();
    if (!out) {
        debug::log(ERR, _("Could not write {}.\n"), kPath);
        return 1;
    }

    fprintf(stdout, _("Installed %s\n"), kPath);
    fprintf(stdout,
            _("\nIt is not enabled. Run 'systemctl daemon-reload', then "
              "'systemctl enable --now tether-btclass@hci0' to apply it.\n"));
    return 0;
}

// The two known gaps (adapter class, experimental API) change the machine
// outside Tether, so they are printed for the user to run, never applied here.
static int print_bt_setup(tether::Client& client) {
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_status\"}\n"));
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not read Bluetooth status from the daemon.\n"));
        return 1;
    }

    if (!resp.value("available", false)) {
        fprintf(stdout, _("Bluetooth: unavailable (is bluetoothd running?)\n"));
        return 1;
    }

    const auto& cap = resp["capability"];
    const auto& setup = cap["setup"];
    if (setup.empty()) {
        fprintf(stdout, _("Bluetooth setup is complete. Nothing to do.\n"));
        return 0;
    }

    fprintf(stdout,
            P_("\n%zu step left before the iPhone Bluetooth features work:\n",
               "\n%zu steps left before the iPhone Bluetooth features work:\n",
               setup.size()),
            setup.size());

    size_t n = 0;
    for (const auto& step : setup) {
        fprintf(stdout, "\n%zu. %s\n\n", ++n, step.value("what", "").c_str());
        std::istringstream lines(step.value("command", ""));
        for (std::string line; std::getline(lines, line);)
            fprintf(stdout, "%s\n", line.c_str());
    }

    fprintf(stdout, _("\nRe-run 'tether --bt-setup' afterwards to confirm.\n"));
    return 0;
}

// /org/bluez/hci0 -> hci0
static std::string adapter_id(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Accepts either spelling: hciN or the address.
static bool adapter_known(const nlohmann::json& status, const std::string& id) {
    for (const auto& adapter : status.value("adapters", nlohmann::json::array())) {
        const std::string path = adapter.value("path", "");
        const std::string address = adapter.value("address", "");
        if (strcasecmp(adapter_id(path).c_str(), id.c_str()) == 0 || strcasecmp(address.c_str(), id.c_str()) == 0)
            return true;
    }
    return false;
}

static int print_bt_status(tether::Client& client) {
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_status\"}\n"));
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not read Bluetooth status from the daemon.\n"));
        return 1;
    }

    if (!resp.value("available", false)) {
        fprintf(stdout, _("Bluetooth: unavailable (is bluetoothd running?)\n"));
        return 1;
    }

    const auto& cap = resp["capability"];
    std::vector<Field> fields;
    fields.emplace_back(_("Mode"), cap.value("mode", "unknown"));
    fields.emplace_back(_("Bearer API"), cap.value("bearer_api", "unknown"));

    const std::string secure = cap.contains("secure_connections") && cap["secure_connections"].is_boolean()
                                   ? (cap["secure_connections"].get<bool>() ? "on" : "OFF")
                                   : _("unknown");
    const std::string in_use = cap.value("adapter_id", "");
    fields.emplace_back(_("Adapter"),
                        (in_use.empty() ? std::string{} : in_use + "  ") +
                            tether::tr_format(_("powered={0} central={1} peripheral={2} advertising={3} class={4}"),
                                              yn(cap.value("powered", false)),
                                              yn(cap.value("le_central", false)),
                                              yn(cap.value("le_peripheral", false)),
                                              yn(cap.value("advertising", false)),
                                              cap.value("class_ok", false) ? _("ok") : _("wrong")) +
                            "\n" + tether::tr_format(_("secure-connections={}"), secure));

    if (cap.value("bonded_device_present", false))
        fields.emplace_back(_("Bond"), cap.value("bond_has_le", false) ? "BR/EDR + LE" : "BR/EDR only");
    // Mirroring off is otherwise invisible here, and it takes the ANCS
    // solicitation off air, so the iPhone never offers notification access.
    fields.emplace_back(_("Notifications"),
                        resp.value("ancs_enabled", true) ? _("mirroring on")
                                                         : _("mirroring off (tether --bt-ancs on)"));
    fields.emplace_back(_("Tether"), resp.value("enabled", true) ? _("connecting") : _("off (--bt-enable on)"));

    if (resp.value("retention", "encrypted") == "encrypted" && !resp.value("retention_ready", true))
        fields.emplace_back(_("History"),
                            _("paused - the desktop keyring has no key to offer yet.\n"
                              "Unlock it, or run 'tether --bt-retention plaintext'."));
    print_fields(fields);

    for (const auto& adapter : resp["adapters"]) {
        const std::string id = adapter_id(adapter.value("path", ""));
        const std::string marker = id == in_use ? std::string("  <- ") + _("in use") : std::string{};
        fprintf(stdout,
                "  %-5s  %s  %s%s\n",
                id.c_str(),
                adapter.value("address", "").c_str(),
                adapter.value("name", "").c_str(),
                marker.c_str());
    }

    const std::string pinned = resp.value("adapter", "");
    if (!pinned.empty() && !adapter_known(resp, pinned))
        fprintf(
            stdout,
            "\n  %s\n",
            tether::tr_format(_("Controller {} is not present; using the first powered one instead."), pinned).c_str());

    if (cap.contains("reasons") && !cap["reasons"].empty()) {
        fprintf(stdout, "\n");
        for (const auto& reason : cap["reasons"])
            fprintf(stdout, "  - %s\n", reason.get<std::string>().c_str());
    }

    const size_t pending = cap.contains("setup") ? cap["setup"].size() : 0;
    if (pending > 0)
        fprintf(stdout,
                P_("\n%zu setup step remaining. Run: tether --bt-setup\n",
                   "\n%zu setup steps remaining. Run: tether --bt-setup\n",
                   pending),
                pending);
    return 0;
}

static int print_bt_devices(tether::Client& client) {
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_list_devices\"}\n"));
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not read Bluetooth devices from the daemon.\n"));
        return 1;
    }

    const auto& devices = resp["devices"];
    if (devices.empty()) {
        fprintf(stdout, _("No Bluetooth devices known to BlueZ.\n"));
        return 0;
    }

    for (const auto& d : devices) {
        std::string flags;
        auto add = [&flags](const char* name) {
            if (!flags.empty())
                flags += ",";
            flags += name;
        };
        if (d.value("bonded", false))
            add("bonded");
        if (d.value("connected", false))
            add("connected");
        if (d.value("le_connected", false))
            add("le");
        if (d.value("map", false))
            add("map");
        if (d.value("pbap", false))
            add("pbap");
        if (d.value("ancs", false))
            add("ancs");
        if (d.value("airpods", false))
            add("airpods");
        if (d.value("preferred_bearer", std::string()) == "bredr" && !d.value("le_connected", false))
            add("pinned-bredr");

        fprintf(stdout,
                "  %-18s %-24s %s%s\n",
                d.value("address", "").c_str(),
                d.value("name", "").c_str(),
                flags.c_str(),
                d.value("iphone", false) ? "  <- iPhone" : "");
    }
    return 0;
}

static int print_bt_airpods(tether::Client& client) {
    nlohmann::json resp;
    nlohmann::json status;
    try {
        resp = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_airpods\"}\n"));
        status = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_status\"}\n"));
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not read the AirPods battery from the daemon.\n"));
        return 1;
    }

    if (resp.value("address", "").empty()) {
        fprintf(stdout,
                "%s",
                status.value("airpods_enabled", false) ? _("AirPods management is off.\n")
                                                       : _("No AirPods connected.\n"));
        return 0;
    }

    fprintf(stdout, "%s  (%s)\n", resp.value("name", "").c_str(), resp.value("address", "").c_str());

    const auto level = [&](const char* label, const char* key) {
        const int percent = resp.value(key, -1);
        if (percent >= 0)
            fprintf(stdout, "  %-6s %d%%\n", label, percent);
        else
            fprintf(stdout, "  %-6s --\n", label);
    };
    level(_("Left"), "left");
    level(_("Right"), "right");
    level(_("Case"), "case");

    if (resp.contains("anc") && resp["anc"].is_string())
        fprintf(stdout, "  %-6s %s\n", _("Mode"), resp["anc"].get<std::string>().c_str());

    if (resp.contains("ear") && resp["ear"].is_object()) {
        const std::string primary = resp["ear"].value("primary", "unknown");
        const std::string secondary = resp["ear"].value("secondary", "unknown");
        if (primary != "unknown" || secondary != "unknown")
            fprintf(stdout, "  %-6s %s, %s\n", _("Worn"), primary.c_str(), secondary.c_str());
    }

    const std::string reason = resp.value("reason", "");
    if (!reason.empty())
        fprintf(stdout, "  %s\n", reason.c_str());

    if (!status.value("apple_device_id", false))
        fprintf(stdout,
                "  %s\n",
                _("This computer does not present itself as Apple hardware, so a call hands the buds over by "
                  "disconnecting them. See DeviceID in /etc/bluetooth/main.conf."));
    return 0;
}

static int set_bt_airpods_mode(tether::Client& client, const std::string& mode) {
    nlohmann::json request;
    request["command"] = "bt_airpods_mode";
    request["mode"] = mode;
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait(request.dump() + "\n"));
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not reach the daemon.\n"));
        return 1;
    }
    if (resp.value("success", false)) {
        fprintf(stdout, _("Listening mode set to %s.\n"), mode.c_str());
        return 0;
    }
    debug::log(ERR, "{}\n", resp.value("message", std::string(_("The listening mode could not be set."))));
    return 1;
}

static int set_bt_airpods_pause(tether::Client& client, const std::string& mode) {
    if (tether::bluetooth::to_string(tether::bluetooth::pause_mode_from_string(mode)) != mode) {
        debug::log(ERR, _("Use never, one-removed or both-removed.\n"));
        return 1;
    }
    if (!client.send(nlohmann::json{{"command", "bt_airpods_pause"}, {"mode", mode}}.dump() + "\n")) {
        debug::log(ERR, _("Could not reach the daemon.\n"));
        return 1;
    }
    fprintf(stdout, _("Pause on removal set to %s.\n"), mode.c_str());
    return 0;
}

static int set_bt_airpods_enable(tether::Client& client, const std::string& value) {
    if (value != "on" && value != "off") {
        debug::log(ERR, _("Use on or off.\n"));
        return 1;
    }
    if (!client.send(nlohmann::json{{"command", "bt_airpods_enable"}, {"enabled", value == "on"}}.dump() + "\n")) {
        debug::log(ERR, _("Could not reach the daemon.\n"));
        return 1;
    }
    fprintf(stdout,
            "%s\n",
            value == "on" ? _("Tether is managing the AirPods.")
                          : _("Tether has released the AirPods channel for another program."));
    return 0;
}

static int set_bt_airpods_handoff(tether::Client& client, const std::string& value) {
    if (value != "on" && value != "off") {
        debug::log(ERR, _("Use on or off.\n"));
        return 1;
    }
    if (!client.send(nlohmann::json{{"command", "bt_airpods_handoff"}, {"enabled", value == "on"}}.dump() + "\n")) {
        debug::log(ERR, _("Could not reach the daemon.\n"));
        return 1;
    }
    fprintf(stdout,
            "%s\n",
            value == "on" ? _("An iPhone call will hand the AirPods to the phone and take them back after.")
                          : _("AirPods handoff is off."));
    return 0;
}

static int set_bt_lock_on_away(tether::Client& client, const std::string& value) {
    if (value != "on" && value != "off") {
        debug::log(ERR, _("Use on or off.\n"));
        return 1;
    }
    if (!client.send(nlohmann::json{{"command", "bt_set_lock_on_away"}, {"enabled", value == "on"}}.dump() + "\n")) {
        debug::log(ERR, _("Could not reach the daemon.\n"));
        return 1;
    }
    fprintf(stdout,
            "%s\n",
            value == "on" ? _("The session will lock when the iPhone goes out of range.")
                          : _("Locking on the iPhone going out of range is off."));
    return 0;
}

// Pairing takes tens of seconds and reports progress as it goes, so this
// subscribes and streams events until the terminal result arrives.
static int run_bt_transaction(tether::Client& client,
                              const std::string& command,
                              const std::string& address = "",
                              const nlohmann::json& extra = nlohmann::json::object()) {
    const std::string result_command = command + "_result";

    client.send("{\"command\":\"subscribe\"}\n");
    nlohmann::json request;
    request["command"] = command;
    if (!address.empty())
        request["address"] = address;
    request.update(extra);
    if (!client.send(request.dump() + "\n")) {
        debug::log(ERR, _("Could not reach the daemon.\n"));
        return 1;
    }

    std::string buffer;
    char buf[8192];
    while (true) {
        ssize_t n = client.read(buf, sizeof(buf));
        if (n <= 0) {
            debug::log(ERR, _("Daemon closed the connection before the operation finished.\n"));
            return 1;
        }
        buffer.append(buf, n);

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (line.empty())
                continue;

            nlohmann::json event;
            try {
                event = nlohmann::json::parse(line);
            } catch (const std::exception&) {
                continue;
            }

            const std::string cmd = event.value("command", "");
            if (cmd == "bt_pair_progress") {
                fprintf(stdout, "  %-12s %s\n", event.value("step", "").c_str(), event.value("detail", "").c_str());
                fflush(stdout);
            } else if (cmd == "bt_pair_confirm_request") {
                const std::string code = event.value("code", "");
                bool accept = false;
                if (isatty(STDIN_FILENO)) {
                    fprintf(stdout,
                            _("\n  The iPhone should be showing this code: %s\n  Does it match? [y/N] "),
                            code.c_str());
                    fflush(stdout);
                    char answer[16] = {0};
                    if (fgets(answer, sizeof(answer), stdin))
                        accept = answer[0] == 'y' || answer[0] == 'Y';
                } else {
                    fprintf(stdout, _("\n  Cannot confirm the pairing code: input is not a terminal.\n"));
                    fflush(stdout);
                }
                client.send(nlohmann::json({{"command", "bt_pair_confirm"}, {"accept", accept}}).dump() + "\n");
            } else if (cmd == result_command) {
                fprintf(stdout, "\n%s\n", event.value("message", "").c_str());
                return event.value("success", false) ? 0 : 1;
            }
        }
    }
}

static int print_bt_connection(tether::Client& client) {
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_connection\"}\n"));
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not read Bluetooth connection state from the daemon.\n"));
        return 1;
    }

    std::vector<Field> fields;
    fields.emplace_back(_("Device present"), yn(resp.value("device_present", false)));
    fields.emplace_back(_("BR/EDR"), yn(resp.value("classic_connected", false)));
    fields.emplace_back(_("LE"),
                        std::string(yn(resp.value("le_connected", false))) +
                            (resp.value("le_available", false) ? "" : " " + std::string(_("(bearer unavailable)"))));

    std::string map = yn(resp.value("map_open", false));
    if (resp.value("map_error", std::string("none")) != "none")
        map += "  [" + resp.value("map_error", "") + "]";
    fields.emplace_back(_("Messages (MAP)"), map);

    std::string pbap = yn(resp.value("pbap_open", false));
    if (resp.value("pbap_error", std::string("none")) != "none")
        pbap += "  [" + resp.value("pbap_error", "") + "]";
    fields.emplace_back(_("Contacts (PBAP)"), pbap);

    fields.emplace_back(_("Notifications"), yn(resp.value("ancs_ready", false)));

    // Null while call control is off, which is the common case.
    const nlohmann::json calls = resp.value("calls", nlohmann::json());
    std::string call_state = yn(calls.is_object() && calls.value("available", false));
    // PipeWire's gateway reports no cellular indicators :(
    if (calls.is_object() && calls.value("available", false) && calls.value("indicators", true)) {
        // Carrier and signal come from the phone over HFP.
        const std::string carrier = calls.value("operator", "");
        call_state += "  [" + (carrier.empty() ? std::string(_("no carrier")) : carrier) +
                      tether::tr_format(_(" signal {0}/5 battery {1}/5"),
                                        std::to_string(calls.value("signal", 0)),
                                        std::to_string(calls.value("battery", 0))) +
                      (calls.value("roaming", false) ? std::string(" ") + _("roaming") : std::string{}) + "]";
    }
    fields.emplace_back(_("Calls (HFP)"), call_state);
    print_fields(fields);

    const std::string link = resp.value("link_reason", "");
    const std::string profiles = resp.value("profile_reason", "");
    const std::string notifications = resp.value("ancs_reason", "");
    const std::string call_reason = calls.is_object() ? calls.value("reason", "") : "";
    if (!link.empty())
        fprintf(stdout, "\n  %s\n", link.c_str());
    if (!profiles.empty())
        fprintf(stdout, "  %s\n", profiles.c_str());
    if (!notifications.empty())
        fprintf(stdout, "  %s\n", notifications.c_str());
    if (!call_reason.empty())
        fprintf(stdout, "  %s\n", call_reason.c_str());
    return 0;
}

// The daemon redacts; this only formats. Anything printed here is already safe
// to paste into an issue.
static int print_bt_diagnostics(tether::Client& client) {
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_diagnostics\"}\n"));
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not read Bluetooth diagnostics from the daemon.\n"));
        return 1;
    }

    fprintf(stdout, "%s\n", resp.dump(2).c_str());
    return 0;
}

static std::string format_time(int64_t epoch) {
    if (epoch <= 0)
        return "";
    std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

// Reports the daemon's own bt_call_result rather than assuming the send worked.
static int run_call_command(tether::Client& client, const nlohmann::json& request, const char* done) {
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait(request.dump() + "\n"));
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not reach the daemon.\n"));
        return 1;
    }
    if (!resp.value("success", false)) {
        debug::log(ERR, "{}\n", resp.value("message", _("The call could not be placed.")));
        return 1;
    }
    fprintf(stdout, "%s\n", done);
    return 0;
}

static int print_bt_calls(tether::Client& client) {
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_list_calls\"}\n"));
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not read calls from the daemon.\n"));
        return 1;
    }

    const auto& calls = resp["calls"];
    if (calls.empty()) {
        fprintf(stdout, _("No calls. Check 'tether --bt-connection'.\n"));
        return 0;
    }

    for (const auto& call : calls) {
        const std::string number = call.value("number", "");
        const std::string name = call.value("name", "");
        fprintf(stdout,
                "%-12s %-24s %s\n",
                call.value("state", "").c_str(),
                (number.empty() ? _("withheld") : number.c_str()),
                name.c_str());
        fprintf(stdout, "  %s\n", call.value("path", "").c_str());
    }
    return 0;
}

static int print_bt_notifications(tether::Client& client) {
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_list_notifications\"}\n"));
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not read notifications from the daemon.\n"));
        return 1;
    }

    const auto& notifications = resp["notifications"];
    if (notifications.empty()) {
        fprintf(stdout, _("No notifications yet. Check 'tether --bt-connection'.\n"));
        return 0;
    }

    for (const auto& n : notifications) {
        const std::string app = n.value("app_name", n.value("app_id", ""));
        const std::string title = n.value("title", "");
        const std::string subtitle = n.value("subtitle", "");
        const std::string body = n.value("body", "");
        fprintf(stdout,
                "%s  %s%s%s\n",
                format_time(n.value("timestamp", static_cast<int64_t>(0))).c_str(),
                app.c_str(),
                title.empty() ? "" : "  -  ",
                title.c_str());
        for (const std::string& line : {subtitle, body}) {
            if (!line.empty())
                fprintf(stdout, "    %s\n", line.c_str());
        }
    }
    return 0;
}

static int print_bt_threads(tether::Client& client) {
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_list_threads\"}\n"));
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not read conversations from the daemon.\n"));
        return 1;
    }

    const auto& threads = resp["threads"];
    if (threads.empty()) {
        fprintf(stdout, _("No conversations yet. Check 'tether --bt-connection'.\n"));
        return 0;
    }

    for (const auto& t : threads) {
        const int unread = t.value("unread", 0);
        std::string preview = t.value("preview", "");
        if (preview.size() > 48)
            preview = preview.substr(0, 45) + "...";
        for (char& c : preview)
            if (c == '\n' || c == '\r')
                c = ' ';

        const std::string unread_note =
            unread > 0 ? "  " + tether::tr_format(P_("({} unread)", "({} unread)", unread), unread) : "";
        fprintf(stdout,
                "  %-24s %-18s %s%s\n",
                t.value("name", "").c_str(),
                format_time(t.value("timestamp", (int64_t)0)).c_str(),
                preview.c_str(),
                unread_note.c_str());
        fprintf(stdout, "      %s\n", t.value("thread", "").c_str());
    }
    return 0;
}

// A package upgrade replaces the binaries but leaves the old tetherd running.
static void warn_if_daemon_is_older(tether::Client& client) {
    std::string running;
    try {
        auto status = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_status\"}\n"));
        running = status.value("version", "");
    } catch (const std::exception&) {
        return;
    }
    if (running == TETHER_VERSION)
        return;

    // No version field at all means a daemon older than the field itself.
    debug::log(ERR,
               _("The running tetherd is {}, but this is tether {}. Stop it and it\n"
                 "restarts on demand:\n"
                 "    pkill tetherd\n"),
               running.empty() ? _("an older build") : running.c_str(),
               TETHER_VERSION);
}

static int print_bt_contacts(tether::Client& client, const std::string& query) {
    nlohmann::json request;
    request["command"] = "bt_list_contacts";
    request["query"] = query;

    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait(request.dump() + "\n"));
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not read contacts from the daemon.\n"));
        return 1;
    }

    const auto& contacts = resp["contacts"];
    if (contacts.empty()) {
        if (query.empty())
            fprintf(stdout, _("No contacts yet. Check 'tether --bt-connection'.\n"));
        else
            fprintf(stdout, _("No contacts match %s\n"), query.c_str());
        return 0;
    }

    // Name then addresses, one address per line
    for (const auto& c : contacts) {
        // A vCard with addresses but no FN would start its block with a blank line
        const std::string name = c.value("name", "");
        if (!name.empty())
            fprintf(stdout, "  %s\n", name.c_str());
        for (const auto& address : c["addresses"])
            fprintf(stdout, "      %s\n", address.get<std::string>().c_str());
    }
    return 0;
}

static int print_bt_messages(tether::Client& client, const std::string& thread) {
    nlohmann::json request;
    request["command"] = "bt_list_messages";
    request["thread"] = thread;

    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait(request.dump() + "\n"));
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not read messages from the daemon.\n"));
        return 1;
    }

    const auto& messages = resp["messages"];
    if (messages.empty()) {
        fprintf(stdout, _("No messages in %s\n"), thread.c_str());
        return 0;
    }

    for (const auto& m : messages) {
        fprintf(stdout,
                "%s  %-8s %s%s\n",
                format_time(m.value("timestamp", (int64_t)0)).c_str(),
                m.value("outgoing", false) ? _("me") : _("them"),
                m.value("body", "").c_str(),
                m.value("read", true) ? "" : "  *");
    }
    return 0;
}

static int send_bt_message(tether::Client& client, const std::string& thread, const std::string& body) {
    nlohmann::json request;
    request["command"] = "bt_send_message";
    request["thread"] = thread;
    request["body"] = body;

    // The daemon answers asynchronously once the phone has accepted or refused
    // the message, so the result arrives on the event stream rather than as a
    // reply to the request.
    client.send("{\"command\":\"subscribe\"}\n");
    if (!client.send(request.dump() + "\n")) {
        debug::log(ERR, _("Could not reach the daemon.\n"));
        return 1;
    }

    std::string buffer;
    char buf[8192];
    while (true) {
        ssize_t n = client.read(buf, sizeof(buf));
        if (n <= 0) {
            debug::log(ERR, _("Daemon closed the connection before the message was sent.\n"));
            return 1;
        }
        buffer.append(buf, n);

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (line.empty())
                continue;

            nlohmann::json event;
            try {
                event = nlohmann::json::parse(line);
            } catch (const std::exception&) {
                continue;
            }

            if (event.value("command", "") != "bt_send_result")
                continue;
            if (event.value("success", false)) {
                fprintf(stdout, _("Sent to %s\n"), thread.c_str());
                return 0;
            }
            debug::log(ERR, "{}\n", event.value("message", _("The message was not sent.")));
            return 1;
        }
    }
}

// The daemon answers one request at a time here, but be strict about it: parse
// the first line only, so a stray broadcast cannot turn into a parse error.
static nlohmann::json parse_first_line(const std::string& response) {
    const auto newline = response.find('\n');
    return nlohmann::json::parse(newline == std::string::npos ? response : response.substr(0, newline));
}

static nlohmann::json state_snapshot(tether::Client& client) {
    return parse_first_line(client.send_and_wait("{\"command\":\"state_snapshot\"}\n"));
}

static int print_status(tether::Client& client) {
    nlohmann::json snap;
    try {
        snap = state_snapshot(client);
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not read the daemon's state.\n"));
        return 1;
    }

    // Defaults rather than lookups: a daemon that predates a field should
    // still give a readable status.
    const auto paired = snap.value("paired_devices", nlohmann::json::array());
    const auto pending = snap.value("pending_pairs", nlohmann::json::array());
    const auto clients = snap.value("connected_clients", nlohmann::json::array());

    std::vector<Field> fields;
    fields.emplace_back(_("Version"), tether::get_version());
    fields.emplace_back(_("Daemon"), _("running"));
    fields.emplace_back(_("Clipboard"),
                        snap.value("clipboard_available", false) ? _("syncing")
                                                                 : _("off, no Wayland session on this machine"));
    fields.emplace_back(_("mDNS"), snap.value("mdns_available", false) ? _("advertising") : _("unavailable"));
    fields.emplace_back(_("Firewall"), yn(snap.value("firewall_active", false)));
    fields.emplace_back(_("Paired devices"), std::to_string(paired.size()));
    fields.emplace_back(_("Pending requests"), std::to_string(pending.size()));
    fields.emplace_back(_("Connected now"), std::to_string(clients.size()));
    print_fields(fields);

    for (const auto& c : clients) {
        fprintf(stdout,
                "\n  %s  %s  %s\n",
                c.value("device_name", "?").c_str(),
                c.value("address", "?").c_str(),
                c.value("paired", false) ? _("paired") : _("unpaired"));
    }

    const auto files = snap.value("recent_received_files", nlohmann::json::array());
    if (!files.empty()) {
        fprintf(stdout, "\n%s\n", _("Recently received:"));
        for (const auto& f : files)
            fprintf(stdout, "  %s\n", f.value("path", "").c_str());
    }

    // The count is already a field above, so this only says what to do about it.
    if (!pending.empty())
        fprintf(stdout, _("\nRun 'tether --pending' to see what is waiting to be accepted.\n"));

    return 0;
}

// The headless half of pairing: with no GTK app to show the prompt, this is how
// a request is seen before 'tether --accept' takes it.
static int print_pending(tether::Client& client) {
    nlohmann::json snap;
    try {
        snap = state_snapshot(client);
    } catch (const std::exception&) {
        debug::log(ERR, _("Could not read the daemon's state.\n"));
        return 1;
    }

    const auto pending = snap.value("pending_pairs", nlohmann::json::array());
    if (pending.empty()) {
        fprintf(stdout, _("No pairing requests are waiting.\n"));
        return 0;
    }

    for (const auto& item : pending)
        fprintf(stdout, "  %-20s  %s\n", item.value("device_name", "?").c_str(), item.value("fingerprint", "").c_str());

    fprintf(stdout, _("\nAccept one with: tether --accept <fingerprint>\n"));
    return 0;
}

// systemd is what runs the unit, so writing one where there is no systemd would
// leave a dead file and instructions that go nowhere.
static bool has_systemd() {
    if (!tether::which_program("systemctl").empty())
        return true;
    fprintf(stdout,
            _("No systemd on this machine, so there is no user service to manage. Start tetherd from "
              "whatever this system uses for services, or let a client start it on demand.\n"));
    return false;
}

static int install_service() {
    if (!has_systemd())
        return 1;

    const auto result = tether::install_tetherd_service();

    for (const auto& err : result.errors)
        debug::log(ERR, "{}\n", err);
    if (!result.errors.empty())
        return 1;

    fprintf(stdout, _("Installed %s\n"), result.path.c_str());
    fprintf(stdout,
            _("\nTether does not enable it for you. To start the daemon now and on every login:\n\n"
              "systemctl --user daemon-reload\n"
              "systemctl --user enable --now tetherd.service\n\n"
              "On a server you log out of, keep the user session alive too:\n\n"
              "loginctl enable-linger $USER\n"));
    return 0;
}

static int uninstall_service() {
    if (!has_systemd())
        return 1;

    const auto result = tether::uninstall_tetherd_service();

    for (const auto& err : result.errors)
        debug::log(ERR, "{}\n", err);
    if (!result.errors.empty())
        return 1;

    if (!result.changed) {
        fprintf(stdout, _("No tetherd user service was installed for this user.\n"));
        return 0;
    }

    fprintf(stdout, _("Removed %s\n"), result.path.c_str());
    fprintf(stdout, _("\nRun 'systemctl --user daemon-reload' to forget it.\n"));
    return 0;
}

int main(int argc, char* argv[]) {
    tether::init_locale();

    // Subcommands are rewritten into the flags below, so there is one parser.
    const std::vector<std::string> expanded = tether::cli::expand_verbs({argv, argv + argc});
    std::vector<char*> rewritten;
    rewritten.reserve(expanded.size());
    for (const auto& arg : expanded)
        rewritten.push_back(const_cast<char*>(arg.c_str()));
    argc = static_cast<int>(rewritten.size());
    argv = rewritten.data();

    if (argc < 2) {
        print_help();
        return 0;
    }

    std::string action, arg_val, arg_val2, host = "";
    bool explicit_pair = false;
    int port = 5134;
    int timeout_ms = 3000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            fprintf(stdout, "tether %s\n", tether::get_version().c_str());
            return 0;
        } else if (arg == "-g" || arg == "--get-clipboard") {
            action = "get";
        } else if (arg == "-d" || arg == "--discover") {
            action = "discover";
        } else if (arg == "--list-devices") {
            action = "list";
        } else if (arg == "--install-extension-host") {
            action = "install_extension_host";
        } else if (arg == "--install-btclass-unit") {
            action = "install_btclass_unit";
        } else if (arg == "--install-service") {
            action = "install_service";
        } else if (arg == "--uninstall-service") {
            action = "uninstall_service";
        } else if (arg == "--status") {
            action = "status";
        } else if (arg == "--pending") {
            action = "pending";
        } else if (arg == "--bt-setup") {
            action = "bt_setup";
        } else if (arg == "--bt-status") {
            action = "bt_status";
        } else if (arg == "--bt-devices") {
            action = "bt_devices";
        } else if (arg == "--bt-connection") {
            action = "bt_connection";
        } else if (arg == "--bt-airpods") {
            action = "bt_airpods";
        } else if (arg == "--bt-airpods-enable") {
            action = "bt_airpods_enable";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-airpods-mode") {
            action = "bt_airpods_mode";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-airpods-pause") {
            action = "bt_airpods_pause";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-airpods-handoff") {
            action = "bt_airpods_handoff";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-lock-on-away") {
            action = "bt_lock_on_away";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-diagnostics") {
            action = "bt_diagnostics";
        } else if (arg == "--bt-threads") {
            action = "bt_threads";
        } else if (arg == "--bt-contacts") {
            action = "bt_contacts";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-messages") {
            action = "bt_messages";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-send") {
            action = "bt_send";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
            if (i + 1 < argc)
                arg_val2 = argv[++i];
        } else if (arg == "--bt-solicit") {
            action = "bt_solicit";
        } else if (arg == "--bt-enable") {
            action = "bt_enable";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-ancs") {
            action = "bt_ancs";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-ancs-content") {
            action = "bt_ancs_content";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-retention") {
            action = "bt_retention";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-adapter") {
            action = "bt_adapter";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-notifications") {
            action = "bt_notifications";
        } else if (arg == "--bt-calls") {
            action = "bt_calls";
        } else if (arg == "--bt-answer") {
            action = "bt_call_action";
            arg_val = "answer";
        } else if (arg == "--bt-hangup") {
            action = "bt_call_action";
            arg_val = "hangup_all";
        } else if (arg == "--bt-call-audio") {
            action = "bt_call_action";
            const std::string value = i + 1 < argc ? argv[++i] : "";
            arg_val = value == "off" ? "audio_phone" : "audio_here";
        } else if (arg == "--bt-call") {
            action = "bt_call";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-calls-enable") {
            action = "bt_calls_enable";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--bt-pair") {
            action = "bt_pair";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--explicit-pair") {
            explicit_pair = true;
        } else if (arg == "--bt-unpair") {
            action = "bt_unpair";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--pair") {
            action = "pair";
        } else if (arg == "--accept") {
            action = "accept";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--forget") {
            action = "forget";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "--host") {
            if (i + 1 < argc && argv[i + 1][0] != '-')
                host = argv[++i];
        } else if (arg == "--port") {
            if (i + 1 < argc && argv[i + 1][0] != '-' && !parse_int(argv[++i], port)) {
                debug::log(ERR, _("--port expects a number.\n"));
                return 1;
            }
        } else if (arg == "--timeout") {
            if (i + 1 < argc && argv[i + 1][0] != '-' && !parse_int(argv[++i], timeout_ms)) {
                debug::log(ERR, _("--timeout expects a number in milliseconds.\n"));
                return 1;
            }
        } else if (arg == "-f" || arg == "--send-file") {
            action = "file";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        } else if (arg == "-n" || arg == "--native-host") {
            action = "native";
        } else if (arg == "-s" || arg == "--set-clipboard") {
            action = "set";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
        }
    }

    if (action.empty()) {
        debug::log(ERR, _("Unknown action. Run tether --help for options\n"));
        return 1;
    }

    if (action == "install_extension_host")
        return print_extension_host_install();

    if (action == "install_btclass_unit")
        return install_btclass_unit();

    if (action == "install_service")
        return install_service();

    if (action == "uninstall_service")
        return uninstall_service();

    tether::Client client;

    // Fast-path local operations that don't need active daemon connection fundamentally
    if (action == "list") {
        debug::log(INFO, "{}", client.list_devices());
        return 0;
    } else if (action == "discover") {
        debug::log(INFO, "Scanning for tetherd instances on the local network...\n\n");

        tether::Crypto::instance().init();

        tether::Discovery discovery;
        auto hosts = discovery.discover(timeout_ms);
        auto devices = tether::group_discovered_hosts(hosts, tether::Crypto::instance().get_my_fingerprint());

        if (devices.empty()) {
            debug::log(INFO, _("  No tetherd instances found.\n"));
        } else {
            for (const auto& dev : devices) {
                std::string status = _("[new]");
                if (!dev.fingerprint.empty() && tether::Crypto::instance().is_host_known(dev.fingerprint)) {
                    status = _("[known]");
                }
                fprintf(stdout, "  %-20s  %s\n", dev.name.c_str(), status.c_str());
                for (const auto& addr : dev.addresses) {
                    fprintf(stdout, "    %s:%d\n", addr.address.c_str(), addr.port);
                }
                fprintf(stdout, "\n");
            }
        }

        fprintf(stdout,
                P_("Found %zu device in %.1fs\n", "Found %zu devices in %.1fs\n", devices.size()),
                devices.size(),
                timeout_ms / 1000.0);
        return 0;
    }

    if (action == "set" && arg_val.empty()) {
        std::string line;
        while (std::getline(std::cin, line))
            arg_val += line + "\n";
        if (!arg_val.empty() && arg_val.back() == '\n')
            arg_val.pop_back();
    }

    // Bind network abstraction natively
    if (!client.connect(host, port)) {
        debug::log(
            ERR,
            _("Explicit framework connection locally rejected! Did you target explicitly invalid TLS or is daemon "
              "broken?\n"));
        return 1;
    }

    if (action == "status")
        return print_status(client);

    if (action == "pending")
        return print_pending(client);

    std::string err;
    if (action == "accept") {
        if (arg_val.empty() || !client.accept_device(arg_val)) {
            debug::log(ERR, _("Could not reach the daemon.\n"));
            return 1;
        }
        debug::log(INFO, "Successfully paired device: {}", arg_val);
    } else if (action == "forget") {
        if (arg_val.empty()) {
            debug::log(ERR, _("--forget expects a fingerprint.\n"));
            return 1;
        }
        if (!client.forget_device(arg_val)) {
            debug::log(ERR, _("No paired device with that fingerprint.\n"));
            return 1;
        }
        debug::log(INFO, "Forgot device: {}", arg_val);
    } else if (action == "pair") {
        char hostname[256] = {};
        gethostname(hostname, sizeof(hostname) - 1);
        std::string resp = client.pair(hostname, err);
        if (!err.empty()) {
            debug::log(ERR, "{}", err);
            return 1;
        }
        debug::log(INFO, "Pairing response: {}", resp);
    } else if (action == "get") {
        std::string clip = client.get_clipboard(err);
        if (!err.empty()) {
            debug::log(ERR, "{}", err);
            return 1;
        }
        debug::log(INFO, "{}", clip);
    } else if (action == "set") {
        if (!client.set_clipboard(arg_val, err)) {
            debug::log(ERR, "{}", err);
            return 1;
        }
    } else if (action == "file") {
        debug::log(INFO, "Sending {}...", arg_val);
        if (!client.send_file(arg_val, err)) {
            debug::log(ERR, _("Transfer Failed: {}"), err);
            return 1;
        }
        debug::log(INFO, "File transfer delivered.\n");
    } else if (action == "native") {
        run_native_messaging_host(client);
    }

    // The Bluetooth commands are what we're checking for version conflicts
    if (action.rfind("bt_", 0) == 0)
        warn_if_daemon_is_older(client);

    if (action == "bt_setup") {
        return print_bt_setup(client);
    } else if (action == "bt_status") {
        return print_bt_status(client);
    } else if (action == "bt_devices") {
        return print_bt_devices(client);
    } else if (action == "bt_connection") {
        return print_bt_connection(client);
    } else if (action == "bt_airpods") {
        return print_bt_airpods(client);
    } else if (action == "bt_airpods_enable") {
        return set_bt_airpods_enable(client, arg_val);
    } else if (action == "bt_airpods_mode") {
        return set_bt_airpods_mode(client, arg_val);
    } else if (action == "bt_airpods_pause") {
        return set_bt_airpods_pause(client, arg_val);
    } else if (action == "bt_airpods_handoff") {
        return set_bt_airpods_handoff(client, arg_val);
    } else if (action == "bt_lock_on_away") {
        return set_bt_lock_on_away(client, arg_val);
    } else if (action == "bt_diagnostics") {
        return print_bt_diagnostics(client);
    } else if (action == "bt_threads") {
        return print_bt_threads(client);
    } else if (action == "bt_contacts") {
        return print_bt_contacts(client, arg_val);
    } else if (action == "bt_notifications") {
        return print_bt_notifications(client);
    } else if (action == "bt_calls") {
        return print_bt_calls(client);
    } else if (action == "bt_call") {
        if (arg_val.empty()) {
            debug::log(ERR, _("A number is required, e.g. --bt-call +15551234567\n"));
            return 1;
        }
        nlohmann::json request;
        request["command"] = "bt_call_dial";
        request["number"] = arg_val;
        return run_call_command(client, request, _("Dialing."));
    } else if (action == "bt_call_action") {
        nlohmann::json request;
        request["command"] = "bt_call_action";
        request["action"] = arg_val;
        const char* done = arg_val == "answer"        ? _("Answering.")
                           : arg_val == "audio_here"  ? _("Call audio comes to this computer.")
                           : arg_val == "audio_phone" ? _("Call audio stays on the iPhone.")
                                                      : _("Hanging up.");
        return run_call_command(client, request, done);
    } else if (action == "bt_calls_enable") {
        if (arg_val != "on" && arg_val != "off") {
            debug::log(ERR, _("Expected on or off, e.g. --bt-calls-enable on\n"));
            return 1;
        }
        nlohmann::json request;
        request["command"] = "bt_set_calls";
        request["enabled"] = arg_val == "on";
        if (!client.send(request.dump() + "\n")) {
            debug::log(ERR, _("Could not reach the daemon.\n"));
            return 1;
        }
        fprintf(stdout, "%s\n", arg_val == "on" ? _("Call control enabled.") : _("Call control disabled."));
    } else if (action == "bt_messages") {
        if (arg_val.empty()) {
            debug::log(ERR, _("A thread key is required, e.g. --bt-messages tel:+15551234567\n"));
            return 1;
        }
        return print_bt_messages(client, arg_val);
    } else if (action == "bt_send") {
        if (arg_val.empty() || arg_val2.empty()) {
            debug::log(ERR, _("A conversation and a message are required, e.g. --bt-send tel:+15551234567 \"hi\"\n"));
            return 1;
        }
        return send_bt_message(client, arg_val, arg_val2);
    } else if (action == "bt_pair" || action == "bt_unpair") {
        if (arg_val.empty()) {
            debug::log(ERR, _("A Bluetooth address is required, e.g. --bt-pair AA:BB:CC:DD:EE:FF\n"));
            return 1;
        }
        nlohmann::json extra = nlohmann::json::object();
        if (action == "bt_pair" && explicit_pair)
            extra["strategy"] = "explicit-pair";
        return run_bt_transaction(client, action == "bt_pair" ? "bt_pair" : "bt_unpair", arg_val, extra);
    } else if (action == "bt_solicit") {
        return run_bt_transaction(client, "bt_solicit");
    } else if (action == "bt_enable") {
        if (arg_val != "on" && arg_val != "off") {
            debug::log(ERR, _("Expected on or off, e.g. --bt-enable off\n"));
            return 1;
        }
        nlohmann::json request;
        request["command"] = "bt_set_enabled";
        request["enabled"] = arg_val == "on";
        if (!client.send(request.dump() + "\n")) {
            debug::log(ERR, _("Could not reach the daemon.\n"));
            return 1;
        }
        if (arg_val == "on") {
            fprintf(stdout, _("Bluetooth enabled.\n"));
        } else {
            fprintf(stdout,
                    _("Bluetooth disabled. Tether will not reconnect the iPhone. A link that is\n"
                      "already up stays up until you disconnect it or the phone goes out of range.\n"));
        }
    } else if (action == "bt_ancs" || action == "bt_ancs_content") {
        const bool content = action == "bt_ancs_content";
        if (arg_val != "on" && arg_val != "off") {
            debug::log(ERR, _("Expected on or off, e.g. {} on\n"), content ? "--bt-ancs-content" : "--bt-ancs");
            return 1;
        }
        nlohmann::json request;
        request["command"] = content ? "bt_set_ancs_content" : "bt_set_ancs";
        request["enabled"] = arg_val == "on";
        if (!client.send(request.dump() + "\n")) {
            debug::log(ERR, _("Could not reach the daemon.\n"));
            return 1;
        }
        const bool on = arg_val == "on";
        fprintf(stdout,
                "%s\n",
                content ? (on ? _("Notification contents enabled.") : _("Notification contents disabled."))
                        : (on ? _("Notification mirroring enabled.") : _("Notification mirroring disabled.")));
    } else if (action == "bt_retention") {
        if (arg_val != "encrypted" && arg_val != "plaintext" && arg_val != "none") {
            debug::log(ERR, _("Expected encrypted, plaintext or none, e.g. --bt-retention encrypted\n"));
            return 1;
        }
        if (arg_val == "none")
            fprintf(stdout, _("Deleting stored messages and contacts.\n"));

        nlohmann::json request;
        request["command"] = "bt_set_retention";
        request["retention"] = arg_val;
        if (!client.send(request.dump() + "\n")) {
            debug::log(ERR, _("Could not reach the daemon.\n"));
            return 1;
        }
        fprintf(stdout, _("Retention set to %s.\n"), arg_val.c_str());
    } else if (action == "bt_adapter") {
        if (arg_val.empty()) {
            debug::log(ERR, _("Expected a controller, e.g. --bt-adapter hci1, or --bt-adapter auto\n"));
            return 1;
        }
        const bool automatic = arg_val == "auto";

        nlohmann::json status;
        try {
            status = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_status\"}\n"));
        } catch (const std::exception&) {
            debug::log(ERR, _("Could not read Bluetooth status from the daemon.\n"));
            return 1;
        }

        if (!automatic && !adapter_known(status, arg_val)) {
            debug::log(ERR, _("No Bluetooth controller matches '{}'. Available:\n"), arg_val);
            for (const auto& adapter : status.value("adapters", nlohmann::json::array()))
                fprintf(stderr,
                        "  %s  %s  %s\n",
                        adapter_id(adapter.value("path", "")).c_str(),
                        adapter.value("address", "").c_str(),
                        adapter.value("name", "").c_str());
            return 1;
        }

        nlohmann::json request;
        request["command"] = "bt_set_adapter";
        request["adapter"] = automatic ? "" : arg_val;
        if (!client.send(request.dump() + "\n")) {
            debug::log(ERR, _("Could not reach the daemon.\n"));
            return 1;
        }
        if (automatic)
            fprintf(stdout, _("Controller selection is automatic: the first powered one.\n"));
        else
            fprintf(stdout, "%s\n", tether::tr_format(_("Using controller {}."), arg_val).c_str());
    }

    return 0;
}
