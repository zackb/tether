#include "tether/bluetooth/connection.hpp"
#include "tether/bluetooth/ancs/client.hpp"
#include "tether/bluetooth/bmessage.hpp"
#include "tether/bluetooth/config.hpp"
#include "tether/bluetooth/contacts.hpp"
#include "tether/bluetooth/groups.hpp"
#include "tether/bluetooth/journal.hpp"
#include "tether/bluetooth/map_session.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/bluetooth/pairing.hpp"
#include "tether/bluetooth/pbap_session.hpp"
#include "tether/log.hpp"
#include "tether/net.hpp"
#include "tether/secret_store.hpp"
#include "tether/session_lock.hpp"
#include <tether/i18n.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <functional>
#include <gio/gio.h>
#include <mutex>
#include <thread>

namespace tether::bluetooth {

    ConnectionManager* g_bt_connections = nullptr;

    namespace {
        MessageStore g_messages;
        std::mutex g_messages_mutex;
        MessageJournal g_journal;
        std::mutex g_map_mutex;
        std::shared_ptr<MapSession> g_map_session;

        std::mutex g_group_mutex;
        GroupCorrelator g_correlator;
        GroupRoster g_rosters;
        std::map<std::string, GroupInfo> g_group_info;
        std::atomic<bool> g_group_replies_enabled{false};

        // MAP folders are navigated from the session root one level at a time.
        // The session parks on the message root and lists its children, so both
        // halves of a conversation can be pulled without walking between them.
        constexpr const char* MAP_MSG_ROOT = "telecom/msg";
        constexpr const char* MAP_OUTBOX = "telecom/msg/outbox";
        constexpr const char* MAP_FOLDER_INBOX = "inbox";
        constexpr const char* MAP_FOLDER_SENT = "sent";
    } // namespace

    namespace {
        bool roster_contradicted(const std::string& thread_key, const std::string& sender_key) {
            const std::vector<std::string>* roster = g_rosters.find(thread_key);
            if (!roster || sender_key.empty())
                return false;
            std::vector<Recipient> members;
            for (const auto& address : *roster) {
                Recipient recipient;
                std::string ignored;
                if (recipient_from_thread_key(address, recipient, ignored))
                    members.push_back(recipient);
            }
            return sender_invalidates_route(sender_key, members);
        }
    } // namespace

    MessageStore& message_store() { return g_messages; }
    std::mutex& message_store_mutex() { return g_messages_mutex; }

