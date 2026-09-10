#include "tether/client.hpp"
#include "tether/base64.hpp"
#include "tether/crypto.hpp"
#include "tether/net.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <netdb.h>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <tether/log.hpp>
#include <unistd.h>

namespace tether {

    namespace {

        std::string extract_protocol_error(const std::string& response) {
            try {
                nlohmann::json parsed = nlohmann::json::parse(response);
                if (parsed.value("command", "") == "error") {
                    return parsed.value("message", "unknown_error");
                }
            } catch (...) {
            }

            return "";
        }

    } // namespace

    Client::Client() {}

    Client::~Client() { disconnect(); }

    void Client::disconnect() {
        if (ssl_) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (sock_ >= 0) {
            close(sock_);
            sock_ = -1;
        }
    }

    bool Client::is_connected() const { return sock_ >= 0; }

    std::string Client::get_peer_fingerprint() const {
        if (!ssl_)
            return "";
        return Crypto::get_peer_fingerprint(ssl_);
    }

    // systemd owns the daemon once its user unit is enabled, and the wants
    // symlink is what 'systemctl --user enable' writes. Spawning our own tetherd
    // alongside it leaves an orphan holding the TCP listener, which the unit then
    // cannot bind, so it would restart into the failure until systemd gives up.
    static bool systemd_owns_tetherd() {
        std::filesystem::path config;
        if (const char* config_home = std::getenv("XDG_CONFIG_HOME"); config_home && *config_home == '/')
            config = config_home;
        else if (const char* home = std::getenv("HOME"); home && *home)
            config = std::filesystem::path(home) / ".config";
        else
            return false;

        std::error_code ec;
        return std::filesystem::exists(config / "systemd/user/default.target.wants/tetherd.service", ec);
    }

    void spawn_daemon() {
        // Let systemd start it, and stay out of the way when it already does.
        if (systemd_owns_tetherd())
            return;

        // everything the child needs is resolved before the fork
        std::filesystem::path self_path = std::filesystem::read_symlink("/proc/self/exe");
        const std::string sibling = (self_path.parent_path() / "tetherd").string();

        // appimage binaries live on a mount that only lives for the life of the parent, so the daemon has to run from a
        // mount of its own.
        const char* appimage = std::getenv("APPIMAGE");

        pid_t pid = fork();
        if (pid < 0)
            return;

        if (pid == 0) {
            if (fork() == 0) {
                // tetherd reopens these onto its own log file.
                if (freopen("/dev/null", "w", stdout) == nullptr) {
                }
                if (freopen("/dev/null", "w", stderr) == nullptr) {
                }
                if (freopen("/dev/null", "r", stdin) == nullptr) {
                }

                if (appimage && *appimage)
                    execl(appimage, "tetherd", "--daemon", nullptr);
                execl(sibling.c_str(), "tetherd", nullptr);
                execlp("tetherd", "tetherd", nullptr);
                _exit(1);
            }
            _exit(0);
        }

        int status;
        waitpid(pid, &status, 0);
    }

    int Client::connect_unix(bool retry) {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0)
            return -1;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::string path = tether::get_runtime_dir() + "/tetherd.sock";
        if (path.size() >= sizeof(addr.sun_path)) {
            debug::log(ERR, "Socket path too long: {}", path);
            close(sock);
            return -1;
        }
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            if (!retry)
                return -1;

