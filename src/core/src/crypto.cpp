#include "tether/crypto.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <cstdlib>

namespace tether {

Crypto& Crypto::instance() {
    static Crypto inst;
    return inst;
}

Crypto::~Crypto() {
    if (server_ctx_) SSL_CTX_free(server_ctx_);
    if (client_ctx_) SSL_CTX_free(client_ctx_);
}

SSL_CTX* Crypto::get_server_context() { return server_ctx_; }
SSL_CTX* Crypto::get_client_context() { return client_ctx_; }

bool Crypto::init() {
    std::filesystem::path config_dir = std::string(getenv("HOME")) + "/.config/tether";
    std::filesystem::create_directories(config_dir);
    cert_path_ = (config_dir / "cert.pem").string();
    key_path_ = (config_dir / "key.pem").string();
    hosts_path_ = (config_dir / "known_hosts.json").string();

    if (!ensure_certificates()) return false;
    
    // server context
    const SSL_METHOD* server_method = TLS_server_method();
    server_ctx_ = SSL_CTX_new(server_method);
    if (!server_ctx_) return false;
    
    SSL_CTX_use_certificate_file(server_ctx_, cert_path_.c_str(), SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(server_ctx_, key_path_.c_str(), SSL_FILETYPE_PEM);
    if (!SSL_CTX_check_private_key(server_ctx_)) return false;

    // Mutually Authenticate, but accept anything to allow pairing request processing
    SSL_CTX_set_verify(server_ctx_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, [](int, X509_STORE_CTX*) {
        return 1;
    });

    // client context
    const SSL_METHOD* client_method = TLS_client_method();
    client_ctx_ = SSL_CTX_new(client_method);
    if (!client_ctx_) return false;
    
    SSL_CTX_use_certificate_file(client_ctx_, cert_path_.c_str(), SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(client_ctx_, key_path_.c_str(), SSL_FILETYPE_PEM);
    
    SSL_CTX_set_verify(client_ctx_, SSL_VERIFY_PEER, [](int, X509_STORE_CTX*) {
        return 1;
    });

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

    load_known_hosts();
    return true;
}

bool Crypto::ensure_certificates() {
    if (std::filesystem::exists(cert_path_) && std::filesystem::exists(key_path_)) {
        return true;
    }

    std::cout << "Crypto: Generating native mTLS RSA/X509 Keypair into ~/.config/tether/..." << std::endl;

    EVP_PKEY* pkey = EVP_PKEY_new();
    BIGNUM* bne = BN_new();
    BN_set_word(bne, RSA_F4);
    RSA* rsa = RSA_new();
    RSA_generate_key_ex(rsa, 2048, bne, nullptr);
    EVP_PKEY_assign_RSA(pkey, rsa); // pkey owns rsa now
    BN_free(bne);

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

    std::filesystem::permissions(key_path_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    return true;
}

void Crypto::load_known_hosts() {
    if (!std::filesystem::exists(hosts_path_)) {
        nlohmann::json empty = nlohmann::json::object();
        std::ofstream off(hosts_path_);
        off << empty.dump(4);
    }
}

void Crypto::save_known_hosts() {
    // We do it directly in add_known_host right now
}

bool Crypto::is_host_known(const std::string& fingerprint) {
    std::ifstream iff(hosts_path_);
    if (!iff.is_open()) return false;
    try {
        nlohmann::json j = nlohmann::json::parse(iff);
        return j.contains(fingerprint);
    } catch (...) { return false; }
}

void Crypto::add_known_host(const std::string& name, const std::string& fingerprint) {
    nlohmann::json j;
    std::ifstream iff(hosts_path_);
    if (iff.is_open()) {
        try { j = nlohmann::json::parse(iff); } catch (...) {}
    }
    j[fingerprint] = name;
    
    std::ofstream off(hosts_path_);
    off << j.dump(4);
}

std::string Crypto::get_known_hosts_dump() const {
    std::ifstream iff(hosts_path_);
    if (iff.is_open()) {
        try { 
            nlohmann::json j = nlohmann::json::parse(iff);
            return j.dump();
        } catch (...) {}
    }
    return "{}";
}

std::string Crypto::get_my_fingerprint() {
    return my_fingerprint_;
}

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
    if (!cert) return "";
    std::string print = generate_fingerprint(cert);
    X509_free(cert);
    return print;
}

} // namespace tether
