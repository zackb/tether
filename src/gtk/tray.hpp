#pragma once

#include "ui_util.hpp"

#include <string>

namespace tether::ui {

    // Publishes a StatusNotifierItem. A desktop with no tray never claims it.
    void tray_init();

    // Mirrors set_route_status; the tooltip is rebuilt from both routes at once.
    void tray_set_route(Route route, bool ok, const std::string& detail);

    // Total unread across threads. Drives the badged icon and a tooltip line.
    void tray_set_unread(int count);

    // AirPods battery for the tooltip, already formatted. Empty removes the line:
    // nothing connected, or the channel has reported nothing yet.
    void tray_set_airpods(const std::string& name, const std::string& battery);

    // The supervised iPhone's battery for the tooltip; empty hides the line.
    void tray_set_phone(const std::string& name, const std::string& battery);

    // Bluetooth supervision state from bt_status; drives the tray menu's connect/disconnect item.
    void tray_set_bluetooth(bool available, bool bonded, bool enabled);

    // The network Tether is hidden on; empty key removes the "Show Tether on" menu item.
    void tray_set_hidden_network(const std::string& key, const std::string& name);

    // Repaints from cached state, for when the daemon returns before any route event.
    void tray_refresh();

    // False means the close button quits, as it always has.
    bool tray_close_to_tray();
    void tray_set_close_to_tray(bool enabled);

} // namespace tether::ui
