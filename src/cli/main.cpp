#include <atomic>
#include <csignal>
#include <ctime>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <tether/client.hpp>
#include <tether/crypto.hpp>
#include <tether/discovery.hpp>
#include <tether/log.hpp>
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

void print_help() {
    std::cout << "tether - Wayland companion CLI\n\n"
              << "Options:\n"
              << "  -h, --help               Show this help message.\n"
              << "  -g, --get-clipboard      Retrieve the current Wayland clipboard text.\n"
              << "  -s, --set-clipboard      Take string input and copy it to the local Wayland clipboard.\n"
              << "  -f, --send-file          Send a file through tether.\n"
              << "  -n, --native-host        Start the browser Native Messaging Host proxy loop.\n"
              << "  -d, --discover           Scan the local network for tetherd instances via mDNS.\n"
              << "  --host <ip>              Connect over TCP to daemon ip instead of UNIX Socket.\n"
              << "  --port <num>             Connect over TCP port (default 5134).\n"
              << "  --timeout <ms>           Discovery scan duration in milliseconds (default 3000).\n"
              << "  --list-devices           List all paired connection devices.\n"
              << "  --bt-status              Show Bluetooth adapter capability and delivery mode.\n"
              << "  --bt-devices             List Bluetooth devices known to BlueZ.\n"
              << "  --bt-connection          Show Bluetooth link and profile connection state.\n"
              << "  --bt-threads             List iPhone message conversations.\n"
              << "  --bt-messages <thread>   Show messages in one conversation.\n"
              << "  --bt-send <thread> <text>  Reply in one conversation.\n"
              << "  --bt-pair <addr>         Pair with an iPhone over Bluetooth.\n"
              << "  --bt-unpair <addr>       Remove a Bluetooth bond.\n"
              << "  --bt-diagnostics         Print a redacted Bluetooth report for a bug report.\n"
              << "  --accept <fingerprint>   Accept a pending pairing request locally.\n"
              << "  --pair                   Send a pair_request over TCP to the daemon.\n\n"
              << "Examples:\n"
              << "  tether -g\n"
              << "  tether -g --host 127.0.0.1\n"
              << "  echo \"pipe\" | tether -s\n"
              << "  tether --discover\n"
              << "  tether --discover --timeout 5000\n"
              << "  tether -f ./report.pdf\n"
              << "  tether --accept 9a4f21...\n";
}

static int print_bt_status(tether::Client& client) {
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_status\"}\n"));
    } catch (const std::exception&) {
        debug::log(ERR, "Could not read Bluetooth status from the daemon.\n");
        return 1;
    }

    if (!resp.value("available", false)) {
        fprintf(stdout, "Bluetooth: unavailable (is bluetoothd running?)\n");
        return 1;
    }

    const auto& cap = resp["capability"];
    fprintf(stdout, "Mode:       %s\n", cap.value("mode", "unknown").c_str());
    fprintf(stdout, "Bearer API: %s\n", cap.value("bearer_api", "unknown").c_str());
    fprintf(stdout,
            "Adapter:    powered=%s central=%s peripheral=%s advertising=%s class=%s\n",
            cap.value("powered", false) ? "yes" : "no",
            cap.value("le_central", false) ? "yes" : "no",
            cap.value("le_peripheral", false) ? "yes" : "no",
            cap.value("advertising", false) ? "yes" : "no",
            cap.value("class_ok", false) ? "ok" : "wrong");

    for (const auto& adapter : resp["adapters"]) {
        fprintf(stdout, "  %s  %s\n", adapter.value("address", "").c_str(), adapter.value("name", "").c_str());
    }

    if (cap.contains("reasons") && !cap["reasons"].empty()) {
        fprintf(stdout, "\n");
        for (const auto& reason : cap["reasons"])
            fprintf(stdout, "  - %s\n", reason.get<std::string>().c_str());
    }
    return 0;
}

static int print_bt_devices(tether::Client& client) {
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_list_devices\"}\n"));
    } catch (const std::exception&) {
        debug::log(ERR, "Could not read Bluetooth devices from the daemon.\n");
        return 1;
    }

    const auto& devices = resp["devices"];
    if (devices.empty()) {
        fprintf(stdout, "No Bluetooth devices known to BlueZ.\n");
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

        fprintf(stdout,
                "  %-18s %-24s %s%s\n",
                d.value("address", "").c_str(),
                d.value("name", "").c_str(),
                flags.c_str(),
                d.value("iphone", false) ? "  <- iPhone" : "");
    }
    return 0;
}

