#include "tether/secret_store.hpp"
#include "tether/base64.hpp"
#include "tether/log.hpp"
#include "tether/paths.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <libsecret/secret.h>
#include <mutex>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <unistd.h>

namespace tether {

    const char* to_string(Retention retention) {
        switch (retention) {
        case Retention::Plaintext:
            return "plaintext";
        case Retention::None:
            return "none";
        case Retention::Encrypted:
            break;
        }
        return "encrypted";
    }

    Retention retention_from_string(const std::string& value) {
        if (value == "plaintext")
            return Retention::Plaintext;
        if (value == "none")
            return Retention::None;
        return Retention::Encrypted;
    }

    namespace secret {

        namespace {

            constexpr size_t KEY_BYTES = 32;
            constexpr size_t NONCE_BYTES = 12;
            constexpr size_t TAG_BYTES = 16;

            const SecretSchema* store_schema() {
                static const SecretSchema schema = {
                    "com.tether.Store",
                    SECRET_SCHEMA_NONE,
                    {
                        {"application", SECRET_SCHEMA_ATTRIBUTE_STRING},
                        {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
                    },
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                };
                return &schema;
            }

            std::string keyfile_path() {
                const std::filesystem::path dir = paths::config_dir();
                if (dir.empty())
                    return {};
                return (dir / "store.key").string();
            }

            int64_t now_seconds() {
                return std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            }

            std::mutex g_mutex;
            Retention g_retention = Retention::Encrypted;
            std::vector<unsigned char> g_key;
            int64_t g_last_failure = 0;
            bool g_failed_once = false;

            // What the wallet can say about the store key without being unlocked.
            enum class KeyLookup {
                Found,       // the secret was readable
                Locked,      // an item exists but its secret is not readable
                LockedNoKey, // no item, and the default collection is locked or missing
                Absent,      // no item and the collection is unlocked: first run
                NoService,   // nothing on the bus
            };

            // Reads the key out of the desktop secret service without unlocking it.
            KeyLookup key_from_service(std::vector<unsigned char>& out) {
                GError* err = nullptr;
                SecretService* service = secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION, nullptr, &err);
                if (!service) {
                    if (err) {
                        debug::log(DEBUG, "secret: no secret service on the bus: {}", err->message);
                        g_error_free(err);
                    }
                    return KeyLookup::NoService;
                }

                GHashTable* attributes = g_hash_table_new(g_str_hash, g_str_equal);
                g_hash_table_insert(attributes, const_cast<char*>("application"), const_cast<char*>("tether"));
                // SEARCH_ALL, so an absent item is really absent rather than just past the first hit.
                GList* items = secret_service_search_sync(
                    service,
                    store_schema(),
                    attributes,
                    static_cast<SecretSearchFlags>(SECRET_SEARCH_ALL | SECRET_SEARCH_LOAD_SECRETS),
                    nullptr,
                    &err);
                g_hash_table_unref(attributes);

                if (err) {
                    debug::log(WARN, "secret: search failed: {}", err->message);
                    g_error_free(err);
                    g_object_unref(service);
                    // An item may well exist, so this must not fall through to generating one.
                    return KeyLookup::Locked;
                }

                bool found = false;
                for (GList* node = items; node && !found; node = node->next) {
                    auto* item = static_cast<SecretItem*>(node->data);
                    // null on a locked collection
                    SecretValue* value = secret_item_get_secret(item);
                    if (!value)
                        continue;
                    gsize length = 0;
                    const gchar* text = secret_value_get(value, &length);
                    if (text) {
                        auto decoded = base64_decode(std::string(text, length));
                        if (decoded.size() == KEY_BYTES) {
                            out = std::move(decoded);
                            found = true;
                        }
                    }
                    secret_value_unref(value);
                }
                const bool any_item = items != nullptr;
                g_list_free_full(items, g_object_unref);

                // An item that cannot be read is locked. Do not overwrite it.
                if (found || any_item) {
                    g_object_unref(service);
                    return found ? KeyLookup::Found : KeyLookup::Locked;
                }

                SecretCollection* collection = secret_collection_for_alias_sync(
                    service, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE, nullptr, &err);
                if (err) {
                    debug::log(DEBUG, "secret: cannot reach the default collection: {}", err->message);
                    g_error_free(err);
                }
                // Storing into a locked or missing collection would unlock prompt.
                const bool unlocked = collection && !secret_collection_get_locked(collection);
                if (collection)
                    g_object_unref(collection);
                g_object_unref(service);
                return unlocked ? KeyLookup::Absent : KeyLookup::LockedNoKey;
            }

