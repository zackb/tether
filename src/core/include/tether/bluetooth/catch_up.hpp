#pragma once

#include "tether/bluetooth/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace tether::bluetooth {

    // Above this many, the messages that arrived while the phone was away are
    // announced as one summary, so a long absence does not bury the desktop
    // in popups.
    inline constexpr size_t CATCH_UP_POPUP_MAX = 3;

    // Each message gets its own popup, oldest first so the newest ends up on top.
    struct CatchUpEach {
        std::vector<Message> messages;
    };

    // One popup for the whole batch.
    struct CatchUpSummary {
        size_t count = 0;
        // The newest message of each conversation, newest conversation first.
        std::vector<Message> threads;
    };

    using CatchUp = std::variant<CatchUpEach, CatchUpSummary>;

    // How to announce the messages a reconnect found on the phone. `missed`
    // holds only messages that would each have had a popup had they arrived
    // while connected; deciding that stays with the caller. Only those newer
    // than `newest_known` arrived while the phone was away. Without it there
    // is nothing to tell new from old, so nothing is announced: on a first
    // sync, every unread message on the phone would otherwise look new.
    CatchUp plan_catch_up(std::vector<Message> missed,
                          std::optional<int64_t> newest_known,
                          size_t popup_max = CATCH_UP_POPUP_MAX);

} // namespace tether::bluetooth
