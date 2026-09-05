#include "ui_util.hpp"

#include "tray.hpp"

#include <tether/i18n.hpp>
#include <tether/version.hpp>

namespace tether::ui {

    namespace {
        GtkWidget* g_window = nullptr;
        GtkWidget* g_header_bar = nullptr;

        struct RouteIndicator {
            GtkWidget* box = nullptr;
            GtkWidget* icon = nullptr;
            GtkWidget* label = nullptr;
            const char* name = nullptr;
            const char* icon_ok = nullptr;
            const char* icon_off = nullptr;
        };

        RouteIndicator g_routes[2];

        RouteIndicator& indicator(Route route) { return g_routes[route == Route::WiFi ? 0 : 1]; }

        constexpr const char* STYLE = R"CSS(
.muted {
    opacity: 0.62;
    font-size: 90%;
}

.tether-bubble {
    padding: 8px 12px;
    border-radius: 14px;
}

.tether-bubble-in {
    background-color: alpha(@theme_fg_color, 0.10);
}

.tether-bubble-out {
    background-color: @theme_selected_bg_color;
    color: @theme_selected_fg_color;
}

.tether-route-bar {
    border-top: 1px solid alpha(@theme_fg_color, 0.12);
}

.tether-route-off {
    opacity: 0.5;
}

.tether-setup {
    background-color: alpha(@theme_fg_color, 0.07);
    border: 1px solid alpha(@theme_fg_color, 0.18);
    border-radius: 8px;
    padding: 12px;
}

.tether-setup-command {
    font-family: monospace;
    font-size: 92%;
}

.tether-thread-unread {
    opacity: 1;
    font-weight: bold;
}

.tether-send-error {
    background-color: alpha(#e5a50a, 0.20);
    border-top: 1px solid alpha(@theme_fg_color, 0.12);
}

.tether-badge {
    background-color: @theme_selected_bg_color;
    color: @theme_selected_fg_color;
    border-radius: 10px;
    padding: 0 8px;
}

.tether-dropzone {
    border: 2px dashed alpha(@theme_fg_color, 0.28);
    border-radius: 12px;
    padding: 20px 28px;
}

.tether-dropzone-active {
    border-color: @theme_selected_bg_color;
    background-color: alpha(@theme_selected_bg_color, 0.12);
}
)CSS";
        // xdg-desktop-portal Settings: 0 = no preference, 1 = dark, 2 = light.
        void apply_color_scheme(guint32 scheme) {
            GtkSettings* settings = gtk_settings_get_default();
            if (!settings)
                return;
            if (scheme == 1 || scheme == 2)
                g_object_set(settings, "gtk-application-prefer-dark-theme", scheme == 1 ? TRUE : FALSE, nullptr);
        }

        void read_color_scheme(GDBusProxy* proxy) {
            GError* error = nullptr;
            GVariant* result =
                g_dbus_proxy_call_sync(proxy,
                                       "Read",
                                       g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
                                       G_DBUS_CALL_FLAGS_NONE,
                                       -1,
                                       nullptr,
                                       &error);
            if (!result) {
                g_clear_error(&error);
                return;
            }
            // Read returns the value boxed twice: (v) holding a v holding the uint32.
            GVariant* outer = nullptr;
            g_variant_get(result, "(v)", &outer);
            GVariant* inner = g_variant_get_variant(outer);
            if (g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32))
                apply_color_scheme(g_variant_get_uint32(inner));
            g_variant_unref(inner);
            g_variant_unref(outer);
            g_variant_unref(result);
        }

