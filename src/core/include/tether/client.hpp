#pragma once
#include <string>
#include <openssl/ssl.h>
#include <vector>

namespace tether {

class Client {
public:
    Client();
    ~Client();

    // host="" limits targeting to UNIX Socket securely, implicitly triggering daemon boot!
    bool connect(const std::string& host = "", int port = 5134);
    void disconnect();

    // Base Level payload loop
    std::string send_and_wait(const std::string& payload);

    // High Level Features
    std::string get_clipboard(std::string& err_out);
    bool set_clipboard(const std::string& text, std::string& err_out);
    
    std::string list_devices();
    bool accept_device(const std::string& fingerprint);
    std::string pair(std::string& err_out);

    bool send_file(const std::string& path, std::string& err_out);

private:
    int connect_unix(bool retry);

    int sock_ = -1;
    SSL* ssl_ = nullptr;
};

} // namespace tether
