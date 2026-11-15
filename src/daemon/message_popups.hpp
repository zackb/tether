#pragma once

#include "notification.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <tether/bluetooth/messages.hpp>
#include <vector>

namespace tether {

    // The desktop popup an incoming MAP message earns, or nothing when it is
    // outgoing, already read, or from a thread the user has hidden. `otp` is the
    // code found in it, which lets a popup through that would otherwise be held back.
    std::optional<NotificationSpec> message_popup(const bluetooth::Message& message, const std::string& otp);

    // Announces what a reconnect found on the phone that is still unread there:
    // a popup each for a few, one summary for more.
    void announce_missed_messages(DesktopNotifier& notifier,
                                  const std::vector<bluetooth::Message>& messages,
                                  std::optional<int64_t> newest_known);

} // namespace tether
