#include "tether/bluetooth/pairing.hpp"
#include "tether/bluetooth/advert.hpp"
#include "tether/bluetooth/agent.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/log.hpp"

#include <chrono>
#include <filesystem>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace tether::bluetooth {

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* IFACE_DEVICE = "org.bluez.Device1";
        constexpr const char* IFACE_ADAPTER = "org.bluez.Adapter1";
        constexpr const char* IFACE_PROPS = "org.freedesktop.DBus.Properties";

        // iphone shows its confirmation prompt and waits for a slow human
        constexpr int PAIR_TIMEOUT_SECONDS = 90;
        constexpr int DIALOG_TIMEOUT_SECONDS = 60;

        constexpr int CLASSIC_SETTLE_SECONDS = 3;
        constexpr int DISCOVERY_TIMEOUT_SECONDS = 30;

        std::string normalize_address(std::string address) {
            for (char& c : address)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return address;
        }

        void notify(const ProgressFn& progress, const std::string& step, const std::string& detail) {
            debug::log(INFO, "bluetooth: {} {}", step, detail);
            if (progress)
                progress(step, detail);
        }

        bool set_property(GDBusConnection* conn,
                          const std::string& path,
                          const char* iface,
                          const char* name,
                          GVariant* value,
                          std::string* err_out = nullptr) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(conn,
                                                          BLUEZ_NAME,
                                                          path.c_str(),
                                                          IFACE_PROPS,
                                                          "Set",
                                                          g_variant_new("(ssv)", iface, name, value),
                                                          nullptr,
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          5000,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                if (err_out)
                    *err_out = error ? error->message : "unknown";
                g_clear_error(&error);
                return false;
            }
            g_variant_unref(reply);
            return true;
        }

        bool call_device(
            GDBusConnection* conn, const std::string& path, const char* method, int timeout_ms, std::string& err_out) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(conn,
                                                          BLUEZ_NAME,
                                                          path.c_str(),
                                                          IFACE_DEVICE,
                                                          method,
                                                          nullptr,
                                                          nullptr,
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          timeout_ms,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                err_out = error ? error->message : "unknown error";
                g_clear_error(&error);
                return false;
            }
            g_variant_unref(reply);
            return true;
        }

        const Device* find_by_address(const BluezObjects& objects, const std::string& address) {
            for (const auto& d : objects.devices) {
                if (normalize_address(d.address) == address)
                    return &d;
            }
            return nullptr;
        }

        // Copies out the device record rather than holding a pointer into a
        // snapshot that the watcher thread replaces underneath us.
        bool lookup(BluezMonitor& monitor, const std::string& address, Device& out) {
            auto objects = monitor.snapshot();
            if (const Device* d = find_by_address(objects, address)) {
                out = *d;
                return true;
            }
            return false;
        }

        bool call_adapter(GDBusConnection* conn, const std::string& path, const char* method) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(conn,
                                                          BLUEZ_NAME,
                                                          path.c_str(),
                                                          IFACE_ADAPTER,
                                                          method,
                                                          nullptr,
                                                          nullptr,
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          5000,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                debug::log(WARN, "bluetooth: {} failed: {}", method, error ? error->message : "unknown");
                g_clear_error(&error);
                return false;
            }
            g_variant_unref(reply);
            return true;
        }

        // Scans until the device shows up. Needed because an unpair makes BlueZ
        // forget the device entirely, which would otherwise leave no way to pair
        // again from inside the app.
        bool discover(BluezMonitor& monitor, GDBusConnection* conn, const std::string& address, Device& out) {
            auto objects = monitor.snapshot();
            if (objects.adapters.empty())
                return false;
            const std::string adapter = objects.adapters.front().path;

            if (!call_adapter(conn, adapter, "StartDiscovery"))
                return false;

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(DISCOVERY_TIMEOUT_SECONDS);
            bool found = false;
            while (std::chrono::steady_clock::now() < deadline) {
                if (lookup(monitor, address, out)) {
                    found = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            // discovery must stop before pairing
            call_adapter(conn, adapter, "StopDiscovery");
            return found;
        }

        // Connect-first makes the iPhone the authentication initiator, so BlueZ
        // must accept an inbound pairing request for the transaction to complete.
        // A desktop adapter normally sits with Pairable off, in which case iOS
        // never shows its half of the numeric comparison and the link fails with
        // br-connection-key-missing.
        //
        // Restores the previous values on every exit path, including the early
        // returns and the timeout.
        class PairableWindow {
        public:
            PairableWindow(GDBusConnection* conn, std::string adapter) : conn_(conn), adapter_(std::move(adapter)) {
                if (adapter_.empty())
                    return;
                was_pairable_ = get_bool(IFACE_ADAPTER, "Pairable");
                was_discoverable_ = get_bool(IFACE_ADAPTER, "Discoverable");
                set_bool("Pairable", true);
                set_bool("Discoverable", true);
            }

            ~PairableWindow() {
                if (adapter_.empty())
                    return;
                set_bool("Pairable", was_pairable_);
                set_bool("Discoverable", was_discoverable_);
            }

            PairableWindow(const PairableWindow&) = delete;
            PairableWindow& operator=(const PairableWindow&) = delete;

        private:
            bool get_bool(const char* iface, const char* name) const {
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(conn_,
                                                              BLUEZ_NAME,
                                                              adapter_.c_str(),
                                                              IFACE_PROPS,
                                                              "Get",
                                                              g_variant_new("(ss)", iface, name),
                                                              G_VARIANT_TYPE("(v)"),
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              5000,
                                                              nullptr,
                                                              &error);
                if (!reply) {
                    g_clear_error(&error);
                    return false;
                }
                GVariant* boxed = nullptr;
                g_variant_get(reply, "(v)", &boxed);
                const bool value = boxed && g_variant_get_boolean(boxed);
                if (boxed)
                    g_variant_unref(boxed);
                g_variant_unref(reply);
                return value;
            }

            void set_bool(const char* name, bool value) const {
                set_property(conn_, adapter_, IFACE_ADAPTER, name, g_variant_new_boolean(value));
            }

            GDBusConnection* conn_;
            std::string adapter_;
            bool was_pairable_ = false;
            bool was_discoverable_ = false;
        };

        std::string adapter_for(BluezMonitor& monitor, const Device& device) {
            if (!device.adapter_path.empty())
                return device.adapter_path;
            auto objects = monitor.snapshot();
            return objects.adapters.empty() ? std::string{} : objects.adapters.front().path;
        }

        // The solicitation advertise outlives the transaction that started it, so it
        // is owned here rather than on a stack frame.
        AncsAdvertisement* g_advert = nullptr;

        // An advert soliciting ANCS must never be on air while Classic pairing is in flight
        void stop_advert(BluezMonitor& monitor) {
            if (!g_advert)
                return;
            g_advert->unregister_with_bluez();
            monitor.invoke_sync([&] { g_advert->unexport_object(); });
            delete g_advert;
            g_advert = nullptr;
        }

    } // namespace

    bool confirm_with_dialog(const std::string& device_name, const std::string& code) {
        std::string body =
            device_name + " wants to pair.\n\nConfirm this code matches the one on your iPhone:\n\n" + code;

        pid_t pid = fork();
        if (pid < 0) {
            debug::log(ERR, "bluetooth: fork() for confirmation dialog failed");
            return false;
        }

        if (pid == 0) {
            std::filesystem::path self_path;
            try {
                self_path = std::filesystem::read_symlink("/proc/self/exe");
            } catch (...) {
            }
            std::string sibling = (self_path.parent_path() / "tether-dialog").string();
            std::string timeout = std::to_string(DIALOG_TIMEOUT_SECONDS);

            execl(sibling.c_str(),
                  "tether-dialog",
                  "--title",
                  "Bluetooth Pairing",
                  "--body",
                  body.c_str(),
                  "--accept",
                  "Confirm",
                  "--reject",
                  "Cancel",
                  "--timeout",
                  timeout.c_str(),
                  nullptr);
            execlp("tether-dialog",
                   "tether-dialog",
                   "--title",
                   "Bluetooth Pairing",
                   "--body",
                   body.c_str(),
                   "--accept",
                   "Confirm",
                   "--reject",
                   "Cancel",
                   "--timeout",
                   timeout.c_str(),
                   nullptr);
            _exit(3);
        }

        int status = 0;
        if (waitpid(pid, &status, 0) < 0)
            return false;
        // Exit 0 is the only acceptance. A dialog that could not be shown exits 3,
        // which must reject rather than silently bond.
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    nlohmann::json to_json(const PairResult& result) {
        return {
            {"command", "bt_pair_result"},
            {"success", result.success},
            {"status", result.status},
            {"message", result.message},
            {"address", result.device_address},
            {"dual_bond", result.dual_bond},
        };
    }

    PairResult pair_device(BluezMonitor& monitor,
                           const std::string& address,
                           AuthStrategy strategy,
                           const ProgressFn& progress,
                           const ConfirmFn& confirm) {
        PairResult result;
        result.device_address = normalize_address(address);

        GDBusConnection* conn = monitor.connection();
        if (!conn) {
            result.status = "error";
            result.message = "Bluetooth is unavailable.";
            return result;
        }

        PairableWindow pairable(conn, [&] {
            auto objects = monitor.snapshot();
            return objects.adapters.empty() ? std::string{} : objects.adapters.front().path;
        }());

        Device device;
        if (!lookup(monitor, result.device_address, device)) {
            notify(progress, "discovering", result.device_address);
            if (!discover(monitor, conn, result.device_address, device)) {
                result.status = "not_found";
                result.message = "Device " + result.device_address +
                                 " is not visible to BlueZ. Unlock the iPhone and open its Bluetooth settings so it "
                                 "is discoverable, then try again.";
                return result;
            }
        }
        result.device_path = device.path;

        const std::string adapter_path = adapter_for(monitor, device);
        const std::string display_name = device.name.empty() ? result.device_address : device.name;

        if (device.paired) {
            notify(progress, "already_paired", display_name);
            result.success = true;
            result.status = "already_paired";
            result.dual_bond = device.has_le_bearer && device.le_bonded;
            result.message = display_name + " is already paired.";
        } else {
            auto cap = monitor.capability();
            if (!cap.class_ok) {
                notify(progress,
                       "warning",
                       "Adapter class is not A/V Hands-Free; the iPhone may not offer its permissions. "
                       "Run scripts/bt-probe.sh --set-class.");
            }

            // Silence any advert left over from an earlier pairing before
            // authentication starts.
            stop_advert(monitor);

            PairingAgent agent(conn, device.path, [&](const std::string& code) {
                notify(progress, "confirm", code);
                return confirm ? confirm(code) : confirm_with_dialog(display_name, code);
            });

            bool exported = false;
            monitor.invoke_sync([&] { exported = agent.export_object(); });
            if (!exported || !agent.register_with_bluez()) {
                monitor.invoke_sync([&] { agent.unexport_object(); });
                result.status = "error";
                result.message = "Could not register a Bluetooth pairing agent.";
                return result;
            }

            std::string err;
            bool initiated = false;
            if (strategy == AuthStrategy::ConnectFirst) {
                notify(progress, "connecting", display_name);
                initiated = call_device(conn, device.path, "Connect", PAIR_TIMEOUT_SECONDS * 1000, err);
            } else {
                notify(progress, "pairing", display_name);
                initiated = call_device(conn, device.path, "Pair", PAIR_TIMEOUT_SECONDS * 1000, err);
            }

            // Connect() can report failure while authentication still completes
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(PAIR_TIMEOUT_SECONDS);
            bool paired = false;
            while (std::chrono::steady_clock::now() < deadline) {
                Device current;
                if (lookup(monitor, result.device_address, current) && current.paired) {
                    paired = true;
                    device = current;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            agent.unregister_with_bluez();
            monitor.invoke_sync([&] { agent.unexport_object(); });

            if (!paired) {
                result.status = err.find("Rejected") != std::string::npos ? "rejected" : "timeout";
                result.message = initiated ? "Pairing did not complete. Confirm the prompt on the iPhone."
                                           : ("Pairing failed: " + err);
                return result;
            }

            notify(progress, "paired", display_name);
            result.success = true;
            result.status = "paired";
            result.message = "Paired with " + display_name + ".";
        }

        // Trusting the bond lets BlueZ reconnect without asking again.
        std::string trust_err;
        if (!set_property(conn, device.path, IFACE_DEVICE, "Trusted", g_variant_new_boolean(TRUE), &trust_err))
            debug::log(WARN, "bluetooth: could not trust device: {}", trust_err);

        // Prefer BR/EDR for the next outbound connection, then let the Classic ACL
        // settle before anything touches LE. Older BlueZ has no such property.
        set_property(conn, device.path, IFACE_DEVICE, "PreferredBearer", g_variant_new_string("bredr"));
        notify(progress, "settling", "waiting for the Classic link to settle");
        std::this_thread::sleep_for(std::chrono::seconds(CLASSIC_SETTLE_SECONDS));

        Device settled;
        if (lookup(monitor, result.device_address, settled))
            result.dual_bond = settled.has_le_bearer && settled.le_bonded;

        // Only now solicit ANCS. This advert is what makes iOS reveal its
        // "Show Message Notifications" and "Sync Contacts" toggles, and it is safe
        // to broadcast because the bond already exists.
        if (!adapter_path.empty() && monitor.capability().advertising) {
            stop_advert(monitor);
            auto* advert = new AncsAdvertisement(conn, adapter_path);

            bool exported = false;
            monitor.invoke_sync([&] { exported = advert->export_object(); });
            // BlueZ reads the advertisement's properties back before returning, so
            // this call must stay off the thread that answers those reads.
            if (exported && advert->register_with_bluez()) {
                g_advert = advert;
                notify(progress,
                       "soliciting",
                       "Open Settings > Bluetooth > (i) on the iPhone and enable Show Message Notifications and "
                       "Sync Contacts. They can take a few minutes to appear.");
            } else {
                monitor.invoke_sync([&] { advert->unexport_object(); });
                delete advert;
                notify(progress, "warning", "Could not advertise for ANCS; notification permissions may not appear.");
            }
        }

        if (!result.dual_bond) {
            result.message += " The bond covers BR/EDR only, so messages and contacts will work but notification "
                              "mirroring will not.";
        }
        return result;
    }

    PairResult unpair_device(BluezMonitor& monitor, const std::string& address) {
        PairResult result;
        result.device_address = normalize_address(address);

        GDBusConnection* conn = monitor.connection();
        if (!conn) {
            result.status = "error";
            result.message = "Bluetooth is unavailable.";
            return result;
        }

        Device device;
        if (!lookup(monitor, result.device_address, device)) {
            result.status = "not_found";
            result.message = "Device " + result.device_address + " is not known to BlueZ.";
            return result;
        }

        const std::string adapter_path = adapter_for(monitor, device);
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(conn,
                                                      BLUEZ_NAME,
                                                      adapter_path.c_str(),
                                                      IFACE_ADAPTER,
                                                      "RemoveDevice",
                                                      g_variant_new("(o)", device.path.c_str()),
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      10000,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            result.status = "error";
            result.message = std::string("Could not remove the bond: ") + (error ? error->message : "unknown");
            g_clear_error(&error);
            return result;
        }
        g_variant_unref(reply);

        result.success = true;
        result.status = "unpaired";
        result.message = "Removed the bond. Also delete this computer from the iPhone's Bluetooth settings before "
                         "pairing again — a stale record there will block a clean retry.";
        return result;
    }

} // namespace tether::bluetooth
