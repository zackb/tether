#pragma once

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string>
#include <vector>

namespace tether {

    class Crypto {
    public:
        static Crypto& instance();

        bool init();
        SSL_CTX* get_server_context();
        SSL_CTX* get_client_context();

        std::string get_my_fingerprint();
        bool is_host_known(const std::string& fingerprint);
        void add_known_host(const std::string& name, const std::string& fingerprint);
        std::string get_known_hosts_dump() const;

        static std::string get_peer_fingerprint(SSL* ssl);
        static std::string generate_fingerprint(X509* cert);

    private:
        Crypto() = default;
        ~Crypto();

        bool ensure_certificates();
        void load_known_hosts();
        void save_known_hosts();

        std::string cert_path_;
        std::string key_path_;
        std::string hosts_path_;

        SSL_CTX* server_ctx_ = nullptr;
        SSL_CTX* client_ctx_ = nullptr;

        std::string my_fingerprint_;
    };

} // namespace tether