            bool key_to_service(const std::vector<unsigned char>& key) {
                GError* err = nullptr;
                const std::string encoded = base64_encode(key);
                const gboolean ok = secret_password_store_sync(store_schema(),
                                                               SECRET_COLLECTION_DEFAULT,
                                                               "Tether message store key",
                                                               encoded.c_str(),
                                                               nullptr,
                                                               &err,
                                                               "application",
                                                               "tether",
                                                               nullptr);
                if (err) {
                    debug::log(WARN, "secret: cannot store the key in the wallet: {}", err->message);
                    g_error_free(err);
                }
                return ok == TRUE;
            }

            bool key_from_file(std::vector<unsigned char>& out) {
                const std::string path = keyfile_path();
                if (path.empty())
                    return false;
                std::ifstream in(path, std::ios::binary);
                if (!in.is_open())
                    return false;
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                    text.pop_back();
                auto decoded = base64_decode(text);
                if (decoded.size() != KEY_BYTES)
                    return false;
                out = std::move(decoded);
                return true;
            }

            bool key_to_file(const std::vector<unsigned char>& key) {
                const std::string path = keyfile_path();
                if (path.empty())
                    return false;

                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

                const std::string tmp = path + ".tmp";
                {
                    std::ofstream out(tmp, std::ios::trunc);
                    if (!out.is_open()) {
                        debug::log(ERR, "secret: cannot write {}", tmp);
                        return false;
                    }
                    out << base64_encode(key) << "\n";
                    if (!out)
                        return false;
                }
                std::filesystem::permissions(tmp,
                                             std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                             std::filesystem::perm_options::replace,
                                             ec);
                std::filesystem::rename(tmp, path, ec);
                if (ec) {
                    debug::log(ERR, "secret: cannot replace {}: {}", path, ec.message());
                    std::filesystem::remove(tmp, ec);
                    return false;
                }
                return true;
            }

            // Caller holds g_mutex.
            bool load_key_locked() {
                if (!g_key.empty())
                    return true;
                if (g_failed_once && now_seconds() - g_last_failure < KEY_RETRY_SECONDS)
                    return false;

                auto wait_and_retry = [](const char* why) {
                    debug::log(WARN, "{}", why);
                    g_failed_once = true;
                    g_last_failure = now_seconds();
                    return false;
                };
                auto generate = [](std::vector<unsigned char>& key) {
                    key.assign(KEY_BYTES, 0);
                    return RAND_bytes(key.data(), static_cast<int>(key.size())) == 1;
                };

                std::vector<unsigned char> key;
                switch (key_from_service(key)) {
                case KeyLookup::Found:
                    g_key = std::move(key);
                    g_failed_once = false;
                    return true;

                case KeyLookup::Locked:
                    return wait_and_retry("secret: wallet is locked; retained history is paused until it unlocks");

                case KeyLookup::LockedNoKey:
                    return wait_and_retry(
                        "secret: wallet is locked and holds no store key; history starts once it unlocks");

                case KeyLookup::Absent: {
                    // an earlier run with no secret service already sealed the store.
                    const bool promoted = key_from_file(key);
                    if (!promoted && !generate(key))
                        return false;
                    if (!key_to_service(key))
                        return wait_and_retry("secret: cannot store a new key in the wallet");
                    g_key = std::move(key);
                    g_failed_once = false;
                    debug::log(INFO,
                               promoted ? "secret: adopted the store key file into the wallet"
                                        : "secret: generated a new store key in the wallet");
                    return true;
                }

                case KeyLookup::NoService:
                    break;
                }

                if (key_from_file(key)) {
                    g_key = std::move(key);
                    g_failed_once = false;
                    return true;
                }
                if (!generate(key))
                    return false;
                if (!key_to_file(key))
                    return wait_and_retry("secret: cannot write the store key file");
                g_key = std::move(key);
                g_failed_once = false;
                debug::log(INFO, "secret: no secret service; store key kept in a 0600 file");
                return true;
            }

