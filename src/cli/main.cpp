#include <cstring>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <tether/net.hpp> // for get_runtime_dir
#include <unistd.h>
#include <vector>

void print_help() {
    std::cout << "tether - Wayland companion CLI\n\n"
              << "Options:\n"
              << "  -h, --help               Show this help message.\n"
              << "  -g, --get-clipboard      Retrieve the current Wayland clipboard text.\n"
              << "  -s, --set-clipboard      Take string input and copy it to the local Wayland clipboard.\n"
              << "                           If no argument is given, it reads from STDIN.\n\n"
              << "Examples:\n"
              << "  tether -g\n"
              << "  tether -s \"Hello world\"\n"
              << "  echo \"pipe me\" | tether -s\n";
}

int connect_to_daemon(bool retry = true) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string path = tether::get_runtime_dir() + "/tetherd.sock";
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        if (!retry)
            return -1;

        // Let's spawn the daemon in the background
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            std::filesystem::path self_path = std::filesystem::read_symlink("/proc/self/exe");
            std::string daemon_path = (self_path.parent_path() / "tetherd").string();

            // Double fork trick to detach completely without zombies
            if (fork() == 0) {
                // Grandchild - disconnects standard output to not spam CLI calling terminal
                freopen("/dev/null", "w", stdout);
                freopen("/dev/null", "w", stderr);
                freopen("/dev/null", "r", stdin);

                execl(daemon_path.c_str(), "tetherd", nullptr);
                // Fallback to path lookup if it wasn't strictly adjacent
                execlp("tetherd", "tetherd", nullptr);
                exit(1); // Abort if launch failed
            }
            exit(0); // Intermediate child exits immediately
        } else if (pid > 0) {
            // Parent waits for intermediate child
            int status;
            waitpid(pid, &status, 0);

            // Give daemon a generous 300ms to spin up epoll and wayland
            usleep(300000);
            return connect_to_daemon(false);
        }
        return -1;
    }
    return sock;
}

std::string send_and_wait(int sock, const std::string& cmd) {
    write(sock, cmd.c_str(), cmd.size());
    char buf[4096];
    ssize_t n = read(sock, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return std::string(buf);
    }
    return "";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help();
        return 0;
    }

    std::string action;
    std::string arg_val;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        } else if (arg == "-g" || arg == "--get-clipboard") {
            action = "get";
        } else if (arg == "-s" || arg == "--set-clipboard") {
            action = "set";
            // Check if there is an argument immediately following the flag
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                arg_val = argv[++i];
            }
        }
    }

    if (action.empty()) {
        std::cerr << "Unknown action. Run tether --help for options\n";
        return 1;
    }

    if (action == "set" && arg_val.empty()) {
        // Read from stdin
        std::string line;
        while (std::getline(std::cin, line)) {
            arg_val += line + "\n";
        }
        if (!arg_val.empty() && arg_val.back() == '\n') {
            arg_val.pop_back(); // clip final stray newline from stream end
        }
    }

    int sock = connect_to_daemon();
    if (sock < 0) {
        std::cerr << "Failed to connect to daemon and automatic launch failed.\n";
        return 1;
    }

    if (action == "get") {
        nlohmann::json j;
        j["command"] = "clipboard_get";
        std::string payload = j.dump() + "\n";

        std::string response = send_and_wait(sock, payload);
        try {
            nlohmann::json r = nlohmann::json::parse(response);
            if (r.contains("content")) {
                std::cout << r["content"].get<std::string>() << std::flush;
            }
        } catch (...) {
            // fallback if protocol didn't match / ignore error
        }
    } else if (action == "set") {
        nlohmann::json j;
        j["command"] = "clipboard_set";
        j["content"] = arg_val;
        std::string payload = j.dump() + "\n";
        send_and_wait(sock, payload); // Ignore daemon's "OK\n"
    }

    close(sock);
    return 0;
}