            spawn_daemon();
            usleep(300000);
            return connect_unix(false);
        }
        return sock;
    }

    bool Client::connect(const std::string& host, int port) {
        disconnect();

        if (!host.empty()) {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

            addrinfo* results = nullptr;
            const std::string service = std::to_string(port);
            if (int err = getaddrinfo(host.c_str(), service.c_str(), &hints, &results); err != 0 || !results) {
                debug::log(ERR, "Invalid address {}: {}", host, gai_strerror(err));
                return false;
            }

            for (addrinfo* ai = results; ai; ai = ai->ai_next) {
                sock_ = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                if (sock_ < 0)
                    continue;
                if (::connect(sock_, ai->ai_addr, ai->ai_addrlen) == 0)
                    break;
                close(sock_);
                sock_ = -1;
            }
            const int connect_errno = errno;
            freeaddrinfo(results);

            if (sock_ < 0) {
                debug::log(ERR, "Failed to connect to {}:{}: {}", host, port, std::strerror(connect_errno));
                return false;
            }

            tether::Crypto::instance().init();
            ssl_ = SSL_new(tether::Crypto::instance().get_client_context());
            SSL_set_fd(ssl_, sock_);
            if (SSL_connect(ssl_) <= 0) {
                return false;
            }
            return true;
        } else {
            sock_ = connect_unix(true);
            return sock_ >= 0;
        }
    }

    std::string Client::send_and_wait(const std::string& payload) {
        constexpr size_t kBufSize = 1024 * 1024; // Generous payload buffer
        if (read_buf_.size() < kBufSize)
            read_buf_.resize(kBufSize);
        char* buf = read_buf_.data();

        if (!send(payload))
            return "";

        if (ssl_) {
            int n = SSL_read(ssl_, buf, static_cast<int>(kBufSize - 1));
            if (n > 0)
                return std::string(buf, n);
        } else {
            ssize_t n = ::read(sock_, buf, kBufSize - 1);
            if (n > 0)
                return std::string(buf, n);
        }
        return "";
    }

    bool Client::send(const std::string& payload) {
        // Loop: a short write on a stream socket is legal, and reporting it as success
        // silently desynchronises the newline-framed protocol. Matters for the ~683 KB
        // base64 payloads send_file() produces.
        size_t total = 0;
        while (total < payload.size()) {
            const char* p = payload.data() + total;
            size_t remaining = payload.size() - total;

            if (ssl_) {
                int n = SSL_write(ssl_, p, static_cast<int>(remaining));
                if (n <= 0) {
                    int err = SSL_get_error(ssl_, n);
                    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ)
                        continue;
                    return false;
                }
                total += static_cast<size_t>(n);
            } else {
                ssize_t n = ::write(sock_, p, remaining);
                if (n < 0) {
                    if (errno == EINTR)
                        continue;
                    debug::log(ERR, "client write error: {}", std::strerror(errno));
                    return false;
                }
                if (n == 0)
                    return false;
                total += static_cast<size_t>(n);
            }
        }
        return true;
    }

    ssize_t Client::read(char* buf, size_t count) {
        if (ssl_) {
            return SSL_read(ssl_, buf, count);
        } else {
            return ::read(sock_, buf, count);
        }
    }

    std::string Client::get_clipboard(std::string& err_out) {
        nlohmann::json j;
        j["command"] = "clipboard_get";
        std::string response = send_and_wait(j.dump() + "\n");

        try {
            nlohmann::json r = nlohmann::json::parse(response);
            if (r.contains("content")) {
                return r["content"].get<std::string>();
            } else if (r.contains("message") && r["message"] == "unauthorized") {
                err_out = "Unauthorized. You must pair this device first.";
            }
        } catch (...) {
        }
        return "";
    }

    bool Client::set_clipboard(const std::string& text, std::string& err_out) {
        nlohmann::json j;
        j["command"] = "clipboard_set";
        j["content"] = text;
        std::string s_resp = send_and_wait(j.dump() + "\n");
        if (s_resp.find("unauthorized") != std::string::npos) {
            err_out = "Unauthorized. Device not paired.";
            return false;
        }
        return true;
    }

    std::string Client::list_devices() {
        tether::Crypto::instance().init();
        return tether::Crypto::instance().get_known_hosts_dump();
    }

    bool Client::accept_device(const std::string& fingerprint, const std::string& name) {
        if (!is_connected() || ssl_ || fingerprint.empty())
            return false;

        nlohmann::json request{{"command", "accept_device"}, {"fingerprint", fingerprint}, {"device_name", name}};
        const std::string response = send_and_wait(request.dump() + "\n");
        try {
            const auto parsed = nlohmann::json::parse(response);
            return parsed.value("command", "") == "accept_device_result" && parsed.value("accepted", false);
        } catch (...) {
            return false;
        }
    }

    bool Client::forget_device(const std::string& fingerprint) {
        if (!is_connected() || ssl_ || fingerprint.empty())
            return false;

        nlohmann::json request{{"command", "forget_device"}, {"fingerprint", fingerprint}};
        const std::string response = send_and_wait(request.dump() + "\n");
        try {
            const auto parsed = nlohmann::json::parse(response);
            return parsed.value("command", "") == "forget_device_result" && parsed.value("forgotten", false);
        } catch (...) {
            return false;
        }
    }

    std::string Client::pair(const std::string& device_name, std::string& err_out) {
        if (!ssl_) {
            err_out = "Pairing requires a TCP+TLS connection. Use --host to specify a target.";
            return "";
        }
        nlohmann::json j;
        j["command"] = "pair_request";
        j["device_name"] = device_name;
        std::string response = send_and_wait(j.dump() + "\n");
        if (response.empty()) {
            err_out = "No response from daemon.";
        }
        return response;
    }

    bool Client::send_file(const std::string& path, std::string& err_out) {
        if (path.empty() || !std::filesystem::exists(path)) {
            err_out = "Invalid or missing file path.";
            return false;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            err_out = "Failed to open file.";
            return false;
        }

        size_t file_size = std::filesystem::file_size(path);
        std::string filename = std::filesystem::path(path).filename().string();
        // pid + counter, not just time(): two transfers in the same second produced the
        // same id, and the receiver rejects a duplicate transfer_id.
        static std::atomic<unsigned> seq{0};
        std::string transfer_id =
            "cli_" + std::to_string(getpid()) + "_" + std::to_string(time(nullptr)) + "_" + std::to_string(seq++);

        nlohmann::json j_start;
        j_start["command"] = "file_start";
        j_start["filename"] = filename;
        j_start["size"] = file_size;
        j_start["transfer_id"] = transfer_id;

        std::string s_resp = send_and_wait(j_start.dump() + "\n");
        if (std::string protocol_error = extract_protocol_error(s_resp); !protocol_error.empty()) {
            if (protocol_error == "unauthorized") {
                err_out = "Unauthorized. Device not paired.";
            } else if (protocol_error == "no_connected_mobile_client") {
                err_out = "No connected iPhone client is available.";
            } else {
                err_out = protocol_error;
            }
            return false;
        }

        const size_t chunk_size = 512 * 1024;
        std::vector<unsigned char> buffer(chunk_size);
        int chunk_idx = 0;

        while (file) {
            file.read(reinterpret_cast<char*>(buffer.data()), chunk_size);
            size_t bytes_read = file.gcount();
            if (bytes_read == 0)
                break;

            nlohmann::json j_chunk;
            j_chunk["command"] = "file_chunk";
            j_chunk["chunk_index"] = chunk_idx++;
            j_chunk["transfer_id"] = transfer_id;
            j_chunk["data"] = tether::base64_encode(buffer.data(), bytes_read);

            std::string chunk_resp = send_and_wait(j_chunk.dump() + "\n");
            if (std::string protocol_error = extract_protocol_error(chunk_resp); !protocol_error.empty()) {
                if (protocol_error == "no_connected_mobile_client") {
                    err_out = "No connected iPhone client is available.";
                } else {
                    err_out = protocol_error;
                }
                return false;
            }
        }

        nlohmann::json j_end;
        j_end["command"] = "file_end";
        j_end["transfer_id"] = transfer_id;

        std::string resp = send_and_wait(j_end.dump() + "\n");
        try {
            nlohmann::json r = nlohmann::json::parse(resp);
            if (r.contains("status") && r["status"] == "success") {
                return true;
            }
            if (r.value("command", "") == "error") {
                std::string message = r.value("message", "Transfer sequence explicitly rejected externally.");
                if (message == "no_connected_mobile_client") {
                    err_out = "No connected iPhone client is available.";
                } else {
                    err_out = message;
                }
                return false;
            }
        } catch (...) {
        }

        err_out = "Transfer sequence explicitly rejected externally.";
        return false;
    }

} // namespace tether
