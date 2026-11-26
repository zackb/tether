#include "tray.hpp"

#include "daemon_client.hpp"
#include "prefs.hpp"
#include <tether/i18n.hpp>

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <nlohmann/json.hpp>
#include <tether/log.hpp>
#include <tether/service.hpp>
#include <unistd.h>

namespace tether::ui {

    namespace {

        // StatusNotifierItem: the tray protocol, spoken directly over D-Bus.
        constexpr const char* SNI_INTERFACE = "org.kde.StatusNotifierItem";
        constexpr const char* SNI_PATH = "/StatusNotifierItem";
        constexpr const char* WATCHER_NAME = "org.kde.StatusNotifierWatcher";
        constexpr const char* WATCHER_PATH = "/StatusNotifierWatcher";

        constexpr const char* INTROSPECTION = R"XML(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <method name="Activate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="ContextMenu">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Scroll">
      <arg name="delta" type="i" direction="in"/>
      <arg name="orientation" type="s" direction="in"/>
    </method>
    <signal name="NewIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg name="status" type="s"/></signal>
  </interface>
</node>
)XML";

        // com.canonical.dbusmenu: the context menu the tray host draws for us.
        constexpr const char* MENU_INTERFACE = "com.canonical.dbusmenu";
        constexpr const char* MENU_PATH = "/MenuBar";

