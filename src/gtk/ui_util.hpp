#pragma once

#include <gtk/gtk.h>
#include <string>

namespace tether::ui {

    // App-owned line icons avoid missing glyphs in third-party desktop themes.
    GtkWidget* navigation_icon(const char* name, int size = 20);
    GtkWidget* avatar(const std::string& name);
    GtkWidget* page_header(const std::string& title, const char* icon);
    GtkWidget* page_frame(GtkWidget* content, const std::string& title, const char* icon);
    void style(GtkWidget* widget, const char* name);

    // Loads the application stylesheet.
    void install_style();

    // Respects the desktop's light/dark preference from the settings portal.
    // GTK3 has no built-in listener for it, so GNOME's "Dark" style would
    // otherwise render light Adwaita.
    void follow_system_color_scheme();

    std::string escape_markup(const std::string& text);

    // Normalized case- and accent-insensitive form of a string.
    std::string fold(const std::string& text);
    void set_markup(GtkWidget* label, const std::string& text);
    void set_text(GtkWidget* label, const std::string& text);
    void clear_list_box(GtkWidget* list_box);

    // Name read by screen readers. Needed where no visible label is linked,
    // such as icon-only buttons; a tooltip only becomes the description.
    void set_accessible_name(GtkWidget* widget, const std::string& name);

    // Registered once at startup so views can reach the window and the header
    // bar without every one of them holding the whole application struct.
    void set_main_window(GtkWidget* window);
    GtkWidget* main_window();
    void set_header_bar(GtkWidget* bar);

    // Transient text: what the app is doing right now. Route state belongs in
    // the indicators below, which nothing else overwrites.
    void set_status_main(const std::string& text);

    // The two independent ways the phone is reached. Both are always visible so
    // "clipboard works but messages do not" is readable at a glance.
    enum class Route { WiFi, Bluetooth };

    // Builds the status strip carrying both indicators. Call once; the caller
    // packs the returned widget at the bottom of the window. It does not go in
    // the header bar, which the view switcher already fills.
    GtkWidget* create_route_bar();

    // "detail" becomes the tooltip. The daemon's reason strings name the actual
    // next step, so they are passed through verbatim rather than summarised.
    void set_route_status(Route route, bool ok, const std::string& detail);

} // namespace tether::ui
