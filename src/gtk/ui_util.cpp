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
.tether-app { background-color: @theme_bg_color; }
.tether-app .tether-nav {
    background-color: mix(@theme_bg_color, @theme_fg_color, 0.045);
    border-right: 1px solid alpha(@theme_fg_color, 0.09);
    padding: 20px 10px 16px;
    min-width: 142px;
}
.tether-app .tether-nav.collapsed { min-width: 0; }
.tether-nav-logo { border: none; box-shadow: none; background: transparent; border-radius: 10px; padding: 4px 8px; }
.tether-nav-logo:hover { background-color: alpha(@theme_fg_color, 0.07); }
.tether-nav-item { border: none; box-shadow: none; background: transparent; border-radius: 10px; padding: 12px; }
.tether-nav-item:hover { background-color: alpha(@theme_fg_color, 0.07); }
.tether-nav-item:checked { background-color: alpha(@theme_selected_bg_color, 0.20); color: @theme_fg_color; font-weight: bold; }
.tether-app headerbar { min-height: 42px; box-shadow: none; border-bottom: 1px solid alpha(@theme_fg_color, 0.08); }
.tether-page-header { padding: 22px 24px; border-bottom: 1px solid alpha(@theme_fg_color, 0.08); }
.tether-page-title { font-size: 22px; font-weight: bold; }
.tether-hero-title { font-size: 23px; font-weight: bold; }
.tether-hero-icon { color: @theme_selected_bg_color; background-color: alpha(@theme_selected_bg_color, 0.10); border-radius: 20px; padding: 18px; }
.tether-card { background-color: mix(@theme_bg_color, @theme_fg_color, 0.045); border: 1px solid alpha(@theme_fg_color, 0.09); border-radius: 16px; padding: 22px; }
.tether-detail { margin: 24px; }
.tether-modes button { border-radius: 8px; padding: 10px; }
.tether-app .tether-list { background: transparent; padding: 8px; }
.tether-app .tether-list row { padding: 0; margin: 3px 0; border-radius: 12px; border: 1px solid transparent; }
.tether-app .tether-list row:hover { background-color: alpha(@theme_fg_color, 0.055); }
.tether-app .tether-list row:selected { background-color: alpha(@theme_selected_bg_color, 0.14); border-color: alpha(@theme_selected_bg_color, 0.28); color: @theme_fg_color; }
.tether-avatar { min-width: 36px; min-height: 36px; border-radius: 24px; background-color: alpha(@theme_selected_bg_color, 0.15); color: @theme_fg_color; font-size: 16px; font-weight: bold; }
.tether-app .tether-feed { padding: 12px 24px 24px; }
.tether-app .tether-feed row { background-color: mix(@theme_bg_color, @theme_fg_color, 0.035); border: 1px solid alpha(@theme_fg_color, 0.09); border-radius: 14px; margin: 0 0 10px; padding: 8px; }
.tether-app entry { border-radius: 10px; min-height: 34px; box-shadow: none; }
.tether-thread-pane { background-color: alpha(@theme_fg_color, 0.025); border-right: 1px solid alpha(@theme_fg_color, 0.08); }
.tether-app .tether-conversation { padding: 16px 12px; background: transparent; }
.tether-app .tether-conversation row { background: transparent; }
.tether-conversation-header { padding: 14px 10px; font-size: 120%; background-color: alpha(@theme_fg_color, 0.025); }
.tether-composer { margin: 12px 16px 16px; border: 1px solid alpha(@theme_fg_color, 0.14); background-color: alpha(@theme_fg_color, 0.03); border-radius: 16px; padding: 8px; }
.tether-composer scrolledwindow, .tether-composer textview, .tether-composer textview text { border: none; box-shadow: none; background: transparent; }
.tether-composer button { border-radius: 10px; min-height: 32px; }
.tether-settings { background-color: @theme_bg_color; }
.tether-settings list { background-color: mix(@theme_bg_color, @theme_fg_color, 0.035); border: 1px solid alpha(@theme_fg_color, 0.10); border-radius: 14px; }
.tether-settings list row { background: transparent; padding: 5px 8px; border-bottom: 1px solid alpha(@theme_fg_color, 0.06); }
.tether-settings list row:last-child { border-bottom: none; }
.tether-settings-heading { font-size: 115%; padding: 18px 0 6px; }
.tether-setting-title { font-weight: bold; }
.tether-empty { padding: 36px; }
.tether-empty label { font-size: 110%; }
.tether-app button.suggested-action { border-radius: 10px; padding: 9px 16px; }
.muted {
    opacity: 0.75;
    font-size: 90%;
}

