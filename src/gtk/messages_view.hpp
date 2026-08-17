#pragma once

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>

namespace tether::ui {

    // Conversations from the iPhone over MAP: a thread list beside the selected
    // conversation, with a composer that stays disabled until sending exists.
    GtkWidget* messages_view_new();

    // Returns true when the event belonged to this view.
    bool messages_view_handle_event(const nlohmann::json& event);

    // Threads are only pulled while someone is looking at them, so the view has
    // to be told when it comes and goes.
    void messages_view_set_visible(bool visible);

} // namespace tether::ui