    bool send_message(const std::string& thread_key, const std::string& body, Message& sent_out, std::string& err_out) {
        if (body.empty()) {
            err_out = _("Nothing to send.");
            return false;
        }

        std::vector<Recipient> recipients;
        if (thread_key.rfind("group:", 0) == 0) {
            GroupInfo info;
            GroupRoster rosters;
            {
                std::lock_guard<std::mutex> lock(g_group_mutex);
                auto found = g_group_info.find(thread_key);
                if (found != g_group_info.end())
                    info = found->second;
                rosters = g_rosters;
            }
            ReplyEligibility eligibility;
            {
                std::lock_guard<std::mutex> lock(g_messages_mutex);
                eligibility = resolve_group_recipients(
                    info, thread_key, contact_store(), rosters, g_group_replies_enabled, recipients, err_out);
            }
            if (eligibility != ReplyEligibility::Allowed)
                return false;
        } else {
            Recipient recipient;
            if (!recipient_from_thread_key(thread_key, recipient, err_out))
                return false;
            recipients.push_back(recipient);
        }

        std::shared_ptr<MapSession> session;
        {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            session = g_map_session;
        }
        if (!session) {
            err_out = _("Messages are not connected.");
            return false;
        }

        const std::string bmessage = build_bmessage(recipients, body);
        if (bmessage.empty()) {
            err_out = _("Could not build a message for this conversation.");
            return false;
        }

        if (!session->push_message(MAP_OUTBOX, bmessage, err_out))
            return false;

        Message sent;
        sent.thread_key = thread_key;
        sent.peer_address = recipients.size() == 1 ? recipients.front().address : std::string{};
        sent.body = body;
        sent.timestamp = static_cast<int64_t>(std::time(nullptr));
        sent.outgoing = true;
        sent.read = true;
        sent.folder = "outbox";

        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            sent.handle =
                LOCAL_HANDLE_PREFIX + std::to_string(std::time(nullptr)) + "-" + std::to_string(g_messages.size());
            g_messages.add(sent);
            g_journal.append(sent);
        }
        sent_out = sent;
        return true;
    }

    void observe_message_notification(const std::string& sender,
                                      const std::string& subtitle,
                                      const std::string& body,
                                      int64_t now) {
        std::lock_guard<std::mutex> lock(g_group_mutex);
        g_correlator.observe({sender, subtitle, body, now});
        g_correlator.expire(now);
    }

    ReplyEligibility group_reply_status(const std::string& thread_key, std::string& reason) {
        GroupInfo info;
        GroupRoster rosters;
        {
            std::lock_guard<std::mutex> lock(g_group_mutex);
            auto found = g_group_info.find(thread_key);
            if (found != g_group_info.end())
                info = found->second;
            rosters = g_rosters;
        }
        std::vector<Recipient> recipients;
        std::lock_guard<std::mutex> lock(g_messages_mutex);
        return resolve_group_recipients(
            info, thread_key, contact_store(), rosters, g_group_replies_enabled, recipients, reason);
    }

    void discard_retained_messages() {
        std::lock_guard<std::mutex> lock(g_messages_mutex);
        g_journal.close();
        g_messages = MessageStore();
        contact_store() = ContactStore();
    }

    void move_retained_store(Retention previous, Retention next) {
        std::lock_guard<std::mutex> lock(g_messages_mutex);
        g_journal.close();
        migrate_contacts(previous, next);
    }

    void set_group_replies_enabled(bool enabled) {
        std::lock_guard<std::mutex> lock(g_group_mutex);
        g_group_replies_enabled = enabled;
    }

    void reload_group_rosters() {
        std::lock_guard<std::mutex> lock(g_group_mutex);
        g_rosters = load_rosters();
        // re-reading the file is the user's answer to an invalidation.
        for (auto& [key, info] : g_group_info)
            info.route_invalidated = false;
    }

    bool mark_message_read(const std::string& handle, bool read, std::string& err_out, bool* synced_out) {
        if (synced_out)
            *synced_out = false;

        // The stored path is what obexd currently answers on. A message replayed
        // from the journal has none, and one that has scrolled out of the listing
        // window never gets another. The desktop's own read state must still move
        // for those, or the unread badge can never reach zero.
        std::string object_path;
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            if (const Message* message = g_messages.find(handle)) {
                known = true;
                object_path = message->object_path;
            }
        }
        if (!known) {
            err_out = _("That message is no longer in the conversation history.");
            return false;
        }

        std::shared_ptr<MapSession> session;
        {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            session = g_map_session;
        }

        if (session && !object_path.empty()) {
            std::string sync_err;
            if (session->set_read(object_path, read, sync_err)) {
                if (synced_out)
                    *synced_out = true;
            } else {
                err_out = sync_err;
            }
        } else {
            err_out = _("The iPhone is not serving this message right now; marked read on this computer only.");
        }

        std::lock_guard<std::mutex> lock(g_messages_mutex);
        if (g_messages.set_read(handle, read)) {
            // Re-append so the state survives a restart. Retention keeps the last
            // line for a handle, so the newer read state wins on the next load.
            if (const Message* updated = g_messages.find(handle))
                g_journal.append(*updated);
        }
        return true;
    }

    void mark_messages_read_async(std::vector<std::string> handles, bool read) {
        if (handles.empty())
            return;

        std::thread([handles = std::move(handles), read]() {
            int changed = 0;
            int synced = 0;
            std::string last_err;
            for (const auto& handle : handles) {
                std::string err;
                bool handle_synced = false;
                if (mark_message_read(handle, read, err, &handle_synced))
                    ++changed;
                if (handle_synced)
                    ++synced;
                if (!err.empty())
                    last_err = err;
            }
            nlohmann::json event;
            event["command"] = "bt_message_read";
            event["handles"] = handles;
            event["read"] = read;
            event["success"] = changed > 0;
            event["synced"] = synced;
            if (!last_err.empty())
                event["message"] = last_err;
            broadcast_local_event(event.dump());
        }).detach();
    }

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* IFACE_DEVICE = "org.bluez.Device1";
        constexpr const char* IFACE_BEARER_LE = "org.bluez.Bearer.LE1";
        constexpr const char* IFACE_BEARER_BREDR = "org.bluez.Bearer.BREDR1";
        constexpr const char* IFACE_PROPS = "org.freedesktop.DBus.Properties";

        constexpr const char* OBEX_NAME = "org.bluez.obex";
        constexpr const char* OBEX_ROOT = "/org/bluez/obex";
        constexpr const char* OBEX_CLIENT = "org.bluez.obex.Client1";

        constexpr int CONNECT_TIMEOUT_MS = 30000;
        // BlueZ keeps working on a connect long after a caller gives up, and every
        // later attempt then collides with it. The LE connect is issued
        // asynchronously and left to run to completion rather than abandoned, so
        // this is generous on purpose.
        constexpr int LE_CONNECT_TIMEOUT_MS = 45000;

        constexpr int MESSAGE_POLL_SECONDS = 15;
        // obexd can drop a MAP session while the Classic link stays up, and the
        // profile supervisor short-circuits once map_open is set, so nothing else
        // notices. Consecutive listing failures are the cheapest liveness signal.
        constexpr int MAP_FAILURES_BEFORE_REOPEN = 3;

        // The OBEX session object is gone, or its transport died under it.
        auto map_session_dead = [](const std::string& err) {
            return err.find("UnknownObject") != std::string::npos ||
                   err.find("Transport got disconnected") != std::string::npos ||
                   err.find("Transport is not connected") != std::string::npos;
        };
        constexpr int MESSAGE_LIST_MAX = 200;
        // Bodies are fetched one OBEX Get at a time on the connection thread,
        // which also has ANCS and the bearer supervisor. Needs cap to keep the
        // first listing of a full mailbox from stalling all of it
        constexpr size_t MESSAGE_BODY_FETCH_MAX = 20;
        constexpr int SUPERVISOR_TICK_SECONDS = 1;

        // Whether the controller can carry ANCS right now. Re-read rather than
        // latched: see the comment in ConnectionManager::start().
        bool ancs_available() { return g_bluez && g_bluez->capability().mode == DeliveryMode::Full; }

        std::string normalize_address(std::string address) {
            for (char& c : address)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return address;
        }

        // Bearer operations backed by BlueZ on the system bus.
        class BluezBearerOps : public BearerOps {
        public:
            BluezBearerOps(BluezMonitor& monitor, std::string address)
                : monitor_(monitor), address_(normalize_address(std::move(address))) {}

            void set_address(const std::string& address) { address_ = normalize_address(address); }

            bool device_present() const override { return lookup().has_value(); }

            bool device_paired() const override {
                auto device = lookup();
                return device && device->paired;
            }

            bool classic_connected() const override {
                auto device = lookup();
                return device && device->classic_link_up();
            }

            bool le_bearer_available() const override {
                auto device = lookup();
                return device && device->has_le_bearer;
            }

            bool le_connected() const override {
                auto device = lookup();
                return device && link_up(*device);
            }

            std::string preferred_bearer() const override {
                auto device = lookup();
                return device ? device->preferred_bearer : std::string();
            }

            void set_preferred_bearer(const std::string& bearer) override {
                auto device = lookup();
                if (!device)
                    return;
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(
                    monitor_.connection(),
                    BLUEZ_NAME,
                    device->path.c_str(),
                    IFACE_PROPS,
                    "Set",
                    g_variant_new("(ssv)", IFACE_DEVICE, "PreferredBearer", g_variant_new_string(bearer.c_str())),
                    nullptr,
                    G_DBUS_CALL_FLAGS_NONE,
                    5000,
                    nullptr,
                    &error);
                if (reply)
                    g_variant_unref(reply);
                // Absent on older BlueZ; not having it is not an error.
                g_clear_error(&error);
            }

            ConnectResult connect_classic(std::string& err) override {
                auto device = lookup();
                if (!device) {
                    err = "device not present";
                    return ConnectResult::Failed;
                }
                // bluez only exposes the per-bearer Connect while the device is connected on some bearer
                std::string bearer_err;
                if (device->has_classic_bearer && device->connected) {
                    if (call(device->path, IFACE_BEARER_BREDR, "Connect", bearer_err))
                        return ConnectResult::Requested;
                    if (bearer_err.find("InProgress") != std::string::npos ||
                        bearer_err.find("AlreadyConnected") != std::string::npos) {
                        err = bearer_err;
                        return ConnectResult::Failed;
                    }
                }

                if (!link_up(*device))
                    set_preferred_bearer("bredr");
                if (call(device->path, IFACE_DEVICE, "Connect", err))
                    return ConnectResult::Requested;
                // per-bearer refusal is the more specific one
                if (!bearer_err.empty())
                    err = bearer_err;
                return ConnectResult::Failed;
            }

            bool le_connect_outstanding() const override {
                std::lock_guard<std::mutex> lock(le_->mutex);
                return le_->pending;
            }

            void set_telephony_probe(std::function<bool()> probe) { telephony_probe_ = std::move(probe); }

            bool telephony_present() const override {
                if (telephony_probe_)
                    return telephony_probe_();
                auto device = lookup();
                return device && monitor_.snapshot().find_telephony(device->path) != nullptr;
            }

            bool disconnect_classic(std::string& err) override {
                auto device = lookup();
                if (!device) {
                    err = "device not present";
                    return false;
                }
                // Only the per-bearer call: Device1.Disconnect() would take the
                // LE half and the ANCS subscription riding on it.
                if (!device->has_classic_bearer) {
                    err = "no BR/EDR bearer";
                    return false;
                }
                return call(device->path, IFACE_BEARER_BREDR, "Disconnect", err);
            }

            bool solicitation_on_air() const override {
                if (!ancs_solicitation_active())
                    return false;
                const auto objects = monitor_.snapshot();
                auto device = lookup();
                const Adapter* adapter = device ? objects.find_adapter(device->adapter_path) : nullptr;
                return adapter && adapter->advertising_active_instances > 0;
            }

            // Issued asynchronously and never abandoned: a synchronous call that
            // times out leaves BlueZ still connecting, and the next attempt then
            // collides with it forever.
            ConnectResult connect_le(std::string& err) override {
                auto device = lookup();
                if (!device) {
                    err = "device not present";
                    return ConnectResult::Failed;
                }
                if (!device->has_le_bearer) {
                    err = "no LE bearer";
                    return ConnectResult::Failed;
                }

                {
                    std::lock_guard<std::mutex> lock(le_->mutex);
                    if (le_->pending)
                        return ConnectResult::Busy;
                    if (!le_->error.empty()) {
                        err = le_->error;
                        le_->error.clear();
                        return ConnectResult::Failed;
                    }
                    le_->pending = true;
                }

                // Some BlueZ builds publish Bearer.LE1 without Connect; the first
                // reply says which, and every later attempt uses the one that
                // exists.
                if (le_->bearer_connect_missing)
                    set_preferred_bearer("le");
                const char* iface = le_->bearer_connect_missing ? IFACE_DEVICE : IFACE_BEARER_LE;
                auto* holder = new std::shared_ptr<LeConnect>(le_);
                const std::string path = device->path;
                monitor_.invoke_sync([this, path, iface, holder] {
                    g_dbus_connection_call(monitor_.connection(),
                                           BLUEZ_NAME,
                                           path.c_str(),
                                           iface,
                                           "Connect",
                                           nullptr,
                                           nullptr,
                                           G_DBUS_CALL_FLAGS_NONE,
                                           LE_CONNECT_TIMEOUT_MS,
                                           nullptr,
                                           on_le_connected,
                                           holder);
                });
                return ConnectResult::Requested;
            }

        private:
            // Outlives the ops object: a reply can land after the supervised
            // device is torn down and replaced.
            struct LeConnect {
                mutable std::mutex mutex;
                bool pending = false;
                std::string error;
                bool bearer_connect_missing = false;
            };
            std::shared_ptr<LeConnect> le_ = std::make_shared<LeConnect>();

            static void on_le_connected(GObject* source, GAsyncResult* result, gpointer user_data) {
                std::unique_ptr<std::shared_ptr<LeConnect>> holder(static_cast<std::shared_ptr<LeConnect>*>(user_data));
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
                std::string message;
                if (reply)
                    g_variant_unref(reply);
                else {
                    message = error ? error->message : "unknown error";
                    g_clear_error(&error);
                }

                std::lock_guard<std::mutex> lock((*holder)->mutex);
                (*holder)->pending = false;
                // Not a failure of the connect, just the wrong interface. Retry on
                // the next tick against Device1 and report nothing.
                if (message.find("UnknownMethod") != std::string::npos)
                    (*holder)->bearer_connect_missing = true;
                else
                    (*holder)->error = message;
            }

            std::optional<Device> lookup() const {
                for (const auto& device : monitor_.snapshot().devices) {
                    if (normalize_address(device.address) == address_)
                        return device;
                }
                return std::nullopt;
            }

            // A BR/EDR reconnect sets ServicesResolved again while the stale ANCS
            // Notifying flag survives, so the inferred LE link is confirmed with a read.
            bool link_up(const Device& device) const {
                if (device.le_connected)
                    return true;
                return device.le_link_up() && gatt_link_alive(monitor_.connection(), device.gap_name_path);
            }

            bool call(const std::string& path,
                      const char* iface,
                      const char* method,
                      std::string& err,
                      int timeout_ms = CONNECT_TIMEOUT_MS) {
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(monitor_.connection(),
                                                              BLUEZ_NAME,
                                                              path.c_str(),
                                                              iface,
                                                              method,
                                                              nullptr,
                                                              nullptr,
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              timeout_ms,
                                                              nullptr,
                                                              &error);
                if (!reply) {
                    err = error ? error->message : "unknown error";
                    g_clear_error(&error);
                    return false;
                }
                g_variant_unref(reply);
                return true;
            }

            BluezMonitor& monitor_;
            std::string address_;
            std::function<bool()> telephony_probe_;
        };

        // OBEX sessions live on the session bus, not the system bus.
        class ObexProfileOps : public ProfileOps {
        public:
            explicit ObexProfileOps(std::string address) : address_(normalize_address(std::move(address))) {}

            ~ObexProfileOps() override { g_clear_object(&conn_); }

            void set_address(const std::string& address) { address_ = normalize_address(address); }

            // The MAP session driver reuses this connection rather than opening a
            // second one to the same bus.
            GDBusConnection* bus() const { return conn_; }

            std::string create_session(const std::string& target, std::string& err) override {
                if (!ensure_connection(err))
                    return {};

                GVariantBuilder args;
                g_variant_builder_init(&args, G_VARIANT_TYPE("a{sv}"));
                g_variant_builder_add(&args, "{sv}", "Target", g_variant_new_string(target.c_str()));

                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(conn_,
                                                              OBEX_NAME,
                                                              OBEX_ROOT,
                                                              OBEX_CLIENT,
                                                              "CreateSession",
                                                              g_variant_new("(sa{sv})", address_.c_str(), &args),
                                                              G_VARIANT_TYPE("(o)"),
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              CONNECT_TIMEOUT_MS,
                                                              nullptr,
                                                              &error);
                if (!reply) {
                    err = error ? error->message : "unknown error";
                    g_clear_error(&error);
                    return {};
                }

                const gchar* path = nullptr;
                g_variant_get(reply, "(&o)", &path);
                std::string session = path ? path : "";
                g_variant_unref(reply);
                return session;
            }

            void remove_session(const std::string& path) override {
                if (path.empty() || !conn_)
                    return;
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(conn_,
                                                              OBEX_NAME,
                                                              OBEX_ROOT,
                                                              OBEX_CLIENT,
                                                              "RemoveSession",
                                                              g_variant_new("(o)", path.c_str()),
                                                              nullptr,
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              5000,
                                                              nullptr,
                                                              &error);
                if (reply)
                    g_variant_unref(reply);
                g_clear_error(&error);
            }

        private:
            bool ensure_connection(std::string& err) {
                if (conn_)
                    return true;
                GError* error = nullptr;
                conn_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
                if (!conn_) {
                    err = std::string("session bus unavailable: ") + (error ? error->message : "unknown");
                    g_clear_error(&error);
                    return false;
                }
                return true;
            }

            GDBusConnection* conn_ = nullptr;
            std::string address_;
        };

    } // namespace

    struct ConnectionState {
        BluezMonitor* monitor = nullptr;
        ConnectionManager::StatusFn on_change;
        ConnectionManager::MessageFn on_message;

        std::unique_ptr<BluezBearerOps> bearer_ops;
        std::unique_ptr<ObexProfileOps> profile_ops;
        std::unique_ptr<BearerSupervisor> bearers;
        std::unique_ptr<ProfileSupervisor> profiles;

        std::thread thread;
        std::atomic<bool> running{false};
        std::mutex mutex;
        std::condition_variable wake;

        // Serializes start/stop/set_device
        std::mutex lifecycle;

        std::string address;
        // What the user asked for, versus what the controller can currently do.
        bool ancs_preference = true;
        bool ancs_enabled = true;
        bool link_was_ready = false;
        bool device_was_present = false;
        // Whether losing the phone to a supervision timeout locks the session
        std::atomic<bool> lock_on_away{false};
        std::atomic<int> lock_away_seconds{AWAY_LOCK_GRACE_SECONDS};
        // When the last path to the phone went down, -1 while any is up.
        int64_t gone_since = -1;
        // Whether the away lock has already fired for this absence.
        bool lock_fired = false;
        // When the LE bearer was last seen down, so a blip can be told from a
        // session that is really gone. -1 while it is up.
        int64_t le_down_since = -1;
        // When the LE link was first seen up while the phone offered no ANCS
        // service. -1 whenever that is not the case.
        int64_t ancs_absent_since = -1;
        uint64_t le_drops_seen = 0;
        int map_failures = 0;
        int64_t next_message_poll = 0;
        std::string open_map_path;
        // Whether the phone answers a listing for a child of the message root.
        // Cleared once, permanently for the session, if it turns out not to.
        bool map_lists_subfolders = true;
        bool map_lists_sent = true;
        // The first listing on a new MAP session returns the phone's existing
        // inbox, which on a first run is every message it holds.
        bool map_session_backfill = true;
        std::string pulled_pbap_path;

        // The phone's recent calls (PBAP cch), newest first. Memory only.
        std::vector<VCard> call_history;
        mutable std::mutex call_history_mutex;
        // Set by a new PBAP session and by a call ending, which can happen on
        // refresh_calls()'s thread.
        std::atomic<bool> call_history_stale{true};
        ConnectionManager::CallsFn on_call_history;

        std::shared_ptr<ancs::AncsClient> ancs_client;
        mutable std::mutex ancs_mutex;
        ConnectionManager::NotificationFn on_notification;
        ConnectionManager::WithdrawFn on_withdraw;
        bool ancs_ready = false;
        std::string ancs_reason;

        std::atomic<bool> ancs_wanted{true};
        std::atomic<bool> ancs_content_wanted{true};

        std::shared_ptr<TelephonyClient> telephony;
        mutable std::mutex telephony_mutex;
        ConnectionManager::CallsFn on_calls;
        std::atomic<bool> calls_wanted{false};
        bool calls_enabled = false;
        // Last published call list. The mutex also serialises on_calls: the supervisor tick and
        // refresh_calls() both publish.
        nlohmann::json last_calls;
        std::mutex calls_publish_mutex;

        // Brings the telephony client in line with the wanted preference.
        void apply_calls_preference();

        // Publishes the call list when it differs from the last one.
        void sync_calls();

        // The current client, kept alive for as long as the caller.
        std::shared_ptr<TelephonyClient> calls_client() const {
            std::lock_guard<std::mutex> lock(telephony_mutex);
            return telephony;
        }

        void run();

        // Locks the session once the phone has been out of range long enough.
        void supervise_away_lock(int64_t now, bool obex_up);

        void sync_messages(int64_t now);
        void sync_contacts();
        void sync_call_history();
        nlohmann::json call_history_json() const;
        void sync_ancs(int64_t now);

        // Brings the ANCS client and the bearer supervisor in line with the wanted preference and the controller's
        // current capability.
        void apply_ancs_preference();
        void build_ancs_client();

        // The current client, kept alive for as long as the caller.
        std::shared_ptr<ancs::AncsClient> ancs() const {
            std::lock_guard<std::mutex> lock(ancs_mutex);
            return ancs_client;
        }

        // Rebuilds the cached payload from the supervisors. Supervisor thread
        // only: it is the sole writer of BearerStatus and ProfileStatus, both of
        // which hold a std::string that no other thread may read directly.
        void refresh_status();

        // Hands the cached status to the subscriber only when it differs from the
        // last one published. The supervisors report their own internal
        // transitions as changes, and most of those are invisible in the payload,
        // so without this a subscriber receives the same status every second.
        void publish();

        // Guards everything below. publish() is reached from the supervisor
        // thread and from the ANCS readiness callback on the GLib thread, and
        // status() from whichever thread is serving a command.
        mutable std::mutex status_mutex;
        nlohmann::json current_status;
        nlohmann::json last_published;
    };

    void ConnectionState::refresh_status() {
        nlohmann::json payload = to_json(bearers->status(), profiles->status());
        auto client = calls_client();
        payload["calls"] = client ? client->status() : nlohmann::json(nullptr);
        std::lock_guard<std::mutex> lock(status_mutex);
        payload["ancs_ready"] = ancs_ready;
        payload["ancs_reason"] = ancs_reason;
        current_status = std::move(payload);
    }

    void ConnectionState::publish() {
        if (!on_change)
            return;

        nlohmann::json payload;
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            if (current_status.is_null() || current_status == last_published)
                return;
            payload = current_status;
            last_published = payload;
        }
        on_change(payload);
    }

    nlohmann::json to_json(const BearerStatus& bearer, const ProfileStatus& profiles) {
        return {
            {"command", "bt_connection_changed"},
            {"device_present", bearer.device_present},
            {"device_paired", bearer.device_paired},
            {"classic_connected", bearer.classic_connected},
            {"le_available", bearer.le_available},
            {"le_connected", bearer.le_connected},
            {"map_open", profiles.map_open},
            {"pbap_open", profiles.pbap_open},
            {"map_error", to_string(profiles.map_error)},
            {"pbap_error", to_string(profiles.pbap_error)},
            // Two independent halves, so both reasons are reported; the UI shows
            // whichever is actionable.
            {"link_reason", bearer.reason},
            {"profile_reason", profiles.reason},
        };
    }

    // Pulls the inbox and folds it into the store, announcing anything new.
    void ConnectionState::sync_messages(int64_t now) {
        // The journal stays shut when the wallet was locked at startup
        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            if (!g_journal.is_open() && secret::retention() != Retention::None && secret::have_key()) {
                if (g_journal.open()) {
                    // PBAP pull while locked lives only in memory
                    if (contact_store().size() == 0)
                        contact_store() = load_contacts();
                    else
                        save_contacts(contact_store());
                    auto history = g_journal.load(now);
                    for (const auto& message : history)
                        g_messages.add(message);
                    g_journal.compact(history);
                    debug::log(INFO, "bluetooth: wallet unlocked; replayed {} message(s)", history.size());
                }
            }
        }

        if (!profiles->status().map_open) {
            if (!open_map_path.empty()) {
                std::lock_guard<std::mutex> lock(g_map_mutex);
                g_map_session.reset();
                open_map_path.clear();
            }
            return;
        }

        if (open_map_path != profiles->map_session()) {
            open_map_path = profiles->map_session();
            auto session = std::make_shared<MapSession>(profile_ops->bus(), open_map_path);
            {
                std::lock_guard<std::mutex> lock(g_map_mutex);
                g_map_session = session;
            }
            // A freshly opened session starts at the root, and SetFolder walks the
            // MAP hierarchy from there — "msg" on its own is not a valid step.
            std::string err;
            if (!session->set_folder(MAP_MSG_ROOT, err))
                debug::log(WARN, "bluetooth: SetFolder({}) failed: {}", MAP_MSG_ROOT, err);
            next_message_poll = 0;
            map_session_backfill = true;
            map_lists_subfolders = true;
            map_lists_sent = true;
        }

        if (now < next_message_poll)
            return;
        next_message_poll = now + MESSAGE_POLL_SECONDS;

        std::shared_ptr<MapSession> session;
        {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            session = g_map_session;
        }
        if (!session)
            return;

        std::string err;
        // Empty means the current folder; a name means that child of it.
        auto listings = session->list_messages(map_lists_subfolders ? MAP_FOLDER_INBOX : "", MESSAGE_LIST_MAX, err);

        // Some stacks only answer for the folder they are standing in. Walking
        // into the inbox costs the sent folder but keeps messages working, so it
        // is worth one attempt before declaring the session dead.
        if (!err.empty() && map_lists_subfolders && !map_session_dead(err)) {
            debug::log(
                WARN, "bluetooth: listing '{}' failed ({}); falling back to the inbox alone", MAP_FOLDER_INBOX, err);
            map_lists_subfolders = false;
            map_lists_sent = false;
            std::string nav_err;
            // Relative: the session is parked on the message root.
            if (!session->set_folder(MAP_FOLDER_INBOX, nav_err))
                debug::log(WARN, "bluetooth: SetFolder({}) failed: {}", MAP_FOLDER_INBOX, nav_err);
            err.clear();
            listings = session->list_messages("", MESSAGE_LIST_MAX, err);
        }

        if (!err.empty()) {
            debug::log(WARN, "bluetooth: ListMessages failed: {}", err);
            // ponytail: a fail count, not an obexd ObjectManager watcher. Costs up
            // to MAP_FAILURES_BEFORE_REOPEN polls of latency; subscribe to
            // org.bluez.obex InterfacesRemoved if that ever proves too slow.
            if (map_session_dead(err) || ++map_failures >= MAP_FAILURES_BEFORE_REOPEN) {
                debug::log(WARN, "bluetooth: MAP session is not answering; reopening it");
                map_failures = 0;
                profiles->drop_map();
                std::lock_guard<std::mutex> lock(g_map_mutex);
                g_map_session.reset();
                open_map_path.clear();
            }
            return;
        }
        map_failures = 0;

        // Replies sent from the phone itself live in a separate folder and name
        // the user as their sender, so without this a conversation only ever
        // shows one side of itself.
        if (map_lists_sent) {
            std::string sent_err;
            auto sent = session->list_messages(MAP_FOLDER_SENT, MESSAGE_LIST_MAX, sent_err);
            if (!sent_err.empty()) {
                if (!map_session_dead(sent_err)) {
                    debug::log(WARN,
                               "bluetooth: the sent folder is unavailable ({}); showing received messages only",
                               sent_err);
                    map_lists_sent = false;
                }
            } else {
                // An empty sent folder is just a folder with nothing new in it.
                // Only an error means the phone will not serve it.
                listings.insert(listings.end(), sent.begin(), sent.end());
            }
        }

        // A listing reports the message text in Subject, which is a preview.
        // Line breaks arrive flattened to spaces and a long one is cut. The
        // bMessage is the only source of the real body, so every message not
        // seen before costs one OBEX Get.
        struct Pending {
            Message message;
            const MapListing* listing;
        };

        std::vector<Pending> unseen;
        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            for (const auto& listing : listings) {
                Message message = message_from_listing(listing);
                if (message.thread_key.empty()) {
                    debug::log(WARN,
                               "bluetooth: dropping message {} from folder '{}': no usable address "
                               "(sender='{}' address='{}')",
                               listing.handle,
                               listing.folder,
                               listing.sender_name,
                               listing.sender_address);
                    continue;
                }
                if (!g_messages.find(message.handle)) {
                    unseen.push_back({std::move(message), &listing});
                    continue;
                }
                g_messages.add(message);
                g_messages.set_read(message.handle, message.read);
            }
        }

        // Newest first, so when the cap defers part of a backfill the messages
        // the user is looking at are the ones that get their real body now.
        std::sort(unseen.begin(), unseen.end(), [](const Pending& a, const Pending& b) {
            return a.message.timestamp > b.message.timestamp;
        });
        if (unseen.size() > MESSAGE_BODY_FETCH_MAX)
            unseen.resize(MESSAGE_BODY_FETCH_MAX);

        for (auto& pending : unseen) {
            BMessage parsed;
            std::string body_err;
            if (session->fetch_bmessage(pending.listing->handle, parsed, body_err))
                pending.message.body = parsed.body;
            else
                debug::log(DEBUG,
                           "bluetooth: no bMessage for {} ({}); keeping the listing preview",
                           pending.message.handle,
                           body_err);
        }

        std::vector<Message> fresh;
        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            for (auto& pending : unseen) {
                const MapListing& listing = *pending.listing;
                Message& message = pending.message;

                // raw, before anything here can correct or discard it
                if (listing.sent || is_outgoing_folder(listing.folder))
                    debug::log(DEBUG,
                               "bluetooth: phone lists sent msg folder='{}' recipient=[{}] sender=[{}] handle={}",
                               listing.folder,
                               listing.recipient_address,
                               listing.sender_address,
                               listing.handle);

                // MAP gives a group message one sender and no conversation
                // identifier. The correlated Messages notification is the only
                // side channel, and an ambiguous match is refused rather than
                // guessed: filing it wrongly would also aim a reply wrongly.
                if (g_group_replies_enabled) {
                    GroupInfo info;
                    std::lock_guard<std::mutex> group_lock(g_group_mutex);
                    if (g_correlator.correlate(message.body, now, info) == Correlation::Matched) {
                        const std::string key = group_thread_key(info);
                        if (!key.empty()) {

                            const bool stale = !message.outgoing && roster_contradicted(key, message.thread_key);

                            // The sender label is display only and never becomes
                            // part of the thread identity or the reply route.
                            message.peer_name = info.sender;
                            message.thread_key = key;
                            const bool was_stale = g_group_info[key].route_invalidated;
                            g_group_info[key] = info;
                            g_group_info[key].route_invalidated = was_stale || stale;
                        }
                    }
                }
                if (g_messages.add(message))
                    fresh.push_back(message);
                else
                    g_messages.set_read(message.handle, message.read);
            }
        }

        const bool backfill = map_session_backfill;
        map_session_backfill = false;

        for (const auto& message : fresh) {
            {
                std::lock_guard<std::mutex> lock(g_messages_mutex);
                g_journal.append(message);
            }
            if (on_message)
                on_message(message, backfill);
        }
    }

    // Pulls the phonebook once per PBAP session. Contacts change far more slowly
    // than messages, and a full pull is an expensive transfer to repeat.
    void ConnectionState::sync_contacts() {
        if (!profiles->status().pbap_open) {
            pulled_pbap_path.clear();
            return;
        }
        if (pulled_pbap_path == profiles->pbap_session())
            return;
        pulled_pbap_path = profiles->pbap_session();
        call_history_stale = true;

        PbapSession session(profile_ops->bus(), pulled_pbap_path);
        std::string err;
        if (!session.select_phonebook(err)) {
            debug::log(WARN, "bluetooth: PBAP Select failed: {}", err);
            return;
        }

        auto contacts = session.pull_all(err);
        if (!err.empty()) {
            debug::log(WARN, "bluetooth: PBAP PullAll failed: {}", err);
            return;
        }

        // The phone refuses fav when its "Phone Favorites" sync toggle is off.
        std::vector<VCard> favorites;
        if (session.select_phonebook(err, "int", "fav"))
            favorites = session.pull_all(err);
        if (err.empty())
            mark_favorites(contacts, favorites);
        else
            debug::log(INFO, "bluetooth: favorites unavailable: {}", err);

        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            contact_store().set(std::move(contacts));
        }
        save_contacts(contact_store());
        debug::log(INFO, "bluetooth: pulled {} contacts, {} favorites", contact_store().size(), favorites.size());
    }

    // Re-pulls recent calls when stale. Clearing the flag before the pull means a
    // call that ends mid-transfer schedules another pull instead of being missed.
    void ConnectionState::sync_call_history() {
        if (!profiles->status().pbap_open || pulled_pbap_path != profiles->pbap_session())
            return;
        if (!call_history_stale.exchange(false))
            return;

        PbapSession session(profile_ops->bus(), pulled_pbap_path);
        std::string err;
        std::vector<VCard> history;
        if (session.select_phonebook(err, "int", "cch"))
            history = session.pull_all(err, PBAP_MAX_CALLS, PBAP_CALL_FIELDS);
        // The phone refuses cch when its "Phone Recents" sync toggle is off.
        if (!err.empty()) {
            debug::log(INFO, "bluetooth: call history unavailable: {}", err);
            return;
        }

        debug::log(INFO, "bluetooth: pulled {} recent calls", history.size());
        {
            std::lock_guard<std::mutex> lock(call_history_mutex);
            call_history = std::move(history);
        }
        if (on_call_history)
            on_call_history(call_history_json());
    }

    nlohmann::json ConnectionState::call_history_json() const {
        std::vector<VCard> history;
        {
            std::lock_guard<std::mutex> lock(call_history_mutex);
            history = call_history;
        }

        nlohmann::json out = nlohmann::json::array();
        std::lock_guard<std::mutex> lock(g_messages_mutex);
        for (const auto& card : history) {
            const std::string number = card.tels.empty() ? std::string{} : normalize_phone(card.tels.front());
            std::string name = card.name;
            if (name.empty() && !number.empty())
                name = contact_store().name_for("tel:" + number);
            std::string type = card.call_type;
            std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::tolower(c); });

            nlohmann::json entry;
            entry["number"] = number;
            entry["name"] = name;
            entry["type"] = type;
            entry["timestamp"] = parse_map_timestamp(card.call_time);
            out.push_back(std::move(entry));
        }
        return out;
    }

    // Keeps the ANCS client pointed at the LE session and drains its work.
    void ConnectionState::sync_ancs(int64_t now) {
        auto client = ancs();
        if (!client)
            return;

        // ANCS rides the LE bearer, but BlueZ keeps exposing the bonded
        // characteristics while that bearer reads disconnected, and the bearer
        // flaps far faster than a GATT subscription can be rebuilt. Dropping the
        // path on every blip resets discovery and the sequencer, so the readiness
        // probe never lives long enough to be answered. Hold the session across a
        // short outage and let discovery decide; a real loss still clears it,
        // because an empty path is what makes the client forget a dead session.
        const bool le_up = bearers->status().le_connected;
        if (le_up)
            le_down_since = -1;
        else if (le_down_since < 0)
            le_down_since = now;
        // A live subscription outranks the property: an iPhone that is answering
        // ANCS is reachable whatever Bearer.LE1 says, and dropping the path under
        // it would end a session that is working.
        bool ready;
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            ready = ancs_ready;
        }
        // ponytail: one fixed grace window rather than per-phase tuning. Widen it
        // if a controller is seen flapping slower than this.
        const bool hold = ready || le_up || now - le_down_since < ANCS_BEARER_GRACE_SECONDS;

        std::string device_path;
        bool offers_ancs = false;
        if (hold && bearers->status().device_present) {
            auto objects = monitor->snapshot();
            for (const auto& device : objects.devices) {
                if (device.address == address || normalize_address(device.address) == normalize_address(address)) {
                    device_path = device.path;
                    offers_ancs = device.supports_ancs();
                    break;
                }
            }
        }

        // An LE link the daemon dialled can come up with no ANCS at all: the
        // phone never answered a solicitation, so it never offered the service.
        if (ready || !le_up || offers_ancs) {
            ancs_absent_since = -1;
        } else if (ancs_absent_since < 0) {
            ancs_absent_since = now;
        } else if (now - ancs_absent_since >= ANCS_ABSENT_GRACE_SECONDS) {
            std::lock_guard<std::mutex> lock(status_mutex);
            ancs_reason = _("The iPhone is not offering notifications on this link. Asking it again...");
            if (!current_status.is_null())
                current_status["ancs_reason"] = ancs_reason;
        }

        client->set_device(device_path);
        const uint64_t drops = device_path.empty() ? le_drops_seen : monitor->le_drop_count(device_path);
        if (drops != le_drops_seen) {
            le_drops_seen = drops;
            client->drop_session("LE link dropped");
        }
        client->tick(now);
    }

    void ConnectionState::build_ancs_client() {
        auto client = std::make_shared<ancs::AncsClient>(
            *monitor,
            [this](const ancs::Notification& notification) {
                if (on_notification)
                    on_notification(notification);
            },
            [this](uint32_t uid) {
                if (on_withdraw)
                    on_withdraw(uid);
            },
            [this](bool ready, const std::string& reason) {
                {
                    // runs on the glib thread, which must not read supervisor state.
                    std::lock_guard<std::mutex> lock(status_mutex);
                    ancs_ready = ready;
                    ancs_reason = reason;
                    if (!current_status.is_null()) {
                        current_status["ancs_ready"] = ready;
                        current_status["ancs_reason"] = reason;
                    }
                }
                debug::log(INFO, "ancs: {}", reason);
                publish();
            });
        client->set_content_enabled(ancs_content_wanted.load());
        std::lock_guard<std::mutex> lock(ancs_mutex);
        ancs_client = std::move(client);
    }

    void ConnectionState::apply_ancs_preference() {
        ancs_preference = ancs_wanted.load();
        const bool effective = ancs_preference && ancs_available();
        if (effective == ancs_enabled) {
            if (auto client = ancs())
                client->set_content_enabled(ancs_content_wanted.load());
            return;
        }

        ancs_enabled = effective;
        bearers->set_ancs_enabled(effective);
        ancs_absent_since = -1;
        if (effective && on_notification) {
            build_ancs_client();
        } else {
            std::lock_guard<std::mutex> lock(ancs_mutex);
            ancs_client.reset();
        }
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            ancs_ready = false;
            ancs_reason.clear();
        }
        debug::log(INFO, "bluetooth: notification mirroring {}", effective ? "enabled" : "disabled");
    }

    void ConnectionState::apply_calls_preference() {
        const bool wanted = calls_wanted.load() && on_calls;
        if (wanted == calls_enabled)
            return;
        calls_enabled = wanted;
        bearers->set_calls_enabled(wanted);

        std::shared_ptr<TelephonyClient> client;
        if (wanted)
            client = make_telephony_client(*monitor, address);
        {
            std::lock_guard<std::mutex> lock(telephony_mutex);
            telephony = std::move(client);
        }
        std::lock_guard<std::mutex> lock(calls_publish_mutex);
        last_calls = nullptr;
        debug::log(INFO, "bluetooth: call control {}", wanted ? "enabled" : "disabled");
    }

    void ConnectionState::sync_calls() {
        auto client = calls_client();
        std::lock_guard<std::mutex> lock(calls_publish_mutex);
        if (!client) {
            last_calls = nullptr;
            return;
        }
        nlohmann::json calls = client->calls();
        if (calls == last_calls)
            return;
        if (last_calls.is_array() && calls.size() < last_calls.size())
            call_history_stale = true;
        last_calls = calls;
        if (on_calls)
            on_calls(std::move(calls));
    }

    void ConnectionState::supervise_away_lock(int64_t now, bool obex_up) {
        const auto& bearer = bearers->status();
        const bool reachable = bearer.classic_connected || bearer.le_connected || obex_up;

        if (reachable) {
            gone_since = -1;
            lock_fired = false;
            // A link that came back must not be judged on why the last one ended.
            if (monitor)
                monitor->clear_disconnect_reason(address);
            return;
        }

        if (gone_since < 0)
            gone_since = now;

        const bool enabled = lock_on_away.load();
        if (!enabled || lock_fired || !monitor)
            return;

        const std::string reason = monitor->last_disconnect_reason(address);
        if (!should_lock_on_away(enabled, bearer, obex_up, reason, now, gone_since, lock_away_seconds.load()))
            return;

        lock_fired = true;
        debug::log(INFO, "bluetooth: iPhone out of range for {}s, locking the session", now - gone_since);
        if (!lock_session(load_config().lock_command))
            debug::log(WARN, "bluetooth: could not lock the session");
    }

    void ConnectionState::run() {
        using namespace std::chrono;
        const auto started = steady_clock::now();

        while (running) {
            const int64_t now = duration_cast<seconds>(steady_clock::now() - started).count();

            apply_ancs_preference();
            apply_calls_preference();

            const bool obex_up = profiles->status().map_open || profiles->status().pbap_open;

            bearers->tick(now, obex_up);
            const bool present = bearers->status().device_present;
            const bool link_ready = bearers->status().classic_connected;

            // A phone that walks back into range must not sit out a backoff that
            // grew to minutes while it was gone.
            if (present && !device_was_present)
                bearers->reset();
            device_was_present = present;

            // A dropped Classic link invalidates every OBEX session on top of it,
            // along with any backoff accumulated against the link that just died.
            if (link_was_ready && !link_ready) {
                profiles->reset();
                bearers->reset();
                map_failures = 0;
            }
            link_was_ready = link_ready;

            profiles->tick(now, bearers->status().device_paired);

            const auto& bearer = bearers->status();

            supervise_away_lock(now, obex_up);

            supervise_ancs_solicitation(*monitor, should_solicit_ancs(ancs_enabled, bearer, now, ancs_absent_since));

            // publish() drops a payload identical to the last one, so refreshing
            // every tick costs nothing and cannot miss a transition.
            refresh_status();
            publish();

            sync_contacts();
            sync_call_history();
            sync_messages(now);
            sync_ancs(now);
            sync_calls();

            std::unique_lock<std::mutex> lock(mutex);
            wake.wait_for(lock, seconds(SUPERVISOR_TICK_SECONDS), [this] { return !running; });
        }
    }

    ConnectionManager::ConnectionManager(BluezMonitor& monitor, StatusFn on_change, MessageFn on_message)
        : state_(std::make_unique<ConnectionState>()) {
        state_->monitor = &monitor;
        state_->on_change = std::move(on_change);
        state_->on_message = std::move(on_message);
    }

    ConnectionManager::~ConnectionManager() { stop(); }

    bool ConnectionManager::start(const std::string& address, bool ancs_enabled) {
        std::lock_guard<std::mutex> lock(state_->lifecycle);
        return start_locked(address, ancs_enabled);
    }

    void ConnectionManager::stop() {
        std::lock_guard<std::mutex> lock(state_->lifecycle);
        stop_locked();
    }

    bool ConnectionManager::start_locked(const std::string& address, bool ancs_enabled) {
        if (state_->running)
            return true;
        if (address.empty()) {
            debug::log(INFO, "bluetooth: no paired iPhone selected; supervision idle");
            return false;
        }

        state_->address = address;
        state_->ancs_preference = ancs_enabled;
        state_->ancs_wanted = ancs_enabled;
        const Config config = load_config();
        state_->ancs_content_wanted = config.ancs_content_enabled;
        state_->calls_wanted = config.calls_enabled;
        state_->lock_on_away = config.lock_on_away;
        state_->lock_away_seconds = config.lock_away_seconds;
        // Read the controller's capability here rather than letting each caller
        // latch its own copy. At startup BlueZ may not have finished enumerating
        // the adapter, and its free advertising-instance count drops to zero
        // while one of our own adverts is registered — a caller that sampled
        // either would disable mirroring for the rest of the daemon's life.
        const bool ancs_effective = ancs_enabled && ancs_available();
        state_->ancs_enabled = ancs_effective;

        // Replay history before the first poll so the store is never briefly
        // empty, and so previously seen handles dedupe instead of re-announcing.
        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            contact_store() = load_contacts();
            if (g_journal.open()) {
                const int64_t now = static_cast<int64_t>(std::time(nullptr));
                auto history = g_journal.load(now);
                for (const auto& message : history)
                    g_messages.add(message);
                g_journal.compact(history);
                debug::log(INFO, "bluetooth: replayed {} message(s) from the journal", history.size());
            }
        }
        state_->bearer_ops = std::make_unique<BluezBearerOps>(*state_->monitor, address);
        state_->bearer_ops->set_telephony_probe([state = state_.get()] {
            auto client = state->calls_client();
            return client && client->present();
        });
        state_->profile_ops = std::make_unique<ObexProfileOps>(address);
        state_->bearers = std::make_unique<BearerSupervisor>(*state_->bearer_ops, ancs_effective);
        state_->profiles = std::make_unique<ProfileSupervisor>(*state_->profile_ops);

        // Only built when the caller can actually show notifications and the
        // bond can carry them.
        if (ancs_effective && state_->on_notification)
            state_->build_ancs_client();

        state_->running = true;
        state_->thread = std::thread([this] { state_->run(); });
        debug::log(INFO, "bluetooth: supervising {} (ANCS {})", address, ancs_effective ? "enabled" : "disabled");
        return true;
    }

    void ConnectionManager::stop_locked() {
        if (!state_->running && !state_->thread.joinable())
            return;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->running = false;
        }
        state_->wake.notify_all();
        if (state_->thread.joinable())
            state_->thread.join();

        // MapSession borrows profile_ops' bus connection, which is released
        // below. Drop the session before anything can hand out a pointer into a
        // connection that is about to go away.
        {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            g_map_session.reset();
        }

        // Everything below is scoped to one device and one set of sessions, and
        // must not leak into the next start().
        {
            std::lock_guard<std::mutex> lock(state_->ancs_mutex);
            state_->ancs_client.reset();
        }

        {
            std::lock_guard<std::mutex> lock(state_->telephony_mutex);
            state_->telephony.reset();
        }
        state_->calls_enabled = false;
        state_->last_calls = nullptr;

        // The supervisors borrow the ops objects — ~ProfileSupervisor removes the
        // obexd sessions through them — so they go first, and start() only ever
        // builds into empty pointers.
        state_->profiles.reset();
        state_->bearers.reset();
        state_->profile_ops.reset();
        state_->bearer_ops.reset();

        state_->open_map_path.clear();
        state_->pulled_pbap_path.clear();
        {
            std::lock_guard<std::mutex> lock(state_->call_history_mutex);
            state_->call_history.clear();
        }
        state_->map_session_backfill = true;
        state_->map_failures = 0;
        state_->next_message_poll = 0;
        state_->link_was_ready = false;
        state_->device_was_present = false;
        state_->ancs_absent_since = -1;
        {
            std::lock_guard<std::mutex> lock(state_->status_mutex);
            state_->current_status = nullptr;
            state_->last_published = nullptr;
            state_->ancs_ready = false;
            state_->ancs_reason.clear();
        }
    }

    void ConnectionManager::set_notification_handlers(NotificationFn on_notification, WithdrawFn on_withdraw) {
        state_->on_notification = std::move(on_notification);
        state_->on_withdraw = std::move(on_withdraw);
    }

    void ConnectionManager::set_ancs_content_enabled(bool enabled) { state_->ancs_content_wanted = enabled; }

    bool ConnectionManager::perform_notification_action(uint32_t uid, ancs::ActionId action) {
        auto client = state_->ancs();
        return client && client->perform_action(uid, action);
    }

    nlohmann::json ConnectionManager::notifications(size_t limit) const {
        nlohmann::json out = nlohmann::json::array();
        auto client = state_->ancs();
        if (!client)
            return out;
        for (const auto& notification : client->recent_notifications(limit))
            out.push_back(ancs::to_json(notification));
        return out;
    }

    void ConnectionManager::set_call_handler(CallsFn on_calls) { state_->on_calls = std::move(on_calls); }

    void ConnectionManager::refresh_calls() { state_->sync_calls(); }

    void ConnectionManager::set_call_history_handler(CallsFn on_history) {
        state_->on_call_history = std::move(on_history);
    }

    nlohmann::json ConnectionManager::call_history() const { return state_->call_history_json(); }

    void ConnectionManager::set_calls_enabled(bool enabled) { state_->calls_wanted = enabled; }

    void ConnectionManager::set_lock_on_away(bool enabled, int grace_seconds) {
        state_->lock_on_away = enabled;
        state_->lock_away_seconds = grace_seconds;
    }

    bool ConnectionManager::dial(const std::string& number, std::string& err) {
        auto client = state_->calls_client();
        if (!client) {
            err = _("Call control is off.");
            return false;
        }
        return client->dial(number, err);
    }

    bool ConnectionManager::call_action(const std::string& path, const std::string& action, std::string& err) {
        auto client = state_->calls_client();
        if (!client) {
            err = _("Call control is off.");
            return false;
        }
        return client->call_action(path, action, err);
    }

    bool ConnectionManager::call_tones(const std::string& tones, std::string& err) {
        auto client = state_->calls_client();
        if (!client) {
            err = _("Call control is off.");
            return false;
        }
        return client->send_tones(tones, err);
    }

    bool ConnectionManager::calls_available() const {
        auto client = state_->calls_client();
        return client && client->available();
    }

    nlohmann::json ConnectionManager::calls() const {
        auto client = state_->calls_client();
        if (!client)
            return nlohmann::json::array();

        nlohmann::json calls = client->calls();
        std::lock_guard<std::mutex> lock(g_messages_mutex);
        for (auto& call : calls) {
            const std::string number = call.value("number", "");
            if (number.empty() || !call.value("name", "").empty())
                continue;
            if (std::string name = contact_store().name_for("tel:" + number); !name.empty())
                call["name"] = std::move(name);
        }
        return calls;
    }

    void ConnectionManager::set_device(const std::string& address, bool ancs_enabled) {
        // Restarting is simpler than mutating supervisor state under the worker,
        // and selecting a device is rare. stop() clears the per-device state.
        std::lock_guard<std::mutex> lock(state_->lifecycle);
        stop_locked();
        start_locked(address, ancs_enabled);
    }

    void ConnectionManager::set_ancs_enabled(bool enabled) { state_->ancs_wanted = enabled; }

    nlohmann::json ConnectionManager::status() const {
        // The supervisors' own status objects belong to the supervisor thread;
        // callers get the snapshot it published instead.
        std::lock_guard<std::mutex> lock(state_->status_mutex);
        if (state_->current_status.is_null()) {
            nlohmann::json idle;
            idle["command"] = "bt_connection_changed";
            idle["device_present"] = false;
            idle["device_paired"] = false;
            idle["link_reason"] = _("No paired iPhone selected.");
            idle["profile_reason"] = "";
            return idle;
        }
        return state_->current_status;
    }

} // namespace tether::bluetooth
