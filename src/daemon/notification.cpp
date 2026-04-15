#include "notification.hpp"

#include <gio/gio.h>
#include <glib.h>
#include <libnotify/notify.h>

#include <iostream>
#include <string>
#include <thread>

namespace tether {

namespace {

struct NotificationRequest {
    std::filesystem::path path;
};

struct NotificationActionData {
    std::string uri;
};

void free_request(gpointer data) {
    delete static_cast<NotificationRequest*>(data);
}

void free_action_data(gpointer data) {
    delete static_cast<NotificationActionData*>(data);
}

bool launch_uri(const std::string& uri) {
    GError* error = nullptr;
    gboolean ok = g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &error);
    if (!ok) {
        std::cerr << "Failed to open URI '" << uri << "'";
        if (error) {
            std::cerr << ": " << error->message;
            g_error_free(error);
        }
        std::cerr << std::endl;
        return false;
    }

    return true;
}

void on_notification_action(NotifyNotification*, char*, gpointer user_data) {
    auto* action = static_cast<NotificationActionData*>(user_data);
    if (!action) {
        return;
    }

    launch_uri(action->uri);
}

void on_notification_closed(NotifyNotification* notification, gpointer) {
    g_object_unref(notification);
}

gboolean show_notification_on_main(gpointer user_data) {
    std::unique_ptr<NotificationRequest> request(static_cast<NotificationRequest*>(user_data));

    auto file = std::filesystem::absolute(request->path);
    auto file_string = file.string();
    auto folder_string = file.parent_path().string();
    auto file_uri = g_filename_to_uri(file_string.c_str(), nullptr, nullptr);
    auto folder_uri = g_filename_to_uri(folder_string.c_str(), nullptr, nullptr);

    if (!file_uri || !folder_uri) {
        if (file_uri) g_free(file_uri);
        if (folder_uri) g_free(folder_uri);
        std::cerr << "Failed to create notification URI for " << file << std::endl;
        return G_SOURCE_REMOVE;
    }

    NotifyNotification* notification = notify_notification_new(
        "File arrived",
        file.filename().c_str(),
        "document-save");

    notify_notification_set_hint(notification, "desktop-entry", g_variant_new_string("tether"));
    notify_notification_set_hint(notification, "resident", g_variant_new_boolean(TRUE));
    notify_notification_set_timeout(notification, 15000);

    auto* open_file = new NotificationActionData{file_uri};
    auto* open_folder = new NotificationActionData{folder_uri};

    g_signal_connect(notification, "closed", G_CALLBACK(on_notification_closed), nullptr);
    notify_notification_add_action(
        notification,
        "open-file",
        "Open File",
        on_notification_action,
        open_file,
        free_action_data);
    notify_notification_add_action(
        notification,
        "open-folder",
        "Open Folder",
        on_notification_action,
        open_folder,
        free_action_data);

    GError* error = nullptr;
    gboolean shown = notify_notification_show(notification, &error);
    if (!shown) {
        std::cerr << "Failed to show notification";
        if (error) {
            std::cerr << ": " << error->message;
            g_error_free(error);
        }
        std::cerr << std::endl;
        g_object_unref(notification);
    }
    g_free(file_uri);
    g_free(folder_uri);
    return G_SOURCE_REMOVE;
}

} // namespace

struct FileArrivalNotifier::Impl {
    GMainContext* context = nullptr;
    GMainLoop* loop = nullptr;
    std::thread thread;
    bool initialized = false;
};

FileArrivalNotifier::FileArrivalNotifier() : impl_(std::make_unique<Impl>()) {}

FileArrivalNotifier::~FileArrivalNotifier() {
    if (!impl_->initialized) {
        return;
    }

    g_main_context_invoke(impl_->context, [](gpointer data) -> gboolean {
        auto* loop = static_cast<GMainLoop*>(data);
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }, impl_->loop);

    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }

    g_main_loop_unref(impl_->loop);
    g_main_context_unref(impl_->context);
    notify_uninit();
}

bool FileArrivalNotifier::init() {
    if (impl_->initialized) {
        return true;
    }

    if (!notify_init("tetherd")) {
        std::cerr << "Failed to initialize libnotify" << std::endl;
        return false;
    }

    impl_->context = g_main_context_new();
    impl_->loop = g_main_loop_new(impl_->context, FALSE);
    impl_->thread = std::thread([ctx = impl_->context, loop = impl_->loop]() {
        g_main_context_push_thread_default(ctx);
        g_main_loop_run(loop);
        g_main_context_pop_thread_default(ctx);
    });
    impl_->initialized = true;
    return true;
}

void FileArrivalNotifier::notify_file_arrived(const std::filesystem::path& path) {
    if (!impl_->initialized) {
        return;
    }

    auto* request = new NotificationRequest{path};
    g_main_context_invoke_full(
        impl_->context,
        G_PRIORITY_DEFAULT,
        show_notification_on_main,
        request,
        nullptr);
}

} // namespace tether
