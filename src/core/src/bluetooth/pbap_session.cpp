#include "tether/bluetooth/pbap_session.hpp"
#include "tether/bluetooth/obex_transfer.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

namespace tether::bluetooth {

    namespace {

        constexpr const char* OBEX_NAME = "org.bluez.obex";
        constexpr const char* IFACE_PBAP = "org.bluez.obex.PhonebookAccess1";

        constexpr int POLL_INTERVAL_MS = 200;

        bool take_error(GError*& error, std::string& err_out) {
            err_out = error ? error->message : "unknown error";
            g_clear_error(&error);
            return false;
        }

        // The phonebook is personal data, so it is staged in the per-user runtime
        // directory rather than a world-readable /tmp.
        std::filesystem::path staging_file() {
            const char* runtime = getenv("XDG_RUNTIME_DIR");
            std::filesystem::path dir =
                runtime && *runtime ? std::filesystem::path(runtime) : std::filesystem::temp_directory_path();
            return dir / ("tether-pbap-" + std::to_string(getpid()) + ".vcf");
        }

        std::string read_file(const std::filesystem::path& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open())
                return {};
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }

    } // namespace

    struct PbapSessionState {
        GDBusConnection* bus = nullptr;
        std::string path;
    };

    PbapSession::PbapSession(GDBusConnection* session_bus, std::string session_path)
        : state_(std::make_unique<PbapSessionState>()) {
        state_->bus = session_bus;
        state_->path = std::move(session_path);
    }

    PbapSession::~PbapSession() = default;

    const std::string& PbapSession::path() const { return state_->path; }

    bool PbapSession::select_phonebook(std::string& err, const std::string& location, const std::string& phonebook) {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(state_->bus,
                                                      OBEX_NAME,
                                                      state_->path.c_str(),
                                                      IFACE_PBAP,
                                                      "Select",
                                                      g_variant_new("(ss)", location.c_str(), phonebook.c_str()),
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      PBAP_CALL_TIMEOUT_MS,
                                                      nullptr,
                                                      &error);
        if (!reply)
            return take_error(error, err);
        g_variant_unref(reply);
        return true;
    }

    std::vector<VCard> PbapSession::pull_all(std::string& err, int max) {
        std::vector<VCard> contacts;

        const std::filesystem::path target = staging_file();
        std::error_code ec;
        std::filesystem::remove(target, ec);

        GVariantBuilder filter;
        g_variant_builder_init(&filter, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&filter, "{sv}", "Format", g_variant_new_string("vcard30"));
        // MaxCount, not MAP's MaxListCount.
        g_variant_builder_add(&filter, "{sv}", "MaxCount", g_variant_new_uint16(static_cast<guint16>(max)));

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(state_->bus,
                                                      OBEX_NAME,
                                                      state_->path.c_str(),
                                                      IFACE_PBAP,
                                                      "PullAll",
                                                      g_variant_new("(sa{sv})", target.c_str(), &filter),
                                                      G_VARIANT_TYPE("(oa{sv})"),
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      PBAP_CALL_TIMEOUT_MS,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            take_error(error, err);
            return contacts;
        }

        const gchar* transfer_path = nullptr;
        GVariant* props = nullptr;
        g_variant_get(reply, "(&o@a{sv})", &transfer_path, &props);
        const std::string transfer = transfer_path ? transfer_path : "";
        if (props)
            g_variant_unref(props);
        g_variant_unref(reply);

        if (transfer.empty()) {
            err = "obexd returned no transfer object";
            return contacts;
        }

        const TransferState state = wait_for_transfer(state_->bus, transfer, PBAP_TRANSFER_TIMEOUT_SECONDS);

        if (state == TransferState::Error) {
            err = "phonebook transfer failed";
            std::filesystem::remove(target, ec);
            return contacts;
        }
        if (state == TransferState::Active) {
            err = "phonebook transfer timed out";
            std::filesystem::remove(target, ec);
            return contacts;
        }

        // Both Complete and Gone are terminal, and in either case the file can
        // still be a moment behind, so wait a bounded time for it to appear.
        const auto grace = std::chrono::steady_clock::now() + std::chrono::seconds(PBAP_FILE_GRACE_SECONDS);
        while (!std::filesystem::exists(target, ec) && std::chrono::steady_clock::now() < grace)
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));

        const std::string text = read_file(target);
        std::filesystem::remove(target, ec);

        if (text.empty()) {
            err = "phonebook transfer produced no data";
            return contacts;
        }

        contacts = parse_vcards(text);
        return contacts;
    }

} // namespace tether::bluetooth
