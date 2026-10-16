#include "tether/bluetooth/ancs/client.hpp"
#include "tether/bluetooth/ancs/sequencer.hpp"
#include "tether/bluetooth/bmessage.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/log.hpp"

#include <atomic>
#include <ctime>
#include <deque>
#include <gio/gio.h>
#include <mutex>

namespace tether::bluetooth::ancs {

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* IFACE_CHARACTERISTIC = "org.bluez.GattCharacteristic1";
        constexpr const char* IFACE_PROPS = "org.freedesktop.DBus.Properties";
        constexpr const char* OBJECT_MANAGER = "org.freedesktop.DBus.ObjectManager";

        constexpr int CALL_TIMEOUT_MS = 10000;

    } // namespace

    struct AncsClientState {
        BluezMonitor* monitor = nullptr;
        AncsClient::NotificationFn on_notification;
        AncsClient::WithdrawFn on_withdraw;
        AncsClient::StatusFn on_status;

        std::string device_path;
        // The last non-empty device path, so an LE teardown can be told apart
        // from actually being pointed at a different phone.
        std::string last_known_device;
        std::string notification_source_path;
        std::string control_point_path;
        std::string data_source_path;

        bool subscribed = false;

        std::atomic<bool> ready{false};
        bool content_enabled = false;
        std::string reason = "Waiting for the iPhone's notification service.";

        int64_t next_discover = 0;
        int64_t next_subscribe = 0;
        int64_t next_probe = 0;
        int64_t next_verify = 0;

        // Guards everything that crosses a thread boundary: the two GATT buffers
        // filled on the GLib thread, the characteristic paths that thread compares
        // against, and actions queued by whichever thread is serving a command.
        std::mutex inbox;
        std::deque<std::vector<uint8_t>> pending_source;
        std::deque<std::vector<uint8_t>> pending_data;
        std::deque<Request> pending_actions;

        // The registry is written by tick() and read by whichever thread answers
        // a "list notifications" command.
        mutable std::mutex registry_mutex;

        guint source_signal = 0;
        guint data_signal = 0;

        NotificationRegistry registry;
        std::unique_ptr<ControlPointSequencer> sequencer;

        // UIDs whose attributes are being fetched, so a notification can be
        // rebuilt when its response lands.
        std::map<uint32_t, SourceEvent> in_progress;

        // Bundle id -> display name, as answered by GetAppAttributes. Only
        // touched from tick()'s thread. An entry with an empty value marks a
        // request already in flight, so an app is asked about once per session.
        std::map<std::string, std::string> app_names;

        bool write_control_point(const std::vector<uint8_t>& payload);
        void set_status(bool now_ready, const std::string& text);
        bool discover();
        bool subscribe();
        void unsubscribe();
        void drop_gatt_paths();
        void verify_subscription();
        void rebuild(const std::string& reason);
        bool link_alive() const;
        void handle_source_event(const SourceEvent& event, int64_t now);
        void handle_response(const Request& request, const Response& response, int64_t now);
        std::string app_display_name(const std::string& app_id);
    };

    namespace {

        void on_properties_changed(GDBusConnection*,
                                   const gchar*,
                                   const gchar* object_path,
                                   const gchar*,
                                   const gchar*,
                                   GVariant* parameters,
                                   gpointer user_data) {
            auto* state = static_cast<AncsClientState*>(user_data);
            if (!state || !object_path)
                return;

            const gchar* iface = nullptr;
            GVariant* changed = nullptr;
            GVariantIter* invalidated = nullptr;
            g_variant_get(parameters, "(&s@a{sv}as)", &iface, &changed, &invalidated);
            if (invalidated)
                g_variant_iter_free(invalidated);
            if (!changed)
                return;

            GVariant* value = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
            g_variant_unref(changed);
            if (!value)
                return;

            gsize length = 0;
            const guchar* bytes = static_cast<const guchar*>(g_variant_get_fixed_array(value, &length, 1));
            if (bytes && length) {
                std::vector<uint8_t> copy(bytes, bytes + length);
                std::lock_guard<std::mutex> lock(state->inbox);
                if (state->notification_source_path == object_path)
                    state->pending_source.push_back(std::move(copy));
                else if (state->data_source_path == object_path)
                    state->pending_data.push_back(std::move(copy));
            }
            g_variant_unref(value);
        }

    } // namespace

    void AncsClientState::set_status(bool now_ready, const std::string& text) {
        if (ready.load() == now_ready && reason == text)
            return;
        ready = now_ready;
        reason = text;
        if (on_status)
            on_status(ready.load(), reason);
    }

    bool AncsClientState::write_control_point(const std::vector<uint8_t>& payload) {
        if (control_point_path.empty() || !monitor || !monitor->connection())
            return false;

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));

        GVariant* value =
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, payload.data(), payload.size(), sizeof(uint8_t));

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(monitor->connection(),
                                                      BLUEZ_NAME,
                                                      control_point_path.c_str(),
                                                      IFACE_CHARACTERISTIC,
                                                      "WriteValue",
                                                      g_variant_new("(@aya{sv})", value, &options),
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      CALL_TIMEOUT_MS,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            const std::string message = error ? error->message : "unknown error";
            g_clear_error(&error);
            // Until the user approves the phone's prompt, the control point
            // refuses every write. That is a pending permission, not a fault.
            debug::log(WARN, "ancs: control point write failed: {}", message);
            return false;
        }
        g_variant_unref(reply);
        return true;
    }

    bool AncsClientState::discover() {
        if (!monitor || !monitor->connection() || device_path.empty())
            return false;

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(monitor->connection(),
                                                      BLUEZ_NAME,
                                                      "/",
                                                      OBJECT_MANAGER,
                                                      "GetManagedObjects",
                                                      nullptr,
                                                      G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      CALL_TIMEOUT_MS,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            g_clear_error(&error);
            return false;
        }

        // Collected locally, then published under `inbox`: the GLib thread
        // compares incoming values against these paths.
        std::string source_path, control_path, data_path;

        GVariant* objects = g_variant_get_child_value(reply, 0);
        GVariantIter iter;
        const gchar* path = nullptr;
        GVariant* interfaces = nullptr;
        g_variant_iter_init(&iter, objects);
        while (g_variant_iter_loop(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
            const std::string object_path = path ? path : "";
            // Characteristics live under the device that owns them.
            if (object_path.rfind(device_path + "/", 0) != 0)
                continue;

            GVariant* props = g_variant_lookup_value(interfaces, IFACE_CHARACTERISTIC, G_VARIANT_TYPE("a{sv}"));
            if (!props)
                continue;

            GVariant* uuid_value = g_variant_lookup_value(props, "UUID", G_VARIANT_TYPE_STRING);
            if (uuid_value) {
                const std::string uuid = g_variant_get_string(uuid_value, nullptr);
                if (uuid == UUID_NOTIFICATION_SOURCE)
                    source_path = object_path;
                else if (uuid == UUID_CONTROL_POINT)
                    control_path = object_path;
                else if (uuid == UUID_DATA_SOURCE)
                    data_path = object_path;
                g_variant_unref(uuid_value);
            }
            g_variant_unref(props);
        }
        g_variant_unref(objects);
        g_variant_unref(reply);

        {
            std::lock_guard<std::mutex> lock(inbox);
            notification_source_path = source_path;
            control_point_path = control_path;
            data_source_path = data_path;
        }

        // All three are needed. A partial tree means BlueZ is still enumerating.
        return !source_path.empty() && !control_path.empty() && !data_path.empty();
    }

    bool AncsClientState::subscribe() {
        if (!monitor || !monitor->connection())
            return false;

        // cached tree can accept StartNotify with no link
        if (!link_alive())
            return false;

        GDBusConnection* conn = monitor->connection();
        bool object_gone = false;
        auto start_notify = [&](const std::string& path) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(conn,
                                                          BLUEZ_NAME,
                                                          path.c_str(),
                                                          IFACE_CHARACTERISTIC,
                                                          "StartNotify",
                                                          nullptr,
                                                          nullptr,
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          CALL_TIMEOUT_MS,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                // StartNotify can race GATT readiness even after the
                // characteristics appear, so a plain failure is retried rather
                // than treated as a reason to rediscover. UnknownObject is the
                // exception: that path is gone and no retry can bring it back.
                const std::string message = error ? error->message : "unknown";
                g_clear_error(&error);
                object_gone = message.find("UnknownObject") != std::string::npos;
                debug::log(INFO, "ancs: StartNotify not ready yet ({})", message);
                return false;
            }
            g_variant_unref(reply);
            return true;
        };

        if (!start_notify(data_source_path) || !start_notify(notification_source_path)) {
            if (object_gone) {
                debug::log(INFO, "ancs: the characteristics moved, rediscovering the notification service");
                drop_gatt_paths();
            }
            return false;
        }

        monitor->invoke_sync([this] {
            GDBusConnection* bus = monitor->connection();
            if (!source_signal) {
                source_signal = g_dbus_connection_signal_subscribe(bus,
                                                                   BLUEZ_NAME,
                                                                   IFACE_PROPS,
                                                                   "PropertiesChanged",
                                                                   notification_source_path.c_str(),
                                                                   nullptr,
                                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                                   on_properties_changed,
                                                                   this,
                                                                   nullptr);
            }
            if (!data_signal) {
                data_signal = g_dbus_connection_signal_subscribe(bus,
                                                                 BLUEZ_NAME,
                                                                 IFACE_PROPS,
                                                                 "PropertiesChanged",
                                                                 data_source_path.c_str(),
                                                                 nullptr,
                                                                 G_DBUS_SIGNAL_FLAGS_NONE,
                                                                 on_properties_changed,
                                                                 this,
                                                                 nullptr);
            }
        });
        return true;
    }

    void AncsClientState::unsubscribe() {
        if (!monitor || !monitor->connection())
            return;
        GDBusConnection* conn = monitor->connection();
        if (subscribed) {
            for (const std::string& path : {data_source_path, notification_source_path}) {
                if (path.empty())
                    continue;
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(conn,
                                                              BLUEZ_NAME,
                                                              path.c_str(),
                                                              IFACE_CHARACTERISTIC,
                                                              "StopNotify",
                                                              nullptr,
                                                              nullptr,
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              2000,
                                                              nullptr,
                                                              &error);
                if (reply)
                    g_variant_unref(reply);
                g_clear_error(&error);
            }
        }
        if (source_signal) {
            g_dbus_connection_signal_unsubscribe(conn, source_signal);
            source_signal = 0;
        }
        if (data_signal) {
            g_dbus_connection_signal_unsubscribe(conn, data_signal);
            data_signal = 0;
        }
    }

    // The cached characteristic paths belong to one GATT session. Cycling
    // Bluetooth on the phone rebuilds the tree under fresh paths, and BlueZ then
    // answers every call on the old ones with UnknownObject forever. Only
    // rediscovery recovers, so drop what is cached and let the next tick run it.
    void AncsClientState::drop_gatt_paths() {
        unsubscribe();
        subscribed = false;
        next_subscribe = 0;
        next_discover = 0;
        std::lock_guard<std::mutex> lock(inbox);
        notification_source_path.clear();
        control_point_path.clear();
        data_source_path.clear();
    }

    // `subscribed` records that StartNotify once succeeded, and `ready` that iOS once answered on the Data Source.
    void AncsClientState::verify_subscription() {
        if (!monitor || !monitor->connection())
            return;
        GDBusConnection* conn = monitor->connection();

        bool object_gone = false;
        bool notifying = true;
        for (const std::string& path : {notification_source_path, data_source_path}) {
            if (path.empty())
                continue;
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(conn,
                                                          BLUEZ_NAME,
                                                          path.c_str(),
                                                          IFACE_PROPS,
                                                          "Get",
                                                          g_variant_new("(ss)", IFACE_CHARACTERISTIC, "Notifying"),
                                                          G_VARIANT_TYPE("(v)"),
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          CALL_TIMEOUT_MS,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                object_gone = true;
                g_clear_error(&error);
                continue;
            }
            GVariant* boxed = nullptr;
            g_variant_get(reply, "(v)", &boxed);
            if (!boxed || !g_variant_is_of_type(boxed, G_VARIANT_TYPE_BOOLEAN) || !g_variant_get_boolean(boxed))
                notifying = false;
            if (boxed)
                g_variant_unref(boxed);
            g_variant_unref(reply);
        }

        // Notifying stays set on a cached tree after the LE link drops.
        const bool link_gone = !object_gone && notifying && !link_alive();
        if (!object_gone && notifying && !link_gone)
            return;

        if (object_gone) {
            debug::log(INFO,
                       "ancs: the notification subscription is no longer live (characteristics gone), rebuilding it");
            drop_gatt_paths();
            set_status(false, "Waiting for the iPhone's notification service.");
            sequencer->reset();
            in_progress.clear();
        } else {
            rebuild(link_gone ? "LE link gone" : "BlueZ cleared Notifying");
        }
    }

    void AncsClientState::rebuild(const std::string& reason) {
        debug::log(INFO, "ancs: the notification subscription is no longer live ({}), rebuilding it", reason);
        unsubscribe();
        subscribed = false;
        next_subscribe = 0;
        set_status(false, "Subscribing to the iPhone's notifications...");
        sequencer->reset();
        in_progress.clear();
    }

    bool AncsClientState::link_alive() const {
        for (const auto& device : monitor->snapshot().devices) {
            if (device.path == device_path)
                return gatt_link_alive(monitor->connection(), device.gap_name_path);
        }
        return true;
    }

    void AncsClientState::handle_source_event(const SourceEvent& event, int64_t now) {
        (void)now;
        Decision decision;
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            decision = registry.classify(event);
            if (decision == Decision::Ignore)
                registry.remember(event);
            else if (decision == Decision::Withdraw)
                registry.forget(event.uid);
        }
        switch (decision) {
        case Decision::Ignore:
            return;
        case Decision::Withdraw:
            if (on_withdraw)
                on_withdraw(event.uid);
            return;
        case Decision::Fetch:
            break;
        }

        if (event.pre_existing() && sequencer->queued() >= MAX_QUEUED_REQUESTS / 2)
            return;

        std::vector<NotificationAttributeId> attributes{NotificationAttributeId::AppIdentifier,
                                                        NotificationAttributeId::Date};
        if (content_enabled) {
            attributes.push_back(NotificationAttributeId::Title);
            attributes.push_back(NotificationAttributeId::Subtitle);
            attributes.push_back(NotificationAttributeId::Message);
        }

        in_progress[event.uid] = event;
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            registry.remember(event);
        }
        if (!sequencer->submit(build_notification_request(event.uid, attributes)))
            in_progress.erase(event.uid);
    }

    // Never blocks on the phone: the derived name is used now and the real one
    // backfills the registry when GetAppAttributes answers.
    std::string AncsClientState::app_display_name(const std::string& app_id) {
        if (app_id.empty())
            return {};
        auto [entry, inserted] = app_names.try_emplace(app_id);
        if (inserted)
            sequencer->submit(build_app_request(app_id));
        return entry->second.empty() ? derive_app_name(app_id) : entry->second;
    }

    void AncsClientState::handle_response(const Request& request, const Response& response, int64_t now) {
        if (response.command == CommandId::GetAppAttributes) {
            // The readiness probe: a Data Source answer proves the phone has
            // authorized notification access, which StartNotify alone does not.
            if (!ready)
                set_status(true, "Notification mirroring is active.");

            const std::string name = response.attribute(static_cast<uint8_t>(AppAttributeId::DisplayName));
            if (!response.app_id.empty() && !name.empty()) {
                app_names[response.app_id] = name;
                // The name arrives after the notifications that triggered the
                // lookup were already stored and broadcast.
                std::lock_guard<std::mutex> lock(registry_mutex);
                registry.rename_app(response.app_id, name);
            }
            return;
        }
        if (response.command != CommandId::GetNotificationAttributes)
            return;

        if (response.uid != request.uid) {
            debug::log(
                WARN, "ancs: discarding a response for uid {} while {} was in flight", response.uid, request.uid);
            return;
        }

        auto found = in_progress.find(request.uid);
        if (found == in_progress.end())
            return;
        const SourceEvent event = found->second;
        in_progress.erase(found);

        Notification notification;
        notification.uid = request.uid;
        notification.app_id = response.attribute(static_cast<uint8_t>(NotificationAttributeId::AppIdentifier));
        notification.app_name = app_display_name(notification.app_id);
        notification.title = response.attribute(static_cast<uint8_t>(NotificationAttributeId::Title));
        notification.subtitle = response.attribute(static_cast<uint8_t>(NotificationAttributeId::Subtitle));
        notification.body = response.attribute(static_cast<uint8_t>(NotificationAttributeId::Message));
        notification.category = event.category;
        notification.silent = event.silent();
        notification.has_positive_action = event.has_positive_action();
        notification.has_negative_action = event.has_negative_action();
        notification.pre_existing = event.pre_existing();
        // ANCS dates are the phone's wall clock with no zone, some apps send none at all.
        const int64_t delivered =
            parse_map_timestamp(response.attribute(static_cast<uint8_t>(NotificationAttributeId::Date)));
        notification.received = delivered ? delivered : now;

        bool should_deliver = false;
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            should_deliver = registry.store(notification);
        }
        if (should_deliver && on_notification)
            on_notification(notification);
    }

    AncsClient::AncsClient(BluezMonitor& monitor,
                           NotificationFn on_notification,
                           WithdrawFn on_withdraw,
                           StatusFn on_status)
        : state_(std::make_unique<AncsClientState>()) {
        state_->monitor = &monitor;
        state_->on_notification = std::move(on_notification);
        state_->on_withdraw = std::move(on_withdraw);
        state_->on_status = std::move(on_status);

        AncsClientState* raw = state_.get();
        state_->sequencer = std::make_unique<ControlPointSequencer>(
            [raw](const std::vector<uint8_t>& payload) { return raw->write_control_point(payload); },
            [raw](const Request& request, const Response& response) {
                raw->handle_response(request, response, static_cast<int64_t>(std::time(nullptr)));
            },
            [raw](const Request& request, const std::string& reason) {
                raw->in_progress.erase(request.uid);
                debug::log(WARN, "ancs: request failed: {}", reason);
            });
    }

    AncsClient::~AncsClient() {
        if (state_)
            state_->unsubscribe();
    }

    void AncsClient::set_device(const std::string& device_path) {
        if (state_->device_path == device_path)
            return;

        // The LE bearer drops and returns constantly, which shows up here as the
        // path alternating with "". That is one session ending, not a different
        // phone: wiping the registry for it would empty the notification list on
        // every blip, and iOS does not always replay its backlog to refill it.
        const bool same_device =
            device_path.empty() || state_->last_known_device.empty() || device_path == state_->last_known_device;
        if (!device_path.empty())
            state_->last_known_device = device_path;

        state_->unsubscribe();
        state_->device_path = device_path;
        state_->subscribed = false;
        state_->next_discover = 0;
        state_->next_subscribe = 0;
        state_->next_probe = 0;
        state_->next_verify = 0;
        state_->sequencer->reset();
        state_->in_progress.clear();
        {
            std::lock_guard<std::mutex> lock(state_->registry_mutex);
            if (same_device) {
                state_->registry.begin_session();
            } else {
                state_->app_names.clear();
                state_->registry.clear();
            }
        }
        {
            std::lock_guard<std::mutex> lock(state_->inbox);
            state_->notification_source_path.clear();
            state_->control_point_path.clear();
            state_->data_source_path.clear();
            state_->pending_source.clear();
            state_->pending_data.clear();
            state_->pending_actions.clear();
        }

        if (device_path.empty())
            state_->set_status(false, "The iPhone is not connected over LE.");
        else
            state_->set_status(false, "Waiting for the iPhone's notification service.");
    }

    void AncsClient::set_content_enabled(bool enabled) { state_->content_enabled = enabled; }

    void AncsClient::drop_session(const std::string& reason) {
        if (state_->subscribed)
            state_->rebuild(reason);
    }

    bool AncsClient::ready() const { return state_->ready; }

    const std::string& AncsClient::status_reason() const { return state_->reason; }

    std::vector<Notification> AncsClient::recent_notifications(size_t limit) const {
        std::lock_guard<std::mutex> lock(state_->registry_mutex);
        return state_->registry.recent(limit);
    }

    bool AncsClient::perform_action(uint32_t uid, ActionId action) {
        {
            std::lock_guard<std::mutex> lock(state_->registry_mutex);
            const Notification* stored = state_->registry.find(uid);
            if (stored && stored->session != state_->registry.session()) {
                state_->registry.forget(uid);
                if (state_->on_withdraw)
                    state_->on_withdraw(uid);
                return true;
            }
        }
        if (!state_->ready)
            return false;
        // The sequencer belongs to tick()'s thread; handing it a request from a
        // command thread would race the queue it pops from. Queue it instead.
        std::lock_guard<std::mutex> lock(state_->inbox);
        if (state_->pending_actions.size() >= MAX_QUEUED_REQUESTS)
            return false;
        state_->pending_actions.push_back(build_action_request(uid, action));
        return true;
    }

    void AncsClient::tick(int64_t now) {
        auto* s = state_.get();
        if (s->device_path.empty())
            return;

        if (s->subscribed && now >= s->next_verify) {
            s->next_verify = now + (s->ready ? ANCS_VERIFY_SECONDS : ANCS_RETRY_SECONDS);
            s->verify_subscription();
        }

        if (s->notification_source_path.empty()) {
            if (now < s->next_discover)
                return;
            s->next_discover = now + ANCS_RETRY_SECONDS;
            if (!s->discover()) {
                s->set_status(false, "Waiting for the iPhone's notification service.");
                return;
            }
        }

        if (!s->subscribed) {
            if (now < s->next_subscribe)
                return;
            s->next_subscribe = now + ANCS_RETRY_SECONDS;
            if (!s->subscribe()) {
                s->set_status(false, "Subscribing to the iPhone's notifications...");
                return;
            }
            s->subscribed = true;
            uint64_t session = 0;
            {
                std::lock_guard<std::mutex> lock(s->registry_mutex);
                s->registry.begin_session();
                session = s->registry.session();
            }
            debug::log(INFO, "ancs: notification session {} started", session);
        }

        // Drain what the other threads buffered.
        std::deque<std::vector<uint8_t>> source, data;
        std::deque<Request> actions;
        {
            std::lock_guard<std::mutex> lock(s->inbox);
            source.swap(s->pending_source);
            data.swap(s->pending_data);
            actions.swap(s->pending_actions);
        }

        for (auto& action : actions)
            s->sequencer->submit(std::move(action));

        for (const auto& packet : data)
            s->sequencer->on_data_source(packet.data(), packet.size(), now);

        for (const auto& packet : source) {
            SourceEvent event;
            if (!parse_source_event(packet.data(), packet.size(), event)) {
                debug::log(WARN, "ancs: discarding a malformed notification packet");
                continue;
            }
            s->handle_source_event(event, now);
        }

        s->sequencer->tick(now);

        // A successful StartNotify proves only that the subscription exists, not
        // that iOS authorized notification access.
        if (!s->ready && now >= s->next_probe) {
            s->next_probe = now + ANCS_RETRY_SECONDS;
            s->set_status(false,
                          "Waiting for the iPhone to allow notification access. Settings > Bluetooth > (i) > "
                          "Share System Notifications.");
            s->sequencer->submit(build_app_request(APP_ID_MESSAGES));
        }
    }

} // namespace tether::bluetooth::ancs
