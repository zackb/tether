#include "tether/crypto.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <sstream>
#include <tether/log.hpp>
#include <tether/paths.hpp>
#include <unistd.h>

namespace tether {

    Crypto& Crypto::instance() {
        static Crypto inst;
        return inst;
    }

    Crypto::~Crypto() {
        if (server_ctx_)
            SSL_CTX_free(server_ctx_);
        if (client_ctx_)
            SSL_CTX_free(client_ctx_);
    }

    SSL_CTX* Crypto::get_server_context() { return server_ctx_; }
    SSL_CTX* Crypto::get_client_context() { return client_ctx_; }

    bool Crypto::init() {
        if (server_ctx_ && client_ctx_)
            return true;

        const std::filesystem::path config_dir = paths::config_dir();
        if (config_dir.empty())
            throw std::runtime_error("Cannot determine home directory (HOME unset and no passwd entry)");
        std::filesystem::create_directories(config_dir);
        cert_path_ = (config_dir / "cert.pem").string();
        key_path_ = (config_dir / "key.pem").string();
        hosts_path_ = (config_dir / "known_hosts.json").string();

        if (!ensure_certificates())
            return false;

        // server context
        const SSL_METHOD* server_method = TLS_server_method();
        server_ctx_ = SSL_CTX_new(server_method);
        if (!server_ctx_)
            return false;

        // Pin to TLS 1.2 to ensure stability with iOS BoringSSL client
        SSL_CTX_set_min_proto_version(server_ctx_, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(server_ctx_, TLS1_2_VERSION);

        SSL_CTX_use_certificate_file(server_ctx_, cert_path_.c_str(), SSL_FILETYPE_PEM);
        SSL_CTX_use_PrivateKey_file(server_ctx_, key_path_.c_str(), SSL_FILETYPE_PEM);
        if (!SSL_CTX_check_private_key(server_ctx_))
            return false;

        // Require a peer certificate but accept any chain: both peers are self-signed and there is
        // no CA. Identity comes from pinning the peer's fingerprint against known_hosts.json in
        // TcpServer::handle_client.
        SSL_CTX_set_verify(
            server_ctx_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, [](int, X509_STORE_CTX*) { return 1; });

        // client context
        const SSL_METHOD* client_method = TLS_client_method();
        client_ctx_ = SSL_CTX_new(client_method);
        if (!client_ctx_)
            return false;

        SSL_CTX_set_min_proto_version(client_ctx_, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(client_ctx_, TLS1_2_VERSION);

        SSL_CTX_use_certificate_file(client_ctx_, cert_path_.c_str(), SSL_FILETYPE_PEM);
        SSL_CTX_use_PrivateKey_file(client_ctx_, key_path_.c_str(), SSL_FILETYPE_PEM);

        // Same as the server context: the chain is not validated, the peer's pinned fingerprint is.
        SSL_CTX_set_verify(client_ctx_, SSL_VERIFY_PEER, [](int, X509_STORE_CTX*) { return 1; });

        // Derive my fingerprint
        FILE* f = fopen(cert_path_.c_str(), "r");
        if (f) {
            X509* x = PEM_read_X509(f, nullptr, nullptr, nullptr);
            fclose(f);
            if (x) {
                my_fingerprint_ = generate_fingerprint(x);
                X509_free(x);
            }
        }

        {
            std::lock_guard<std::mutex> lock(hosts_mutex_);
            load_known_hosts();
        }
        return true;
    }

    bool Crypto::ensure_certificates() {
        if (std::filesystem::exists(cert_path_) && std::filesystem::exists(key_path_)) {
            return true;
        }

        debug::log(INFO, "Crypto: Generating native mTLS RSA/X509 Keypair into ~/.config/tether/...");

        /*
        EVP_PKEY* pkey = EVP_PKEY_new();
        BIGNUM* bne = BN_new();
        BN_set_word(bne, RSA_F4);
        RSA* rsa = RSA_new();
        RSA_generate_key_ex(rsa, 2048, bne, nullptr);
        EVP_PKEY_assign_RSA(pkey, rsa); // pkey owns rsa now
        BN_free(bne);
        */
        EVP_PKEY* pkey = nullptr;
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY_keygen_init(ctx);
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
        EVP_PKEY_keygen(ctx, &pkey);
        EVP_PKEY_CTX_free(ctx);

        X509* x509 = X509_new();
        ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
        X509_gmtime_adj(X509_get_notBefore(x509), 0);
        X509_gmtime_adj(X509_get_notAfter(x509), 315360000L); // 10 years

        X509_set_pubkey(x509, pkey);
        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char*)"Tether", -1, -1, 0);
        X509_set_issuer_name(x509, name);

        X509_sign(x509, pkey, EVP_sha256());

        FILE* f_key = fopen(key_path_.c_str(), "wb");
        if (f_key) {
            PEM_write_PrivateKey(f_key, pkey, nullptr, nullptr, 0, nullptr, nullptr);
            fclose(f_key);
        }

        FILE* f_cert = fopen(cert_path_.c_str(), "wb");
        if (f_cert) {
            PEM_write_X509(f_cert, x509);
            fclose(f_cert);
        }

        X509_free(x509);
        EVP_PKEY_free(pkey);

        std::filesystem::permissions(key_path_,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        return true;
    }

    void Crypto::load_known_hosts() {
        known_hosts_.clear();
        hosts_loaded_ = true;

        std::error_code exists_ec;
        if (!std::filesystem::exists(hosts_path_, exists_ec)) {
            save_known_hosts(); // materialise an empty file on first run
            return;
        }

        std::ifstream iff(hosts_path_);
        if (!iff.is_open()) {
            debug::log(ERR, "Crypto: cannot read {}; treating as empty without rewriting it", hosts_path_);
            return;
        }

        try {
            nlohmann::json j = nlohmann::json::parse(iff);
            for (auto& [fingerprint, name] : j.items()) {
                known_hosts_[fingerprint] = name.is_string() ? name.get<std::string>() : "Unknown Device";
            }
        } catch (...) {
            debug::log(ERR, "Crypto: known_hosts.json is corrupt; treating as empty");
        }
        iff.close();

        std::error_code ec;
        hosts_mtime_ = std::filesystem::last_write_time(hosts_path_, ec);
    }

    void Crypto::refresh_known_hosts_if_stale() {
        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(hosts_path_, ec);
        if (ec) {
            if (!hosts_loaded_)
                load_known_hosts();
            return;
        }
        if (!hosts_loaded_ || mtime != hosts_mtime_) {
            load_known_hosts();
        }
    }

    // writes to a temp file and renames
    void Crypto::save_known_hosts() {
        nlohmann::json j = nlohmann::json::object();
        for (const auto& [fingerprint, name] : known_hosts_) {
            j[fingerprint] = name;
        }

        std::string tmp_path = hosts_path_ + ".tmp";
        {
            std::ofstream off(tmp_path);
            if (!off.is_open()) {
                debug::log(ERR, "Crypto: cannot write {}", tmp_path);
                return;
            }
            off << j.dump(4);
            off.flush();
            if (!off) {
                debug::log(ERR, "Crypto: failed writing {}", tmp_path);
                return;
            }
        }

        std::error_code ec;
        std::filesystem::rename(tmp_path, hosts_path_, ec);
        if (ec) {
            debug::log(ERR, "Crypto: failed to replace {}: {}", hosts_path_, ec.message());
            std::filesystem::remove(tmp_path, ec);
            return;
        }
        hosts_mtime_ = std::filesystem::last_write_time(hosts_path_, ec);
    }

    bool Crypto::is_host_known(const std::string& fingerprint) {
        std::lock_guard<std::mutex> lock(hosts_mutex_);
        refresh_known_hosts_if_stale();
        return known_hosts_.count(fingerprint) > 0;
    }

    void Crypto::add_known_host(const std::string& name, const std::string& fingerprint) {
        std::lock_guard<std::mutex> lock(hosts_mutex_);
        refresh_known_hosts_if_stale(); // don't clobber a pairing another process added
        known_hosts_[fingerprint] = name;
        save_known_hosts();
    }

    bool Crypto::remove_known_host(const std::string& fingerprint) {
        std::lock_guard<std::mutex> lock(hosts_mutex_);
        refresh_known_hosts_if_stale();
        if (known_hosts_.erase(fingerprint) == 0)
            return false;
        save_known_hosts();
        return true;
    }

    std::string Crypto::get_known_hosts_dump() const {
        std::lock_guard<std::mutex> lock(hosts_mutex_);
        const_cast<Crypto*>(this)->refresh_known_hosts_if_stale();
        nlohmann::json j = nlohmann::json::object();
        for (const auto& [fingerprint, name] : known_hosts_) {
            j[fingerprint] = name;
        }
        return j.dump(2);
    }

    std::string Crypto::get_my_fingerprint() { return my_fingerprint_; }

    std::string Crypto::generate_fingerprint(X509* cert) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        unsigned int len = 0;
        X509_digest(cert, EVP_sha256(), hash, &len);

        std::stringstream ss;
        for (unsigned int i = 0; i < len; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        }
        return ss.str();
    }

    std::string Crypto::get_peer_fingerprint(SSL* ssl) {
        X509* cert = SSL_get_peer_certificate(ssl);
        if (!cert)
            return "";
        std::string print = generate_fingerprint(cert);
        X509_free(cert);
        return print;
    }

} // namespace tether