.tether-bubble {
    padding: 11px 16px;
    border-radius: 18px;
}

.tether-bubble-in {
    background-color: mix(@theme_bg_color, @theme_fg_color, 0.09);
    border: 1px solid alpha(@theme_fg_color, 0.08);
}

.tether-bubble-out {
    background-color: @theme_selected_bg_color;
    color: @theme_selected_fg_color;
}

.tether-route-bar {
    border-top: 1px solid alpha(@theme_fg_color, 0.09);
    padding: 5px 12px;
    font-size: 90%;
}

.tether-route-off {
    opacity: 0.75;
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
    padding: 32px;
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

    void style(GtkWidget* widget, const char* name) {
        gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
    }

    GtkWidget* navigation_icon(const char* name, int size) {
        GtkWidget* icon = gtk_drawing_area_new();
        gtk_widget_set_size_request(icon, size, size);
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
        g_object_set_data_full(G_OBJECT(icon), "icon-name", g_strdup(name), g_free);
        g_signal_connect(icon,
                         "draw",
                         G_CALLBACK(+[](GtkWidget* widget, cairo_t* cr, gpointer) -> gboolean {
                             GtkStyleContext* context = gtk_widget_get_style_context(widget);
                             const int width = gtk_widget_get_allocated_width(widget);
                             const int height = gtk_widget_get_allocated_height(widget);
                             gtk_render_background(context, cr, 0, 0, width, height);
                             gtk_render_frame(context, cr, 0, 0, width, height);
                             GdkRGBA color;
                             gtk_style_context_get_color(
                                 gtk_widget_get_style_context(widget), gtk_widget_get_state_flags(widget), &color);
                             gdk_cairo_set_source_rgba(cr, &color);
                             cairo_scale(cr,
                                         gtk_widget_get_allocated_width(widget) / 24.0,
                                         gtk_widget_get_allocated_height(widget) / 24.0);
                             cairo_set_line_width(cr, 1.7);
                             cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                             cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                             const std::string name =
                                 static_cast<const char*>(g_object_get_data(G_OBJECT(widget), "icon-name"));
                             if (name == "logo") {
                                 // The two linked rings from tether-symbolic.svg.
                                 cairo_set_line_width(cr, 2.4);
                                 cairo_arc(cr, 7.35, 12, 3.9, 0, 6.283185);
                                 cairo_stroke(cr);
                                 cairo_arc(cr, 16.65, 12, 3.9, 0, 6.283185);
                             } else if (name == "contacts") {
                                 cairo_arc(cr, 12, 7, 3.5, 0, 6.283185);
                                 cairo_stroke(cr);
                                 cairo_move_to(cr, 4, 21);
                                 cairo_curve_to(cr, 4, 11, 20, 11, 20, 21);
                             } else if (name == "messages") {
                                 cairo_move_to(cr, 4, 4);
                                 cairo_line_to(cr, 20, 4);
                                 cairo_line_to(cr, 20, 17);
                                 cairo_line_to(cr, 10, 17);
                                 cairo_line_to(cr, 4, 21);
                                 cairo_close_path(cr);
                                 cairo_move_to(cr, 8, 9);
                                 cairo_line_to(cr, 16, 9);
                                 cairo_move_to(cr, 8, 13);
                                 cairo_line_to(cr, 13, 13);
                             } else if (name == "notifications") {
                                 cairo_move_to(cr, 5, 17);
                                 cairo_line_to(cr, 7, 14);
                                 cairo_line_to(cr, 7, 9);
                                 cairo_curve_to(cr, 7, 2, 17, 2, 17, 9);
                                 cairo_line_to(cr, 17, 14);
                                 cairo_line_to(cr, 19, 17);
                                 cairo_close_path(cr);
                                 cairo_move_to(cr, 10, 21);
                                 cairo_line_to(cr, 14, 21);
                             } else if (name == "settings") {
                                 cairo_arc(cr, 12, 12, 3, 0, 6.283185);
                                 cairo_stroke(cr);
                                 cairo_arc(cr, 12, 12, 7, 0, 6.283185);
                                 cairo_stroke(cr);
                                 for (int i = 0; i < 8; ++i) {
                                     cairo_save(cr);
                                     cairo_translate(cr, 12, 12);
                                     cairo_rotate(cr, i * 0.785398);
                                     cairo_move_to(cr, 0, 7);
                                     cairo_line_to(cr, 0, 10);
                                     cairo_stroke(cr);
                                     cairo_restore(cr);
                                 }
                             } else if (name == "calls") {
                                 cairo_move_to(cr, 5, 3);
                                 cairo_line_to(cr, 9, 3);
                                 cairo_line_to(cr, 11, 8);
                                 cairo_line_to(cr, 8, 10);
                                 cairo_curve_to(cr, 10, 14, 11, 15, 15, 17);
                                 cairo_line_to(cr, 17, 14);
                                 cairo_line_to(cr, 21, 16);
                                 cairo_line_to(cr, 21, 20);
                                 cairo_curve_to(cr, 12, 25, -1, 12, 5, 3);
                             } else {
                                 cairo_rectangle(cr, 6, 2, 12, 20);
                                 cairo_move_to(cr, 10, 5);
                                 cairo_line_to(cr, 14, 5);
                                 cairo_move_to(cr, 11, 19);
                                 cairo_line_to(cr, 13, 19);
                             }
                             cairo_stroke(cr);
                             return FALSE;
                         }),
                         nullptr);
        return icon;
    }

    GtkWidget* avatar(const std::string& name) {
        std::string first;
        std::string last;
        bool in_word = false;
        for (const char* cursor = name.c_str(); *cursor; cursor = g_utf8_next_char(cursor)) {
            const gunichar character = g_utf8_get_char(cursor);
            if (g_unichar_isspace(character)) {
                in_word = false;
            } else if (!in_word) {
                const std::string initial(cursor, g_utf8_next_char(cursor) - cursor);
                if (first.empty())
                    first = initial;
                else
                    last = initial;
                in_word = true;
            }
        }
        const std::string initials = first + last;
        gchar* upper = g_utf8_strup(initials.c_str(), -1);
        GtkWidget* label = gtk_label_new(*upper ? upper : "•");
        g_free(upper);
        style(label, "tether-avatar");
        gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
        return label;
    }

    GtkWidget* page_header(const std::string& title, const char* icon) {
        GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        style(header, "tether-page-header");
        gtk_box_pack_start(GTK_BOX(header), navigation_icon(icon, 26), FALSE, FALSE, 0);
        GtkWidget* label = gtk_label_new(title.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        style(label, "tether-page-title");
        gtk_box_pack_start(GTK_BOX(header), label, TRUE, TRUE, 0);
        return header;
    }

    GtkWidget* page_frame(GtkWidget* content, const std::string& title, const char* icon) {
        GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        style(page, "tether-page");
        gtk_box_pack_start(GTK_BOX(page), page_header(title, icon), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), content, TRUE, TRUE, 0);
        return page;
    }

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

    void set_accessible_name(GtkWidget* widget, const std::string& name) {
        if (widget) {
            atk_object_set_name(gtk_widget_get_accessible(widget), name.c_str());
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
        atk_object_set_description(gtk_widget_get_accessible(r.label), detail.c_str());

        tray_set_route(route, ok, detail);
    }

} // namespace tether::ui
