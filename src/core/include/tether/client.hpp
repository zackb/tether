#pragma once
#include <openssl/ssl.h>
#include <string>
#include <vector>

namespace tether {

    class Client {
    public:
        Client();
        ~Client();

        // host="" uses the local UNIX socket, auto-launching the daemon if needed.
        // host!="" opens a TCP+TLS connection to a remote daemon.
        bool connect(const std::string& host = "", int port = 5134);
        void disconnect();
        bool is_connected() const;

        // Base Level payload loop
        std::string send_and_wait(const std::string& payload);
        std::string get_peer_fingerprint() const;

        // High Level Features
        std::string get_clipboard(std::string& err_out);
        bool set_clipboard(const std::string& text, std::string& err_out);

        std::string list_devices();
        bool accept_device(const std::string& fingerprint, const std::string& name = "Paired Device");
        std::string pair(const std::string& device_name, std::string& err_out);

        bool send_file(const std::string& path, std::string& err_out);

    private:
        int connect_unix(bool retry);

        int sock_ = -1;
        SSL* ssl_ = nullptr;
    };

} // namespace tether