            bool encrypt(const std::string& plain,
                         const std::vector<unsigned char>& key,
                         std::vector<unsigned char>& out) {
                out.resize(NONCE_BYTES + plain.size() + TAG_BYTES);
                if (RAND_bytes(out.data(), NONCE_BYTES) != 1)
                    return false;

                EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
                if (!ctx)
                    return false;

                bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), out.data()) == 1;
                int length = 0;
                if (ok)
                    ok = EVP_EncryptUpdate(ctx,
                                           out.data() + NONCE_BYTES,
                                           &length,
                                           reinterpret_cast<const unsigned char*>(plain.data()),
                                           static_cast<int>(plain.size())) == 1;
                int total = length;
                if (ok)
                    ok = EVP_EncryptFinal_ex(ctx, out.data() + NONCE_BYTES + total, &length) == 1;
                total += length;
                if (ok)
                    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_BYTES, out.data() + NONCE_BYTES + total) ==
                         1;
                EVP_CIPHER_CTX_free(ctx);

                if (!ok)
                    return false;
                out.resize(NONCE_BYTES + static_cast<size_t>(total) + TAG_BYTES);
                return true;
            }

            bool decrypt(const std::vector<unsigned char>& sealed,
                         const std::vector<unsigned char>& key,
                         std::string& out) {
                if (sealed.size() < NONCE_BYTES + TAG_BYTES)
                    return false;
                const size_t body = sealed.size() - NONCE_BYTES - TAG_BYTES;

                EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
                if (!ctx)
                    return false;

                std::string plain(body, '\0');
                bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), sealed.data()) == 1;
                int length = 0;
                if (ok && body)
                    ok = EVP_DecryptUpdate(ctx,
                                           reinterpret_cast<unsigned char*>(plain.data()),
                                           &length,
                                           sealed.data() + NONCE_BYTES,
                                           static_cast<int>(body)) == 1;
                int total = length;
                if (ok)
                    ok = EVP_CIPHER_CTX_ctrl(ctx,
                                             EVP_CTRL_GCM_SET_TAG,
                                             TAG_BYTES,
                                             const_cast<unsigned char*>(sealed.data() + NONCE_BYTES + body)) == 1;
                // Fails when the tag does not match, which is the whole point.
                if (ok)
                    ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plain.data()) + total, &length) == 1;
                total += length;
                EVP_CIPHER_CTX_free(ctx);

                if (!ok)
                    return false;
                plain.resize(static_cast<size_t>(total));
                out = std::move(plain);
                return true;
            }

        } // namespace

        void set_retention(Retention retention) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_retention = retention;
        }

        Retention retention() {
            std::lock_guard<std::mutex> lock(g_mutex);
            return g_retention;
        }

        bool have_key() {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_retention != Retention::Encrypted)
                return true;
            return load_key_locked();
        }

        std::string seal(const std::string& plain, Retention mode) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (mode == Retention::Plaintext)
                return plain;
            if (mode == Retention::None || !load_key_locked())
                return {};

            std::vector<unsigned char> sealed;
            if (!encrypt(plain, g_key, sealed)) {
                debug::log(ERR, "secret: failed to seal a record");
                return {};
            }
            return base64_encode(sealed);
        }

        bool unseal(const std::string& record, std::string& out, Retention mode) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (mode == Retention::Plaintext) {
                out = record;
                return true;
            }
            if (mode == Retention::None || !load_key_locked())
                return false;
            return decrypt(base64_decode(record), g_key, out);
        }

        std::string seal(const std::string& plain) { return seal(plain, retention()); }

        bool unseal(const std::string& record, std::string& out) { return unseal(record, out, retention()); }

        void reset_for_test() {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_retention = Retention::Encrypted;
            g_key.clear();
            g_failed_once = false;
            g_last_failure = 0;
        }

    } // namespace secret

} // namespace tether
