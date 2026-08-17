#include "tether/bluetooth/ancs/client.hpp"
#include "tether/bluetooth/ancs/sequencer.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/log.hpp"

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
        std::string notification_source_path;
        std::string control_point_path;
        std::string data_source_path;

        bool subscribed = false;
        bool ready = false;
        bool initial_sync = true;
        bool content_enabled = false;
        std::string reason = "Waiting for the iPhone's notification service.";

        int64_t next_discover = 0;
        int64_t next_subscribe = 0;
        int64_t next_probe = 0;

        // Filled on the GLib thread, drained on the caller's thread. Only these
        // two buffers cross the boundary.
        std::mutex inbox;
        std::deque<std::vector<uint8_t>> pending_source;
        std::deque<std::vector<uint8_t>> pending_data;

        guint source_signal = 0;
        guint data_signal = 0;

        NotificationRegistry registry;
        std::unique_ptr<ControlPointSequencer> sequencer;

        // UIDs whose attributes are being fetched, so a notification can be
        // rebuilt when its response lands.
        std::map<uint32_t, SourceEvent> in_progress;

        bool write_control_point(const std::vector<uint8_t>& payload);
        void set_status(bool now_ready, const std::string& text);
        bool discover();
        bool subscribe();
        void unsubscribe();
        void handle_source_event(const SourceEvent& event, int64_t now);
        void handle_response(const Request& request, const Response& response, int64_t now);
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
        if (ready == now_ready && reason == text)
            return;
        ready = now_ready;
        reason = text;
        if (on_status)
            on_status(ready, reason);
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

        notification_source_path.clear();
        control_point_path.clear();
        data_source_path.clear();

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
                    notification_source_path = object_path;
                else if (uuid == UUID_CONTROL_POINT)
                    control_point_path = object_path;
                else if (uuid == UUID_DATA_SOURCE)
                    data_source_path = object_path;
                g_variant_unref(uuid_value);
            }
            g_variant_unref(props);
        }
        g_variant_unref(objects);
        g_variant_unref(reply);

        // All three are needed. A partial tree means BlueZ is still enumerating.
        return !notification_source_path.empty() && !control_point_path.empty() && !data_source_path.empty();
    }

    bool AncsClientState::subscribe() {
        if (!monitor || !monitor->connection())
            return false;

        GDBusConnection* conn = monitor->connection();
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
                // characteristics appear, so this is retried rather than treated
                // as a reason to rediscover.
                const std::string message = error ? error->message : "unknown";
                g_clear_error(&error);
                debug::log(INFO, "ancs: StartNotify not ready yet ({})", message);
                return false;
            }
            g_variant_unref(reply);
            return true;
        };

        if (!start_notify(data_source_path))
            return false;
        if (!start_notify(notification_source_path))
            return false;

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
        if (source_signal) {
            g_dbus_connection_signal_unsubscribe(conn, source_signal);
            source_signal = 0;
        }
        if (data_signal) {
            g_dbus_connection_signal_unsubscribe(conn, data_signal);
            data_signal = 0;
        }
    }

    void AncsClientState::handle_source_event(const SourceEvent& event, int64_t now) {
        (void)now;
        switch (registry.classify(event, initial_sync)) {
        case Decision::Ignore:
            registry.remember(event);
            return;
        case Decision::Withdraw:
            registry.forget(event.uid);
            if (on_withdraw)
                on_withdraw(event.uid);
            return;
        case Decision::Fetch:
            break;
        }

        std::vector<NotificationAttributeId> attributes{NotificationAttributeId::AppIdentifier};
        if (content_enabled) {
            attributes.push_back(NotificationAttributeId::Title);
            attributes.push_back(NotificationAttributeId::Subtitle);
            attributes.push_back(NotificationAttributeId::Message);
        }

        in_progress[event.uid] = event;
        registry.remember(event);
        if (!sequencer->submit(build_notification_request(event.uid, attributes)))
            in_progress.erase(event.uid);
    }

    void AncsClientState::handle_response(const Request& request, const Response& response, int64_t now) {
        if (response.command == CommandId::GetAppAttributes) {
            // The readiness probe: a Data Source answer proves the phone has
            // authorized notification access, which StartNotify alone does not.
            if (!ready)
                set_status(true, "Notification mirroring is active.");
            return;
        }
        if (response.command != CommandId::GetNotificationAttributes)
            return;

        auto found = in_progress.find(request.uid);
        if (found == in_progress.end())
            return;
        const SourceEvent event = found->second;
        in_progress.erase(found);

        Notification notification;
        notification.uid = request.uid;
        notification.app_id = response.attribute(static_cast<uint8_t>(NotificationAttributeId::AppIdentifier));
        notification.app_name = notification.app_id;
        notification.title = response.attribute(static_cast<uint8_t>(NotificationAttributeId::Title));
        notification.subtitle = response.attribute(static_cast<uint8_t>(NotificationAttributeId::Subtitle));
        notification.body = response.attribute(static_cast<uint8_t>(NotificationAttributeId::Message));
        notification.category = event.category;
        notification.silent = event.silent();
        notification.has_positive_action = event.has_positive_action();
        notification.has_negative_action = event.has_negative_action();
        notification.received = now;

        registry.store(notification);
        if (on_notification)
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

        state_->unsubscribe();
        state_->device_path = device_path;
        state_->subscribed = false;
        state_->notification_source_path.clear();
        state_->control_point_path.clear();
        state_->data_source_path.clear();
        state_->next_discover = 0;
        state_->next_subscribe = 0;
        state_->next_probe = 0;
        state_->sequencer->reset();
        state_->in_progress.clear();
        // A new LE session replays the phone's backlog, so the next batch of
        // pre-existing notifications is a sync rather than news.
        state_->initial_sync = true;
        state_->registry.clear();
        {
            std::lock_guard<std::mutex> lock(state_->inbox);
            state_->pending_source.clear();
            state_->pending_data.clear();
        }

        if (device_path.empty())
            state_->set_status(false, "The iPhone is not connected over LE.");
        else
            state_->set_status(false, "Waiting for the iPhone's notification service.");
    }

    void AncsClient::set_content_enabled(bool enabled) { state_->content_enabled = enabled; }

    bool AncsClient::ready() const { return state_->ready; }

    const std::string& AncsClient::status_reason() const { return state_->reason; }

    const NotificationRegistry& AncsClient::registry() const { return state_->registry; }

    bool AncsClient::perform_action(uint32_t uid, ActionId action) {
        if (!state_->ready)
            return false;
        return state_->sequencer->submit(build_action_request(uid, action));
    }

    void AncsClient::tick(int64_t now) {
        auto* s = state_.get();
        if (s->device_path.empty())
            return;

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
        }

        // Drain what the GLib thread buffered.
        std::deque<std::vector<uint8_t>> source, data;
        {
            std::lock_guard<std::mutex> lock(s->inbox);
            source.swap(s->pending_source);
            data.swap(s->pending_data);
        }

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

        // The phone's backlog arrives immediately after subscribing, so once a
        // batch has been processed anything later is genuinely new.
        if (!source.empty())
            s->initial_sync = false;

        s->sequencer->tick(now);

        // A successful StartNotify proves only that the subscription exists, not
        // that iOS authorized notification access.
        if (!s->ready && now >= s->next_probe) {
            s->next_probe = now + ANCS_RETRY_SECONDS;
            s->set_status(false, "Waiting for notification access to be allowed on the iPhone.");
            s->sequencer->submit(build_app_request(APP_ID_MESSAGES));
        }
    }

} // namespace tether::bluetooth::ancs
