#include "notification.hpp"

#include <tether/app_icons.hpp>
#include <tether/i18n.hpp>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib.h>
#include <libnotify/notify.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <tether/bluetooth/ancs/notifications.hpp>
#include <tether/log.hpp>
#include <tether/net.hpp>
#include <tether/service.hpp>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace tether {

    namespace {

        // flatpak renames the desktop file after the app id
        const std::string& desktop_entry() {
            static const std::string id = [] {
                const std::string app_id = flatpak_app_id();
                return app_id.empty() ? std::string("tether-gtk") : app_id;
            }();
            return id;
        }

        struct NotificationRequest {
            std::filesystem::path path;
        };

        // Cuts a downloaded icon to the iPhone icon shape; empty keeps the original.
        std::string shape_app_icon(const std::string& png) {
            GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
            if (!loader)
                return {};
            const bool written =
                gdk_pixbuf_loader_write(loader, reinterpret_cast<const guchar*>(png.data()), png.size(), nullptr);
            const bool closed = gdk_pixbuf_loader_close(loader, nullptr);

            std::string out;
            GdkPixbuf* decoded = written && closed ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
            if (decoded && gdk_pixbuf_get_bits_per_sample(decoded) == 8) {
                // Always yields an 8-bit RGBA copy, with or without source alpha.
                if (GdkPixbuf* rgba = gdk_pixbuf_add_alpha(decoded, FALSE, 0, 0, 0)) {
                    tether::app_icons::apply_icon_shape(gdk_pixbuf_get_pixels(rgba),
                                                        gdk_pixbuf_get_width(rgba),
                                                        gdk_pixbuf_get_height(rgba),
                                                        gdk_pixbuf_get_rowstride(rgba));
                    gchar* buffer = nullptr;
                    gsize size = 0;
                    if (gdk_pixbuf_save_to_buffer(rgba, &buffer, &size, "png", nullptr, nullptr)) {
                        out.assign(buffer, size);
                        g_free(buffer);
                    }
                    g_object_unref(rgba);
                }
            }
            g_object_unref(loader);
            return out;
        }

        // Every icon name installed by any theme on this machine.
        // scanned once and cached for the life of the process.
        const std::unordered_set<std::string>& installed_icons() {
            static const std::unordered_set<std::string> names = [] {
                std::unordered_set<std::string> found;
                std::vector<std::filesystem::path> roots;

                const char* home = std::getenv("HOME");
                if (const char* data_home = std::getenv("XDG_DATA_HOME"); data_home && *data_home)
                    roots.emplace_back(std::filesystem::path(data_home) / "icons");
                else if (home)
                    roots.emplace_back(std::filesystem::path(home) / ".local/share/icons");
                if (home)
                    roots.emplace_back(std::filesystem::path(home) / ".icons");

                const char* data_dirs = std::getenv("XDG_DATA_DIRS");
                std::string dirs = data_dirs && *data_dirs ? data_dirs : "/usr/local/share:/usr/share";
                for (size_t start = 0; start <= dirs.size();) {
                    const size_t end = dirs.find(':', start);
                    const std::string dir = dirs.substr(start, end - start);
                    if (!dir.empty())
                        roots.emplace_back(std::filesystem::path(dir) / "icons");
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
                roots.emplace_back("/usr/share/pixmaps");

                // follow symlinks (nix link whole themes) but check for recursion,
                // different themes link to each other or to itself
                std::unordered_set<std::string> visited;
                std::error_code ec;
                for (const auto& root : roots) {
                    const auto real_root = std::filesystem::canonical(root, ec);
                    if (ec || !visited.insert(real_root.string()).second) {
                        ec.clear();
                        continue;
                    }
                    auto it = std::filesystem::recursive_directory_iterator(
                        root,
                        std::filesystem::directory_options::skip_permission_denied |
                            std::filesystem::directory_options::follow_directory_symlink,
                        ec);
                    for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                        const auto& entry = *it;
                        std::error_code entry_ec;
                        if (entry.is_directory(entry_ec)) {
                            const auto real = std::filesystem::canonical(entry.path(), entry_ec);
                            if (entry_ec || !visited.insert(real.string()).second)
                                it.disable_recursion_pending();
                            continue;
                        }
                        const auto ext = entry.path().extension();
                        if (ext == ".png" || ext == ".svg" || ext == ".xpm")
                            found.insert(entry.path().stem().string());
                    }
                    ec.clear();
                }
                debug::log(INFO, "Found {} icon names across the installed themes", found.size());
                return found;
            }();
            return names;
        }

        // The first candidate this machine can actually draw. Handing the server
        // a name no theme has leaves the popup with no icon at all, which is
        // worse than a generic one. Candidates are icon file paths or theme names.
        std::string resolve_icon(const std::vector<std::string>& candidates) {
            const auto& installed = installed_icons();
            for (const auto& name : candidates) {
                if (!name.empty() && name.front() == '/') {
                    std::error_code ec;
                    if (std::filesystem::is_regular_file(name, ec))
                        return name;
                    continue;
                }
                if (installed.count(name))
                    return name;
            }
            return {};
        }

        // Asked per notification, like the capabilities: the server can change while tetherd runs.
        bool server_is_gnome_shell() {
            char* name = nullptr;
            if (!notify_get_server_info(&name, nullptr, nullptr, nullptr))
                return false;
            const bool gnome = g_strcmp0(name, "gnome-shell") == 0;
            g_free(name);
            return gnome;
        }

        // Identifies the sending application to the notification server. Without
        // this KDE shows the popup and then forgets it.
        void set_identity(NotifyNotification* notification,
                          const std::string& app_name,
                          bool from_iphone_app,
                          const std::string& icon) {
            // GNOME titles a popup after its desktop entry, so iPhone apps go without one.
            if (from_iphone_app && !app_name.empty() && server_is_gnome_shell()) {
                notify_notification_set_app_name(notification, app_name.c_str());
#if NOTIFY_CHECK_VERSION(0, 8, 4)
                if (!icon.empty())
                    notify_notification_set_app_icon(notification, icon.c_str());
#else
                (void)icon;
#endif
                return;
            }

            notify_notification_set_hint(notification, "desktop-entry", g_variant_new_string(desktop_entry().c_str()));
            if (!app_name.empty()) {
                // Keeps history and configuration grouped under Tether while the
                // header names the iPhone app the notification came from.
                notify_notification_set_hint(
                    notification, "x-kde-display-appname", g_variant_new_string(app_name.c_str()));
            }
            notify_notification_set_hint(notification, "x-kde-origin-name", g_variant_new_string("iPhone"));
        }

        struct NotificationActionData {
            std::string payload;
            std::string handle;
        };

        // Set once during startup, before any notification exists.
        std::function<void(const std::string&, const std::string&)> g_copy_handler;
        std::function<void(const std::string&, const std::string&)> g_reply_handler;
        std::function<void(const std::string&, const std::string&)> g_mute_handler;
        std::function<void(const std::string&, bool)> g_network_trust_handler;
        // trust prompts by network key, so repeat raises the same popup
        std::unordered_map<std::string, NotifyNotification*> g_trust_popups;

        // Server notification id -> conversation, for notifications showing an
        // inline reply field. Touched only on the notifier's thread.
        std::unordered_map<guint32, std::string> g_reply_threads;

        // iPhone notification UID -> its open popup
        std::unordered_map<uint32_t, NotifyNotification*> g_ancs_popups;

        // Open "File arrived" popup that a burst of transfers folds into.
        constexpr auto FILE_BATCH_WINDOW = std::chrono::seconds(10);
        NotifyNotification* g_file_popup = nullptr;
        int g_file_count = 0;
        std::chrono::steady_clock::time_point g_last_file_arrival;

        guint32 notification_id(NotifyNotification* notification) {
            gint id = 0;
            g_object_get(notification, "id", &id, nullptr);
            return static_cast<guint32>(id);
        }

        // Asked per notification rather than cached: the server can be swapped
        // or restarted while tetherd runs.
        bool server_supports_inline_reply() {
            bool supported = false;
            GList* caps = notify_get_server_caps();
            for (GList* cap = caps; cap; cap = cap->next) {
                if (g_strcmp0(static_cast<const char*>(cap->data), "inline-reply") == 0)
                    supported = true;
            }
            g_list_free_full(caps, g_free);
            return supported;
        }

        // The servers emit this just before closing the notification.
        void on_notification_replied(
            GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* parameters, gpointer) {
            guint32 id = 0;
            const char* text = nullptr;
            g_variant_get(parameters, "(u&s)", &id, &text);

            const auto it = g_reply_threads.find(id);
            if (it == g_reply_threads.end())
                return;
            const std::string thread = it->second;
            g_reply_threads.erase(it);
            if (g_reply_handler && text && *text)
                g_reply_handler(thread, text);
        }

        void free_request(gpointer data) { delete static_cast<NotificationRequest*>(data); }

        void free_action_data(gpointer data) { delete static_cast<NotificationActionData*>(data); }

        bool launch_uri(const std::string& uri) {
            GError* error = nullptr;
            gboolean ok = g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &error);
            if (!ok) {
                debug::log(ERR, "Failed to open URI '{}'", uri);
                if (error) {
                    debug::log(ERR, ": {}", error->message);
                    g_error_free(error);
                }
                debug::log(ERR, "");
                return false;
            }

            return true;
        }

        // Hitting an already-running GUI is GApplication's job. A second launch is routed to the primary instance
        // rather than starting a new window.
        void open_gui_thread(const std::string& thread_key) {
            std::string argument = "--thread=" + thread_key;
            char* argv[] = {const_cast<char*>("tether-gtk"), argument.data(), nullptr};
            GError* error = nullptr;
            if (!g_spawn_async(nullptr,
                               argv,
                               nullptr,
                               static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                                                        G_SPAWN_STDERR_TO_DEV_NULL),
                               nullptr,
                               nullptr,
                               nullptr,
                               &error)) {
                debug::log(ERR, "Failed to open tether-gtk: {}", error ? error->message : "unknown");
                g_clear_error(&error);
            }
        }

        void on_copy_code_action(NotifyNotification*, char*, gpointer user_data) {
            auto* action = static_cast<NotificationActionData*>(user_data);
            if (action && g_copy_handler)
                g_copy_handler(action->payload, action->handle);
        }

        void on_reply_action(NotifyNotification*, char*, gpointer user_data) {
            if (auto* action = static_cast<NotificationActionData*>(user_data))
                open_gui_thread(action->payload);
        }

        // Whether this process can see the host's installed applications. A flatpak sandbox cannot.
        bool sandboxed() {
            static const bool yes = std::filesystem::exists("/.flatpak-info");
            return yes;
        }

        // How to open the Linux counterpart of an iPhone app.
        struct AppLauncher {
            // "scheme://" URI, a "<id>.desktop" entry, or empty when
            std::string payload;
            std::string name;
        };

        AppLauncher resolve_launcher(const std::string& app_id) {
            const auto target = bluetooth::ancs::launch_target(app_id);

            if (!target.uri_scheme.empty()) {
                if (GAppInfo* info = g_app_info_get_default_for_uri_scheme(target.uri_scheme.c_str())) {
                    // Launch the handler itself rather than a bare "scheme://" the app
                    // would have to make sense of.
                    const char* id = g_app_info_get_id(info);
                    const char* name = g_app_info_get_display_name(info);
                    AppLauncher launcher{id ? id : target.uri_scheme + "://", name ? name : ""};
                    g_object_unref(info);
                    return launcher;
                }
                if (sandboxed())
                    return {target.uri_scheme + "://", ""};
            }

            for (const auto& id : target.desktop_ids) {
                const std::string entry = id + ".desktop";
                if (GDesktopAppInfo* info = g_desktop_app_info_new(entry.c_str())) {
                    const char* name = g_app_info_get_display_name(G_APP_INFO(info));
                    AppLauncher launcher{entry, name ? name : ""};
                    g_object_unref(info);
                    return launcher;
                }
            }
            return {};
        }

        void on_mute_action(NotifyNotification*, char*, gpointer user_data) {
            auto* action = static_cast<NotificationActionData*>(user_data);
            if (action && g_mute_handler)
                g_mute_handler(action->payload, action->handle);
        }

        void on_open_app_action(NotifyNotification*, char*, gpointer user_data) {
            auto* action = static_cast<NotificationActionData*>(user_data);
            if (!action)
                return;

            if (!action->payload.ends_with(".desktop")) {
                launch_uri(action->payload);
                return;
            }

            GDesktopAppInfo* info = g_desktop_app_info_new(action->payload.c_str());
            if (!info) {
                debug::log(ERR, "No desktop entry '{}' to open", action->payload);
                return;
            }
            GError* error = nullptr;
            if (!g_app_info_launch(G_APP_INFO(info), nullptr, nullptr, &error)) {
                debug::log(ERR, "Failed to launch {}: {}", action->payload, error ? error->message : "unknown");
                g_clear_error(&error);
            }
            g_object_unref(info);
        }

        void on_notification_action(NotifyNotification*, char*, gpointer user_data) {
            auto* action = static_cast<NotificationActionData*>(user_data);
            if (!action) {
                return;
            }

            launch_uri(action->payload);
        }

        void on_notification_closed(NotifyNotification* notification, gpointer) {
            g_reply_threads.erase(notification_id(notification));
            std::erase_if(g_ancs_popups, [notification](const auto& entry) { return entry.second == notification; });
            std::erase_if(g_trust_popups, [notification](const auto& entry) { return entry.second == notification; });
            if (notification == g_file_popup)
                g_file_popup = nullptr;
            g_object_unref(notification);
        }

        // payload is the network key; handle is "trust" or "decline".
        void on_network_trust_action(NotifyNotification*, char*, gpointer user_data) {
            auto* action = static_cast<NotificationActionData*>(user_data);
            if (action && g_network_trust_handler)
                g_network_trust_handler(action->payload, action->handle == "trust");
        }

        struct NetworkTrustRequest {
            std::string key;
            std::string name;
        };

        gboolean show_network_trust_on_main(gpointer user_data) {
            std::unique_ptr<NetworkTrustRequest> request(static_cast<NetworkTrustRequest*>(user_data));
            if (const auto open = g_trust_popups.find(request->key); open != g_trust_popups.end()) {
                notify_notification_show(open->second, nullptr);
                return G_SOURCE_REMOVE;
            }
            const std::string body = tr_format(
                _("A paired device is on {}. Tether stays hidden on networks you have not trusted."), request->name);
            NotifyNotification* notification =
                notify_notification_new(_("Show Tether on this network?"), body.c_str(), "network-wireless");
            set_identity(notification, "", false, "");
            notify_notification_set_hint(notification, "resident", g_variant_new_boolean(TRUE));
            g_signal_connect(notification, "closed", G_CALLBACK(on_notification_closed), nullptr);
            notify_notification_add_action(notification,
                                           "trust",
                                           _("Trust"),
                                           on_network_trust_action,
                                           new NotificationActionData{request->key, "trust"},
                                           free_action_data);
            notify_notification_add_action(notification,
                                           "decline",
                                           _("Not now"),
                                           on_network_trust_action,
                                           new NotificationActionData{request->key, "decline"},
                                           free_action_data);
            if (!notify_notification_show(notification, nullptr)) {
                debug::log(ERR, "Failed to show network trust notification");
                g_object_unref(notification);
            } else {
                g_trust_popups[request->key] = notification;
            }
            return G_SOURCE_REMOVE;
        }

        gboolean withdraw_on_main(gpointer user_data) {
            const auto uid = static_cast<uint32_t>(GPOINTER_TO_UINT(user_data));
            const auto it = g_ancs_popups.find(uid);
            if (it == g_ancs_popups.end())
                return G_SOURCE_REMOVE;
            // The "closed" signal that follows drops the entry and the reference.
            notify_notification_close(it->second, nullptr);
            return G_SOURCE_REMOVE;
        }

        gboolean show_notification_on_main(gpointer user_data) {
            std::unique_ptr<NotificationRequest> request(static_cast<NotificationRequest*>(user_data));

            auto file = std::filesystem::absolute(request->path);
            auto file_string = file.string();
            auto folder_string = file.parent_path().string();
            auto file_uri = g_filename_to_uri(file_string.c_str(), nullptr, nullptr);
            auto folder_uri = g_filename_to_uri(folder_string.c_str(), nullptr, nullptr);

            if (!file_uri || !folder_uri) {
                if (file_uri)
                    g_free(file_uri);
                if (folder_uri)
                    g_free(folder_uri);
                debug::log(ERR, "Failed to create notification URI for {}", file);
                return G_SOURCE_REMOVE;
            }

            const auto now = std::chrono::steady_clock::now();
            if (g_file_popup && now - g_last_file_arrival < FILE_BATCH_WINDOW) {
                g_last_file_arrival = now;
                ++g_file_count;
                const std::string body =
                    tr_format(P_("{} file arrived", "{} files arrived", g_file_count), g_file_count);
                notify_notification_update(g_file_popup, _("Files arrived"), body.c_str(), "document-save");
                notify_notification_clear_actions(g_file_popup);
                notify_notification_add_action(g_file_popup,
                                               "open-folder",
                                               _("Open Folder"),
                                               on_notification_action,
                                               new NotificationActionData{folder_uri},
                                               free_action_data);
                if (!notify_notification_show(g_file_popup, nullptr))
                    debug::log(ERR, "Failed to update file notification");
                g_free(file_uri);
                g_free(folder_uri);
                return G_SOURCE_REMOVE;
            }

            NotifyNotification* notification =
                notify_notification_new(_("File arrived"), file.filename().c_str(), "document-save");

            set_identity(notification, "", false, "");
            notify_notification_set_hint(notification, "resident", g_variant_new_boolean(TRUE));
            notify_notification_set_timeout(notification, NOTIFY_EXPIRES_DEFAULT);

            auto* open_file = new NotificationActionData{file_uri};
            auto* open_folder = new NotificationActionData{folder_uri};

            g_signal_connect(notification, "closed", G_CALLBACK(on_notification_closed), nullptr);
            notify_notification_add_action(
                notification, "open-file", _("Open File"), on_notification_action, open_file, free_action_data);
            notify_notification_add_action(
                notification, "open-folder", _("Open Folder"), on_notification_action, open_folder, free_action_data);

            GError* error = nullptr;
            gboolean shown = notify_notification_show(notification, &error);
            if (!shown) {
                debug::log(ERR, "Failed to show notification");
                if (error) {
                    debug::log(ERR, ": {}", error->message);
                    g_error_free(error);
                }
                debug::log(ERR, "");
                g_object_unref(notification);
            } else {
                g_file_popup = notification;
                g_file_count = 1;
                g_last_file_arrival = now;
            }
            g_free(file_uri);
            g_free(folder_uri);
            return G_SOURCE_REMOVE;
        }

        gboolean show_spec_on_main(gpointer user_data) {
            std::unique_ptr<NotificationSpec> spec(static_cast<NotificationSpec*>(user_data));

            const std::string icon = resolve_icon(spec->icons);
            NotifyNotification* notification =
                notify_notification_new(spec->summary.c_str(),
                                        spec->body.empty() ? nullptr : spec->body.c_str(),
                                        icon.empty() ? nullptr : icon.c_str());
            if (!notification)
                return G_SOURCE_REMOVE;

            set_identity(notification, spec->app_name, !spec->app_id.empty(), icon);
            notify_notification_set_urgency(notification, spec->quiet ? NOTIFY_URGENCY_LOW : NOTIFY_URGENCY_NORMAL);

            const AppLauncher launcher = spec->app_id.empty() ? AppLauncher{} : resolve_launcher(spec->app_id);
            const bool has_actions = !spec->reply_thread.empty() || !spec->otp_code.empty() ||
                                     !launcher.payload.empty() || spec->ancs_uid >= 0;
            if (has_actions)
                g_signal_connect(notification, "closed", G_CALLBACK(on_notification_closed), nullptr);

            const bool inline_reply = !spec->reply_thread.empty() && g_reply_handler && server_supports_inline_reply();

            if (!spec->reply_thread.empty()) {
                notify_notification_add_action(notification,
                                               "default",
                                               "default",
                                               on_reply_action,
                                               new NotificationActionData{spec->reply_thread},
                                               free_action_data);
                if (inline_reply) {
                    // The server answers with NotificationReplied rather than
                    // ActionInvoked, so this callback only runs on servers that
                    // treat "inline-reply" as a plain button.
                    notify_notification_add_action(notification,
                                                   "inline-reply",
                                                   _("Reply"),
                                                   on_reply_action,
                                                   new NotificationActionData{spec->reply_thread},
                                                   free_action_data);
                    notify_notification_set_hint(
                        notification, "x-kde-reply-placeholder-text", g_variant_new_string(_("Reply…")));
                } else {
                    notify_notification_add_action(notification,
                                                   "reply",
                                                   _("Reply"),
                                                   on_reply_action,
                                                   new NotificationActionData{spec->reply_thread},
                                                   free_action_data);
                }
            }

            if (!spec->otp_code.empty()) {
                notify_notification_add_action(notification,
                                               "copy-code",
                                               _("Copy Code"),
                                               on_copy_code_action,
                                               new NotificationActionData{spec->otp_code, spec->read_handle},
                                               free_action_data);
            }

            if (!launcher.payload.empty()) {
                const std::string label =
                    tr_format(_("Open in {}"), launcher.name.empty() ? spec->app_name : launcher.name);
                notify_notification_add_action(notification,
                                               "open-app",
                                               label.c_str(),
                                               on_open_app_action,
                                               new NotificationActionData{launcher.payload},
                                               free_action_data);
            }

            if (spec->ancs_uid >= 0 && !spec->app_id.empty() && g_mute_handler) {
                const std::string label = tr_format(_("Mute {}"), spec->app_name);
                notify_notification_add_action(notification,
                                               "mute-app",
                                               label.c_str(),
                                               on_mute_action,
                                               new NotificationActionData{spec->app_id, spec->app_name},
                                               free_action_data);
            }

            GError* error = nullptr;
            if (!notify_notification_show(notification, &error)) {
                debug::log(ERR, "Failed to show notification: {}", error ? error->message : "unknown");
                g_clear_error(&error);
                g_object_unref(notification);
                return G_SOURCE_REMOVE;
            }
            if (inline_reply)
                g_reply_threads[notification_id(notification)] = spec->reply_thread;
            if (spec->ancs_uid >= 0)
                g_ancs_popups[static_cast<uint32_t>(spec->ancs_uid)] = notification;
            if (!has_actions)
                g_object_unref(notification);
            return G_SOURCE_REMOVE;
        }

    } // namespace

    struct DesktopNotifier::Impl {
        GMainContext* context = nullptr;
        GMainLoop* loop = nullptr;
        std::thread thread;
        bool initialized = false;
        GDBusConnection* bus = nullptr;
        guint reply_subscription = 0;
    };

    DesktopNotifier::DesktopNotifier() : impl_(std::make_unique<Impl>()) {}

    DesktopNotifier::~DesktopNotifier() {
        if (!impl_->initialized) {
            return;
        }

        g_main_context_invoke(
            impl_->context,
            [](gpointer data) -> gboolean {
                auto* loop = static_cast<GMainLoop*>(data);
                g_main_loop_quit(loop);
                return G_SOURCE_REMOVE;
            },
            impl_->loop);

        if (impl_->thread.joinable()) {
            impl_->thread.join();
        }

        if (impl_->bus) {
            g_dbus_connection_signal_unsubscribe(impl_->bus, impl_->reply_subscription);
            g_object_unref(impl_->bus);
        }
        g_main_loop_unref(impl_->loop);
        g_main_context_unref(impl_->context);
        notify_uninit();
    }

    bool DesktopNotifier::init() {
        if (impl_->initialized) {
            return true;
        }

        if (!notify_init("Tether")) {
            debug::log(ERR, "Failed to initialize libnotify");
            return false;
        }

        tether::app_icons::set_image_filter(shape_app_icon);

        impl_->context = g_main_context_new();
        impl_->loop = g_main_loop_new(impl_->context, FALSE);
        impl_->thread = std::thread([impl = impl_.get()]() {
            g_main_context_push_thread_default(impl->context);
            // Walking the icon themes takes long enough to be worth doing before
            // the first notification rather than during it.
            installed_icons();
            // Subscribed from this thread so replies are dispatched on the same
            // context that owns g_reply_threads.
            GError* error = nullptr;
            impl->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
            if (impl->bus) {
                impl->reply_subscription = g_dbus_connection_signal_subscribe(impl->bus,
                                                                              "org.freedesktop.Notifications",
                                                                              "org.freedesktop.Notifications",
                                                                              "NotificationReplied",
                                                                              "/org/freedesktop/Notifications",
                                                                              nullptr,
                                                                              G_DBUS_SIGNAL_FLAGS_NONE,
                                                                              on_notification_replied,
                                                                              nullptr,
                                                                              nullptr);
            } else {
                debug::log(ERR, "No session bus for notification replies: {}", error ? error->message : "unknown");
                g_clear_error(&error);
            }
            g_main_loop_run(impl->loop);
            g_main_context_pop_thread_default(impl->context);
        });
        impl_->initialized = true;
        return true;
    }

    void DesktopNotifier::set_mute_handler(
        std::function<void(const std::string& app_id, const std::string& app_name)> handler) {
        g_mute_handler = std::move(handler);
    }

    void DesktopNotifier::set_copy_handler(
        std::function<void(const std::string& code, const std::string& handle)> handler) {
        g_copy_handler = std::move(handler);
    }

    void DesktopNotifier::set_reply_handler(
        std::function<void(const std::string& thread, const std::string& body)> handler) {
        g_reply_handler = std::move(handler);
    }

    void DesktopNotifier::set_network_trust_handler(std::function<void(const std::string& key, bool trust)> handler) {
        g_network_trust_handler = std::move(handler);
    }

    void DesktopNotifier::notify_network_trust(const std::string& key, const std::string& name) {
        if (!impl_->initialized)
            return;
        g_main_context_invoke_full(impl_->context,
                                   G_PRIORITY_DEFAULT,
                                   show_network_trust_on_main,
                                   new NetworkTrustRequest{key, name},
                                   nullptr);
    }

    void DesktopNotifier::notify(const NotificationSpec& spec) {
        if (!impl_ || !impl_->initialized || !desktop_popups_enabled()) {
            return;
        }

        // Bluetooth delivers on its own thread; libnotify's proxy belongs to the
        // notifier's loop.
        auto* copy = new NotificationSpec(spec);
        g_main_context_invoke_full(impl_->context, G_PRIORITY_DEFAULT, show_spec_on_main, copy, nullptr);
    }

    void DesktopNotifier::withdraw(uint32_t ancs_uid) {
        if (!impl_ || !impl_->initialized)
            return;
        g_main_context_invoke_full(
            impl_->context, G_PRIORITY_DEFAULT, withdraw_on_main, GUINT_TO_POINTER(ancs_uid), nullptr);
    }

    void DesktopNotifier::notify_file_arrived(const std::filesystem::path& path) {
        if (!impl_->initialized || !desktop_popups_enabled()) {
            return;
        }

        auto* request = new NotificationRequest{path};
        g_main_context_invoke_full(impl_->context, G_PRIORITY_DEFAULT, show_notification_on_main, request, nullptr);
    }

} // namespace tether