// Pairing takes tens of seconds and reports progress as it goes, so this
// subscribes and streams events until the terminal result arrives.
static int run_bt_transaction(tether::Client& client, const std::string& command, const std::string& address) {
    const std::string result_command = command == "bt_pair" ? "bt_pair_result" : "bt_unpair_result";

    client.send("{\"command\":\"subscribe\"}\n");
    nlohmann::json request;
    request["command"] = command;
    request["address"] = address;
    if (!client.send(request.dump() + "\n")) {
        debug::log(ERR, "Could not reach the daemon.\n");
        return 1;
    }

    std::string buffer;
    char buf[8192];
    while (true) {
        ssize_t n = client.read(buf, sizeof(buf));
        if (n <= 0) {
            debug::log(ERR, "Daemon closed the connection before the operation finished.\n");
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
        debug::log(ERR, "Could not read Bluetooth connection state from the daemon.\n");
        return 1;
    }

    auto yn = [](bool v) { return v ? "yes" : "no"; };
    fprintf(stdout, "Device present:  %s\n", yn(resp.value("device_present", false)));
    fprintf(stdout, "BR/EDR:          %s\n", yn(resp.value("classic_connected", false)));
    fprintf(stdout,
            "LE:              %s%s\n",
            yn(resp.value("le_connected", false)),
            resp.value("le_available", false) ? "" : " (bearer unavailable)");
    fprintf(stdout, "Messages (MAP):  %s", yn(resp.value("map_open", false)));
    if (resp.value("map_error", std::string("none")) != "none")
        fprintf(stdout, "  [%s]", resp.value("map_error", "").c_str());
    fprintf(stdout, "\nContacts (PBAP): %s", yn(resp.value("pbap_open", false)));
    if (resp.value("pbap_error", std::string("none")) != "none")
        fprintf(stdout, "  [%s]", resp.value("pbap_error", "").c_str());
    fprintf(stdout, "\n");

    const std::string link = resp.value("link_reason", "");
    const std::string profiles = resp.value("profile_reason", "");
    if (!link.empty())
        fprintf(stdout, "\n  %s\n", link.c_str());
    if (!profiles.empty())
        fprintf(stdout, "  %s\n", profiles.c_str());
    return 0;
}

// The daemon redacts; this only formats. Anything printed here is already safe
// to paste into an issue.
static int print_bt_diagnostics(tether::Client& client) {
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_diagnostics\"}\n"));
    } catch (const std::exception&) {
        debug::log(ERR, "Could not read Bluetooth diagnostics from the daemon.\n");
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

static int print_bt_threads(tether::Client& client) {
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait("{\"command\":\"bt_list_threads\"}\n"));
    } catch (const std::exception&) {
        debug::log(ERR, "Could not read conversations from the daemon.\n");
        return 1;
    }

    const auto& threads = resp["threads"];
    if (threads.empty()) {
        fprintf(stdout, "No conversations yet. Check 'tether --bt-connection'.\n");
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

        const std::string unread_note = unread > 0 ? "  (" + std::to_string(unread) + " unread)" : "";
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

static int print_bt_messages(tether::Client& client, const std::string& thread) {
    nlohmann::json request;
    request["command"] = "bt_list_messages";
    request["thread"] = thread;

    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(client.send_and_wait(request.dump() + "\n"));
    } catch (const std::exception&) {
        debug::log(ERR, "Could not read messages from the daemon.\n");
        return 1;
    }

    const auto& messages = resp["messages"];
    if (messages.empty()) {
        fprintf(stdout, "No messages in %s\n", thread.c_str());
        return 0;
    }

    for (const auto& m : messages) {
        fprintf(stdout,
                "%s  %-8s %s%s\n",
                format_time(m.value("timestamp", (int64_t)0)).c_str(),
                m.value("outgoing", false) ? "me" : "them",
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
        debug::log(ERR, "Could not reach the daemon.\n");
        return 1;
    }

    std::string buffer;
    char buf[8192];
    while (true) {
        ssize_t n = client.read(buf, sizeof(buf));
        if (n <= 0) {
            debug::log(ERR, "Daemon closed the connection before the message was sent.\n");
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
                fprintf(stdout, "Sent to %s\n", thread.c_str());
                return 0;
            }
            debug::log(ERR, "{}\n", event.value("message", "The message was not sent."));
            return 1;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help();
        return 0;
    }

    std::string action, arg_val, arg_val2, host = "";
    int port = 5134;
    int timeout_ms = 3000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        } else if (arg == "-g" || arg == "--get-clipboard") {
            action = "get";
        } else if (arg == "-d" || arg == "--discover") {
            action = "discover";
        } else if (arg == "--list-devices") {
            action = "list";
        } else if (arg == "--bt-status") {
            action = "bt_status";
        } else if (arg == "--bt-devices") {
            action = "bt_devices";
        } else if (arg == "--bt-connection") {
            action = "bt_connection";
        } else if (arg == "--bt-diagnostics") {
            action = "bt_diagnostics";
        } else if (arg == "--bt-threads") {
            action = "bt_threads";
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
        } else if (arg == "--bt-pair") {
            action = "bt_pair";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                arg_val = argv[++i];
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
        } else if (arg == "--host") {
            if (i + 1 < argc && argv[i + 1][0] != '-')
                host = argv[++i];
        } else if (arg == "--port") {
            if (i + 1 < argc && argv[i + 1][0] != '-')
                port = std::stoi(argv[++i]);
        } else if (arg == "--timeout") {
            if (i + 1 < argc && argv[i + 1][0] != '-')
                timeout_ms = std::stoi(argv[++i]);
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
        debug::log(ERR, "Unknown action. Run tether --help for options\n");
        return 1;
    }

    tether::Client client;

    // Fast-path local operations that don't need active daemon connection fundamentally
    if (action == "list") {
        debug::log(INFO, "{}", client.list_devices());
        return 0;
    } else if (action == "accept") {
        client.accept_device(arg_val);
        debug::log(INFO, "Successfully paired device: {}", arg_val);
        return 0;
    } else if (action == "discover") {
        debug::log(INFO, "Scanning for tetherd instances on the local network...\n\n");

        tether::Discovery discovery;
        auto hosts = discovery.discover(timeout_ms);
        auto devices = tether::group_discovered_hosts(hosts);

        if (devices.empty()) {
            debug::log(INFO, "  No tetherd instances found.\n");
        } else {
            tether::Crypto::instance().init();
            for (const auto& dev : devices) {
                std::string status = "[new]";
                if (!dev.fingerprint.empty() && tether::Crypto::instance().is_host_known(dev.fingerprint)) {
                    status = "[known]";
                }
                fprintf(stdout, "  %-20s  %s\n", dev.name.c_str(), status.c_str());
                for (const auto& addr : dev.addresses) {
                    fprintf(stdout, "    %s:%d\n", addr.address.c_str(), addr.port);
                }
                fprintf(stdout, "\n");
            }
        }

        fprintf(stdout, "Found %zu device(s) in %.1fs\n", devices.size(), timeout_ms / 1000.0);
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
        debug::log(ERR,
                   "Explicit framework connection locally rejected! Did you target explicitly invalid TLS or is daemon "
                   "broken?\n");
        return 1;
    }

    std::string err;
    if (action == "pair") {
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
            debug::log(ERR, "Transfer Failed: {}", err);
            return 1;
        }
        debug::log(INFO, "File transfer delivered.\n");
    } else if (action == "native") {
        run_native_messaging_host(client);
    } else if (action == "bt_status") {
        return print_bt_status(client);
    } else if (action == "bt_devices") {
        return print_bt_devices(client);
    } else if (action == "bt_connection") {
        return print_bt_connection(client);
    } else if (action == "bt_diagnostics") {
        return print_bt_diagnostics(client);
    } else if (action == "bt_threads") {
        return print_bt_threads(client);
    } else if (action == "bt_messages") {
        if (arg_val.empty()) {
            debug::log(ERR, "A thread key is required, e.g. --bt-messages tel:+15551234567\n");
            return 1;
        }
        return print_bt_messages(client, arg_val);
    } else if (action == "bt_send") {
        if (arg_val.empty() || arg_val2.empty()) {
            debug::log(ERR, "A conversation and a message are required, e.g. --bt-send tel:+15551234567 \"hi\"\n");
            return 1;
        }
        return send_bt_message(client, arg_val, arg_val2);
    } else if (action == "bt_pair" || action == "bt_unpair") {
        if (arg_val.empty()) {
            debug::log(ERR, "A Bluetooth address is required, e.g. --bt-pair AA:BB:CC:DD:EE:FF\n");
            return 1;
        }
        return run_bt_transaction(client, action == "bt_pair" ? "bt_pair" : "bt_unpair", arg_val);
    }

    return 0;
}