        void on_setting_changed(GDBusProxy*, const gchar*, const gchar* signal_name, GVariant* params, gpointer) {
            if (g_strcmp0(signal_name, "SettingChanged") != 0)
                return;
            const gchar* nspace = nullptr;
            const gchar* key = nullptr;
            GVariant* value = nullptr;
            g_variant_get(params, "(&s&sv)", &nspace, &key, &value);
            if (g_strcmp0(nspace, "org.freedesktop.appearance") == 0 && g_strcmp0(key, "color-scheme") == 0 &&
                g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32))
                apply_color_scheme(g_variant_get_uint32(value));
            g_variant_unref(value);
        }
    } // namespace

    void follow_system_color_scheme() {
        // An explicit GTK_THEME overrides.
        if (g_getenv("GTK_THEME"))
            return;

        static GDBusProxy* proxy = nullptr;
        if (proxy)
            return;

        GError* error = nullptr;
        proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
                                              G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                              nullptr,
                                              "org.freedesktop.portal.Desktop",
                                              "/org/freedesktop/portal/desktop",
                                              "org.freedesktop.portal.Settings",
                                              nullptr,
                                              &error);
        if (!proxy) {
            // no portal
            g_clear_error(&error);
            return;
        }

        g_signal_connect(proxy, "g-signal", G_CALLBACK(on_setting_changed), nullptr);
        read_color_scheme(proxy);
    }

    void install_style() {
        GdkScreen* screen = gdk_screen_get_default();
        if (!screen)
            return;

        GtkCssProvider* provider = gtk_css_provider_new();
        GError* error = nullptr;
        if (!gtk_css_provider_load_from_data(provider, STYLE, -1, &error)) {
            g_warning("tether: stylesheet rejected: %s", error ? error->message : "unknown");
            g_clear_error(&error);
        }
        gtk_style_context_add_provider_for_screen(
            screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider);
    }

    std::string fold(const std::string& text) {
        gchar* normalized = g_utf8_normalize(text.c_str(), -1, G_NORMALIZE_ALL);
        gchar* folded = g_utf8_casefold(normalized ? normalized : text.c_str(), -1);
        std::string out = folded ? folded : "";
        g_free(normalized);
        g_free(folded);
        return out;
    }

    std::string escape_markup(const std::string& text) {
        gchar* escaped = g_markup_escape_text(text.c_str(), -1);
        std::string result = escaped ? escaped : "";
        g_free(escaped);
        return result;
    }

    void set_markup(GtkWidget* label, const std::string& text) {
        if (label) {
            gtk_label_set_markup(GTK_LABEL(label), text.c_str());
        }
    }

    void set_text(GtkWidget* label, const std::string& text) {
        if (label) {
            gtk_label_set_text(GTK_LABEL(label), text.c_str());
        }
    }

    void clear_list_box(GtkWidget* list_box) {
        GList* children = gtk_container_get_children(GTK_CONTAINER(list_box));
        for (GList* iter = children; iter; iter = g_list_next(iter)) {
            gtk_widget_destroy(GTK_WIDGET(iter->data));
        }
        g_list_free(children);
    }

    void set_main_window(GtkWidget* window) { g_window = window; }

    GtkWidget* main_window() { return g_window; }

    void set_header_bar(GtkWidget* bar) { g_header_bar = bar; }

    void set_status_main(const std::string& text) {
        if (g_header_bar) {
            gtk_header_bar_set_subtitle(GTK_HEADER_BAR(g_header_bar), text.c_str());
        }
    }

    GtkWidget* create_route_bar() {
        GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
        gtk_container_set_border_width(GTK_CONTAINER(bar), 6);
        gtk_style_context_add_class(gtk_widget_get_style_context(bar), "tether-route-bar");

        auto build = [](RouteIndicator& route, const char* name, const char* icon_ok, const char* icon_off) {
            route.icon_ok = icon_ok;
            route.icon_off = icon_off;
            route.name = name;
            route.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            route.icon = gtk_image_new_from_icon_name(icon_off, GTK_ICON_SIZE_MENU);
            gtk_box_pack_start(GTK_BOX(route.box), route.icon, FALSE, FALSE, 0);
            route.label = gtk_label_new(nullptr);
            gtk_label_set_ellipsize(GTK_LABEL(route.label), PANGO_ELLIPSIZE_END);
            gtk_box_pack_start(GTK_BOX(route.box), route.label, FALSE, FALSE, 0);
        };

        build(indicator(Route::WiFi),
              "Wi-Fi",
              "network-wireless-signal-excellent-symbolic",
              "network-wireless-offline-symbolic");
        build(indicator(Route::Bluetooth), "Bluetooth", "bluetooth-active-symbolic", "bluetooth-disabled-symbolic");

        gtk_box_pack_start(GTK_BOX(bar), indicator(Route::WiFi).box, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bar), indicator(Route::Bluetooth).box, FALSE, FALSE, 0);

        GtkWidget* version = gtk_label_new("v" TETHER_VERSION);
        gtk_style_context_add_class(gtk_widget_get_style_context(version), "muted");
        gtk_box_pack_end(GTK_BOX(bar), version, FALSE, FALSE, 0);

        set_route_status(Route::WiFi, false, _("Waiting for the Tether daemon."));
        set_route_status(Route::Bluetooth, false, _("Waiting for the Tether daemon."));
        return bar;
    }

    void set_route_status(Route route, bool ok, const std::string& detail) {
        RouteIndicator& r = indicator(route);
        if (!r.box)
            return;

        gtk_image_set_from_icon_name(GTK_IMAGE(r.icon), ok ? r.icon_ok : r.icon_off, GTK_ICON_SIZE_MENU);
        GtkStyleContext* context = gtk_widget_get_style_context(r.box);
        if (ok)
            gtk_style_context_remove_class(context, "tether-route-off");
        else
            gtk_style_context_add_class(context, "tether-route-off");

        // TRANSLATORS: {} is a transport name, "Wi-Fi" or "Bluetooth".
        const std::string state = tr_format(_("{}: {}"), r.name, ok ? _("connected") : _("not connected"));
        set_text(r.label, state);
        // The reason can be a sentence or two, which would push the other route
        // off the strip, so it lives in the tooltip. The Devices page shows it
        // in full.
        gtk_widget_set_tooltip_text(r.box, detail.empty() ? state.c_str() : (state + "\n" + detail).c_str());

        tray_set_route(route, ok, detail);
    }

} // namespace tether::ui