        constexpr const char* MENU_INTROSPECTION = R"XML(
<node>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout">
      <arg name="parentId" type="i" direction="in"/>
      <arg name="recursionDepth" type="i" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="revision" type="u" direction="out"/>
      <arg name="layout" type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="properties" type="a(ia{sv})" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg name="id" type="i" direction="in"/>
      <arg name="name" type="s" direction="in"/>
      <arg name="value" type="v" direction="out"/>
    </method>
    <method name="Event">
      <arg name="id" type="i" direction="in"/>
      <arg name="eventId" type="s" direction="in"/>
      <arg name="data" type="v" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg name="events" type="a(isvu)" direction="in"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg name="id" type="i" direction="in"/>
      <arg name="needUpdate" type="b" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="updatesNeeded" type="ai" direction="out"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <signal name="ItemsPropertiesUpdated">
      <arg name="updatedProps" type="a(ia{sv})"/>
      <arg name="removedProps" type="a(ias)"/>
    </signal>
    <signal name="LayoutUpdated">
      <arg name="revision" type="u"/>
      <arg name="parent" type="i"/>
    </signal>
    <signal name="ItemActivationRequested">
      <arg name="id" type="i"/>
      <arg name="timestamp" type="u"/>
    </signal>
  </interface>
</node>
)XML";

        enum MenuId : gint32 {
            MENU_ROOT = 0,
            MENU_SHOW = 1,
            MENU_SEP_BT = 2,
            MENU_BLUETOOTH = 3,
            MENU_SEP_QUIT = 4,
            MENU_QUIT = 5,
            MENU_TRUST = 6,
            MENU_LAST = MENU_TRUST,
        };

        struct RouteState {
            bool ok = false;
            std::string detail;
        };

        // The network Tether is hidden on while it is shown on none; empty hides the menu item.
        std::string g_hidden_network_key;
        std::string g_hidden_network_name;

        GDBusConnection* g_bus = nullptr;
        GDBusNodeInfo* g_node = nullptr;
        std::string g_bus_name;
        guint g_own_id = 0;
        guint g_watch_id = 0;
        bool g_exported = false;
        GDBusNodeInfo* g_menu_node = nullptr;
        bool g_menu_exported = false;
        guint32 g_menu_revision = 1;

        bool g_bt_available = false;
        bool g_bt_bonded = false;
        bool g_bt_enabled = true;

        RouteState g_state[2];
        std::string g_icon = icon_name("tether-offline");
        int g_unread = 0;
        std::string g_tooltip;
        std::string g_airpods_name;
        std::string g_airpods_battery;
        std::string g_phone_name;
        std::string g_phone_battery;
        bool g_close_to_tray = false;
        std::string g_icon_style = "symbolic";
        std::string g_icon_suffix;

        RouteState& state(Route route) { return g_state[route == Route::WiFi ? 0 : 1]; }
        const char* route_name(Route route) { return route == Route::WiFi ? "Wi-Fi" : "Bluetooth"; }

        void load_prefs() {
            g_close_to_tray = prefs().value("close_to_tray", false);
            g_icon_style = prefs().value("tray_icon", std::string("symbolic"));
        }

        // Symbolic icons are drawn in the panel's foreground color, so the item matches the rest of
        // the tray. Falls back to the color set when the symbolic one is not installed.
        void resolve_icon_style() {
            if (g_icon_style == "color")
                return;
            if (gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), icon_name("tether-symbolic").c_str()))
                g_icon_suffix = "-symbolic";
        }

        void present_window() {
            if (GtkWidget* window = main_window())
                gtk_window_present(GTK_WINDOW(window));
        }

        // The only hide affordance on a desktop with no window buttons. Keyed on
        // visibility, not focus: clicking the icon takes focus off the window.
        void toggle_window() {
            GtkWidget* window = main_window();
            if (!window)
                return;
            if (gtk_widget_get_visible(window))
                gtk_widget_hide(window);
            else
                gtk_window_present(GTK_WINDOW(window));
        }

        void emit(const char* signal) {
            if (!g_exported)
                return;
            g_dbus_connection_emit_signal(g_bus, nullptr, SNI_PATH, SNI_INTERFACE, signal, nullptr, nullptr);
        }

        GVariant* build_tooltip() {
            GVariantBuilder pixmaps;
            g_variant_builder_init(&pixmaps, G_VARIANT_TYPE("a(iiay)"));
            return g_variant_new("(sa(iiay)ss)", "", &pixmaps, "Tether", g_tooltip.c_str());
        }

        GVariant* get_property(
            GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* property, GError**, gpointer) {
            const std::string name = property;
            if (name == "Category")
                return g_variant_new_string("ApplicationStatus");
            if (name == "Id")
                return g_variant_new_string("tether");
            if (name == "Title")
                return g_variant_new_string("Tether");
            if (name == "Status")
                return g_variant_new_string("Active");
            if (name == "IconName")
                return g_variant_new_string(g_icon.c_str());
            if (name == "ItemIsMenu")
                return g_variant_new_boolean(FALSE);
            if (name == "Menu")
                return g_variant_new_object_path(g_menu_exported ? MENU_PATH : "/NO_DBUSMENU");
            if (name == "ToolTip")
                return build_tooltip();
            return nullptr;
        }

        void on_method(GDBusConnection*,
                       const gchar*,
                       const gchar*,
                       const gchar*,
                       const gchar* method,
                       GVariant*,
                       GDBusMethodInvocation* invocation,
                       gpointer) {
            const std::string name = method;
            if (name == "Activate" || name == "SecondaryActivate")
                toggle_window();
            else if (name == "ContextMenu")
                present_window();
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }

        constexpr GDBusInterfaceVTable VTABLE = {on_method, get_property, nullptr, {nullptr}};

        // ---- dbusmenu ----

        bool bt_item_visible() { return g_bt_available && g_bt_bonded; }

        std::string bt_item_label() {
            const std::string phone = g_phone_name.empty() ? "iPhone" : g_phone_name;
            // TRANSLATORS: {} is the iPhone's name. Stops Bluetooth without unpairing.
            return g_bt_enabled ? tether::tr_format(_("Disconnect {}"), phone)
                                // TRANSLATORS: {} is the iPhone's name.
                                : tether::tr_format(_("Connect {}"), phone);
        }

        GVariant* menu_item_props(gint32 id) {
            GVariantBuilder props;
            g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
            auto add_str = [&](const char* key, const std::string& value) {
                g_variant_builder_add(&props, "{sv}", key, g_variant_new_string(value.c_str()));
            };
            switch (id) {
                case MENU_ROOT:
                    add_str("children-display", "submenu");
                    break;
                case MENU_SHOW:
                    add_str("label", _("Show Tether"));
                    break;
                case MENU_SEP_BT:
                    add_str("type", "separator");
                    g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(bt_item_visible()));
                    break;
                case MENU_BLUETOOTH:
                    add_str("label", bt_item_label());
                    add_str("icon-name", g_bt_enabled ? "bluetooth-disabled-symbolic" : "bluetooth-active-symbolic");
                    g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(bt_item_visible()));
                    break;
                case MENU_SEP_QUIT:
                    add_str("type", "separator");
                    break;
                case MENU_QUIT:
                    add_str("label", _("Quit"));
                    break;
                case MENU_TRUST:
                    // TRANSLATORS: {} is a Wi-Fi or network connection name.
                    add_str("label", tether::tr_format(_("Show Tether on {}"), g_hidden_network_name));
                    add_str("icon-name", "network-wireless-symbolic");
                    g_variant_builder_add(
                        &props, "{sv}", "visible", g_variant_new_boolean(!g_hidden_network_key.empty()));
                    break;
                default:
                    break;
            }
            return g_variant_builder_end(&props);
        }

        constexpr gint32 MENU_CHILDREN[] = {
            MENU_SHOW, MENU_TRUST, MENU_SEP_BT, MENU_BLUETOOTH, MENU_SEP_QUIT, MENU_QUIT};

        bool menu_id_valid(gint32 id) { return id >= MENU_ROOT && id <= MENU_LAST; }

        GVariant* menu_layout(gint32 id, gint32 depth) {
            GVariantBuilder children;
            g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
            if (id == MENU_ROOT && depth != 0) {
                for (gint32 child : MENU_CHILDREN)
                    g_variant_builder_add(&children, "v", menu_layout(child, depth - 1));
            }
            return g_variant_new("(i@a{sv}av)", id, menu_item_props(id), &children);
        }

        void menu_layout_changed() {
            ++g_menu_revision;
            if (!g_menu_exported)
                return;
            g_dbus_connection_emit_signal(g_bus,
                                          nullptr,
                                          MENU_PATH,
                                          MENU_INTERFACE,
                                          "LayoutUpdated",
                                          g_variant_new("(ui)", g_menu_revision, (gint32)MENU_ROOT),
                                          nullptr);
        }

        void menu_activate(gint32 id) {
            switch (id) {
                case MENU_SHOW:
                    present_window();
                    break;
                case MENU_TRUST:
                    if (!g_hidden_network_key.empty())
                        daemon_send({{"command", "trust_network"}, {"key", g_hidden_network_key}});
                    break;
                case MENU_BLUETOOTH: {
                    const bool enable = !g_bt_enabled;
                    if (daemon_send({{"command", "bt_set_enabled"}, {"enabled", enable}})) {
                        // The daemon confirms with bt_status; flip now so a quick reopen is not stale.
                        g_bt_enabled = enable;
                        menu_layout_changed();
                    }
                    break;
                }
                case MENU_QUIT:
                    if (GApplication* app = g_application_get_default())
                        g_application_quit(app);
                    break;
                default:
                    break;
            }
        }

        GVariant* menu_get_property(
            GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* property, GError**, gpointer) {
            const std::string name = property;
            if (name == "Version")
                return g_variant_new_uint32(3);
            if (name == "TextDirection")
                return g_variant_new_string("ltr");
            if (name == "Status")
                return g_variant_new_string("normal");
            if (name == "IconThemePath")
                return g_variant_new_strv(nullptr, 0);
            return nullptr;
        }

        void on_menu_method(GDBusConnection*,
                            const gchar*,
                            const gchar*,
                            const gchar*,
                            const gchar* method,
                            GVariant* params,
                            GDBusMethodInvocation* invocation,
                            gpointer) {
            const std::string name = method;
            if (name == "GetLayout") {
                gint32 parent = 0;
                gint32 depth = -1;
                g_variant_get(params, "(ii^a&s)", &parent, &depth, nullptr);
                if (!menu_id_valid(parent)) {
                    g_dbus_method_invocation_return_dbus_error(
                        invocation, "com.canonical.dbusmenu.Error.InvalidId", "unknown menu item");
                    return;
                }
                g_dbus_method_invocation_return_value(
                    invocation, g_variant_new("(u@(ia{sv}av))", g_menu_revision, menu_layout(parent, depth)));
                return;
            }
            if (name == "GetGroupProperties") {
                GVariantIter* ids = nullptr;
                g_variant_get(params, "(ai^a&s)", &ids, nullptr);
                GVariantBuilder out;
                g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
                gint32 id = 0;
                bool any = false;
                while (g_variant_iter_next(ids, "i", &id)) {
                    if (!menu_id_valid(id))
                        continue;
                    g_variant_builder_add(&out, "(i@a{sv})", id, menu_item_props(id));
                    any = true;
                }
                g_variant_iter_free(ids);
                // An empty request means every item.
                if (!any) {
                    for (gint32 all = MENU_ROOT; all <= MENU_LAST; ++all)
                        g_variant_builder_add(&out, "(i@a{sv})", all, menu_item_props(all));
                }
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ia{sv}))", &out));
                return;
            }
            if (name == "GetProperty") {
                gint32 id = 0;
                const gchar* key = nullptr;
                g_variant_get(params, "(i&s)", &id, &key);
                GVariant* props = menu_item_props(id);
                GVariant* value = g_variant_lookup_value(props, key, nullptr);
                g_variant_unref(g_variant_ref_sink(props));
                if (!value) {
                    g_dbus_method_invocation_return_dbus_error(
                        invocation, "com.canonical.dbusmenu.Error.InvalidProperty", "unknown property");
                    return;
                }
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
                g_variant_unref(value);
                return;
            }
            if (name == "Event") {
                gint32 id = 0;
                const gchar* event_id = nullptr;
                g_variant_get(params, "(i&svu)", &id, &event_id, nullptr, nullptr);
                if (std::string(event_id) == "clicked")
                    menu_activate(id);
                g_dbus_method_invocation_return_value(invocation, nullptr);
                return;
            }
            if (name == "EventGroup") {
                GVariantIter* events = nullptr;
                g_variant_get(params, "(a(isvu))", &events);
                gint32 id = 0;
                const gchar* event_id = nullptr;
                GVariant* data = nullptr;
                guint32 ts = 0;
                while (g_variant_iter_next(events, "(i&svu)", &id, &event_id, &data, &ts)) {
                    if (std::string(event_id) == "clicked")
                        menu_activate(id);
                    g_variant_unref(data);
                }
                g_variant_iter_free(events);
                g_dbus_method_invocation_return_value(
                    invocation, g_variant_new("(@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
                return;
            }
            if (name == "AboutToShow") {
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
                return;
            }
            if (name == "AboutToShowGroup") {
                GVariant* empty1 = g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0);
                GVariant* empty2 = g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0);
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai@ai)", empty1, empty2));
                return;
            }
            g_dbus_method_invocation_return_dbus_error(
                invocation, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
        }

        constexpr GDBusInterfaceVTable MENU_VTABLE = {on_menu_method, menu_get_property, nullptr, {nullptr}};

        void register_with_watcher() {
            if (!g_exported)
                return;
            g_dbus_connection_call(g_bus,
                                   WATCHER_NAME,
                                   WATCHER_PATH,
                                   WATCHER_NAME,
                                   "RegisterStatusNotifierItem",
                                   g_variant_new("(s)", g_bus_name.c_str()),
                                   nullptr,
                                   G_DBUS_CALL_FLAGS_NONE,
                                   -1,
                                   nullptr,
                                   nullptr,
                                   nullptr);
        }

        void on_name_acquired(GDBusConnection* connection, const gchar*, gpointer) {
            if (g_exported)
                return;
            GError* error = nullptr;
            // The menu goes first so the item's Menu property already points at it when the host reads it.
            if (g_menu_node && g_dbus_connection_register_object(connection,
                                                                 MENU_PATH,
                                                                 g_menu_node->interfaces[0],
                                                                 &MENU_VTABLE,
                                                                 nullptr,
                                                                 nullptr,
                                                                 &error) != 0) {
                g_menu_exported = true;
            } else {
                debug::log(WARN, "tray: could not export the menu ({})", error ? error->message : "unknown");
                g_clear_error(&error);
            }
            if (g_dbus_connection_register_object(
                    connection, SNI_PATH, g_node->interfaces[0], &VTABLE, nullptr, nullptr, &error) == 0) {
                debug::log(WARN, "tray: could not export the item ({})", error ? error->message : "unknown");
                g_clear_error(&error);
                return;
            }
            g_exported = true;
            register_with_watcher();
        }

    } // namespace

    void tray_init() {
        if (g_own_id != 0)
            return;

        load_prefs();
        resolve_icon_style();
        g_icon += g_icon_suffix;

        GError* error = nullptr;
        g_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!g_bus) {
            debug::log(WARN, "tray: no session bus ({})", error ? error->message : "unknown");
            g_clear_error(&error);
            return;
        }

        g_node = g_dbus_node_info_new_for_xml(INTROSPECTION, &error);
        if (!g_node) {
            debug::log(ERR, "tray: bad introspection data ({})", error ? error->message : "unknown");
            g_clear_error(&error);
            return;
        }

        g_menu_node = g_dbus_node_info_new_for_xml(MENU_INTROSPECTION, &error);
        if (!g_menu_node) {
            debug::log(WARN, "tray: bad menu introspection data ({})", error ? error->message : "unknown");
            g_clear_error(&error);
        }

        // Keyed on the process, per spec, so two instances cannot collide.
        g_bus_name = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
        g_own_id = g_bus_own_name_on_connection(
            g_bus, g_bus_name.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE, on_name_acquired, nullptr, nullptr, nullptr);

        // Re-registering on reappearance is what survives a panel restart.
        g_watch_id = g_bus_watch_name_on_connection(
            g_bus,
            WATCHER_NAME,
            G_BUS_NAME_WATCHER_FLAGS_NONE,
            +[](GDBusConnection*, const gchar*, const gchar*, gpointer) { register_with_watcher(); },
            nullptr,
            nullptr,
            nullptr);

        tray_refresh();
    }

    void tray_set_route(Route route, bool ok, const std::string& detail) {
        state(route) = {ok, detail};
        tray_refresh();
    }

    void tray_refresh() {
        std::string tooltip;
        for (Route route : {Route::WiFi, Route::Bluetooth}) {
            const RouteState& r = state(route);
            const std::string status = r.ok ? _("connected") : r.detail.empty() ? _("not connected") : r.detail;
            // TRANSLATORS: {} is a transport name, "Wi-Fi" or "Bluetooth", or a device name.
            tooltip += tether::tr_format(_("{}: {}"), route_name(route), status) + "\n";
        }
        if (!g_phone_battery.empty())
            tooltip += tether::tr_format(_("{}: {}"), g_phone_name, g_phone_battery) + "\n";
        if (!g_airpods_battery.empty())
            tooltip += tether::tr_format(_("{}: {}"), g_airpods_name, g_airpods_battery) + "\n";
        if (!daemon_connected())
            tooltip += std::string(_("Daemon: not running")) + "\n";
        if (g_unread > 0)
            tooltip += tether::tr_format(P_("{} unread message", "{} unread messages", g_unread), g_unread) + "\n";
        if (!tooltip.empty())
            tooltip.pop_back();

        const bool any_up = g_state[0].ok || g_state[1].ok;
        const std::string icon = icon_name(
            std::string(!any_up ? "tether-offline" : (g_unread > 0 ? "tether-unread" : "tether")) + g_icon_suffix);

        if (icon != g_icon) {
            g_icon = icon;
            emit("NewIcon");
        }
        if (tooltip != g_tooltip) {
            g_tooltip = tooltip;
            emit("NewToolTip");
        }
    }

    void tray_set_airpods(const std::string& name, const std::string& battery) {
        if (name == g_airpods_name && battery == g_airpods_battery)
            return;
        g_airpods_name = name;
        g_airpods_battery = battery;
        tray_refresh();
    }

    void tray_set_phone(const std::string& name, const std::string& battery) {
        if (name == g_phone_name && battery == g_phone_battery)
            return;
        const bool renamed = name != g_phone_name;
        g_phone_name = name;
        g_phone_battery = battery;
        tray_refresh();
        if (renamed)
            menu_layout_changed();
    }

    void tray_set_bluetooth(bool available, bool bonded, bool enabled) {
        if (available == g_bt_available && bonded == g_bt_bonded && enabled == g_bt_enabled)
            return;
        g_bt_available = available;
        g_bt_bonded = bonded;
        g_bt_enabled = enabled;
        menu_layout_changed();
    }

    void tray_set_hidden_network(const std::string& key, const std::string& name) {
        if (key == g_hidden_network_key && name == g_hidden_network_name)
            return;
        g_hidden_network_key = key;
        g_hidden_network_name = name;
        menu_layout_changed();
    }

    void tray_set_unread(int count) {
        if (count == g_unread)
            return;
        g_unread = count;
        tray_refresh();
    }

    bool tray_close_to_tray() { return g_close_to_tray; }

    void tray_set_close_to_tray(bool enabled) {
        if (g_close_to_tray == enabled)
            return;
        g_close_to_tray = enabled;
        prefs()["close_to_tray"] = enabled;
        prefs_save();
    }

} // namespace tether::ui
