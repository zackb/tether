#include <iostream>
#include <string>
#include <tether/client.hpp>

void print_help() {
    std::cout << "tether - Wayland companion CLI\n\n"
              << "Options:\n"
              << "  -h, --help               Show this help message.\n"
              << "  -g, --get-clipboard      Retrieve the current Wayland clipboard text.\n"
              << "  -s, --set-clipboard      Take string input and copy it to the local Wayland clipboard.\n"
              << "  -f, --send-file          Send a file securely to the local Wayland download directory.\n"
              << "  --host <ip>              Connect over TCP to daemon ip instead of UNIX Socket.\n"
              << "  --port <num>             Connect over TCP port (default 5134).\n"
              << "  --list-devices           List all paired connection devices.\n"
              << "  --accept <fingerprint>   Accept a pending pairing request locally.\n"
              << "  --pair                   Send a pair_request over TCP to the daemon.\n\n"
              << "Examples:\n"
              << "  tether -g\n"
              << "  tether -g --host 127.0.0.1\n"
              << "  echo \"pipe\" | tether -s\n"
              << "  tether --accept 9a4f21...\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help();
        return 0;
    }

    std::string action, arg_val, host = "";
    int port = 5134;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        } else if (arg == "-g" || arg == "--get-clipboard") { action = "get"; }
        else if (arg == "--list-devices") { action = "list"; }
        else if (arg == "--pair") { action = "pair"; }
        else if (arg == "--accept") { action = "accept"; if (i + 1 < argc && argv[i + 1][0] != '-') arg_val = argv[++i]; }
        else if (arg == "--host") { if (i + 1 < argc && argv[i + 1][0] != '-') host = argv[++i]; }
        else if (arg == "--port") { if (i + 1 < argc && argv[i + 1][0] != '-') port = std::stoi(argv[++i]); }
        else if (arg == "-f" || arg == "--send-file") {
            action = "file";
            if (i + 1 < argc && argv[i + 1][0] != '-') arg_val = argv[++i];
        } else if (arg == "-s" || arg == "--set-clipboard") {
            action = "set";
            if (i + 1 < argc && argv[i + 1][0] != '-') arg_val = argv[++i];
        }
    }

    if (action.empty()) {
        std::cerr << "Unknown action. Run tether --help for options\n";
        return 1;
    }

    tether::Client client;

    // Fast-path local operations that don't need active daemon connection fundamentally
    if (action == "list") {
        std::cout << client.list_devices() << "\n";
        return 0;
    } else if (action == "accept") {
        client.accept_device(arg_val);
        std::cout << "Successfully paired device: " << arg_val << "\n";
        return 0;
    }

    if (action == "set" && arg_val.empty()) {
        std::string line;
        while (std::getline(std::cin, line)) arg_val += line + "\n";
        if (!arg_val.empty() && arg_val.back() == '\n') arg_val.pop_back();
    }

    // Bind network abstraction natively
    if (!client.connect(host, port)) {
        std::cerr << "Explicit framework connection locally rejected! Did you target explicitly invalid TLS or is daemon broken?\n";
        return 1;
    }

    std::string err;
    if (action == "pair") {
        std::string resp = client.pair("CLI", err);
        if (!err.empty()) {
            std::cerr << err << "\n";
            return 1;
        }
        std::cout << "Pairing response: " << resp << std::flush;
    } else if (action == "get") {
        std::string clip = client.get_clipboard(err);
        if (!err.empty()) {
            std::cerr << err << "\n";
            return 1;
        }
        std::cout << clip << std::flush;
    } else if (action == "set") {
        if (!client.set_clipboard(arg_val, err)) {
            std::cerr << err << "\n";
            return 1;
        }
    } else if (action == "file") {
        std::cout << "Uploading " << arg_val << "..." << std::endl;
        if (!client.send_file(arg_val, err)) {
            std::cerr << "Transfer Failed: " << err << "\n";
            return 1;
        }
        std::cout << "File transfer successfully saved.\n";
    }

    return 0;
}
