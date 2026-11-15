#include "message_popups.hpp"

#include <algorithm>
#include <mutex>
#include <tether/bluetooth/ancs/notifications.hpp>
#include <tether/bluetooth/ancs/protocol.hpp>
#include <tether/bluetooth/bmessage.hpp>
#include <tether/bluetooth/catch_up.hpp>
#include <tether/bluetooth/config.hpp>
#include <tether/bluetooth/connection.hpp>
#include <tether/bluetooth/contacts.hpp>
#include <tether/bluetooth/removed_threads.hpp>
#include <tether/i18n.hpp>
#include <tether/net.hpp>

namespace tether {

    namespace {

        // Conversations named in a summary before the rest are only counted.
        constexpr size_t SUMMARY_NAMES_MAX = 3;

        std::vector<std::string> messages_icons() {
            bluetooth::ancs::Notification as_notification;
            as_notification.app_id = bluetooth::ancs::APP_ID_MESSAGES;
            return bluetooth::ancs::icon_candidates(as_notification);
        }

        std::string sender_label(const bluetooth::Message& message) {
            std::string who;
            {
                std::lock_guard<std::mutex> lock(bluetooth::message_store_mutex());
                who = bluetooth::contact_store().name_for(message.thread_key);
            }
            if (who.empty())
                who = message.peer_name.empty() ? message.peer_address : message.peer_name;
            return who.empty() ? "iPhone" : who;
        }

        // "Mom, Alex and Sam", or "Mom, Alex, Sam and 2 others" past the cap.
        std::string summary_body(const std::vector<bluetooth::Message>& threads) {
            std::vector<std::string> names;
            for (size_t i = 0; i < threads.size() && i < SUMMARY_NAMES_MAX; ++i)
                names.push_back(sender_label(threads[i]));

            std::string listed;
            const bool has_others = threads.size() > names.size();
            const size_t joined = has_others ? names.size() : names.size() - 1;
            for (size_t i = 0; i < joined; ++i)
                listed += (i ? ", " : "") + names[i];

            if (has_others) {
                const size_t others = threads.size() - names.size();
                return tr_format(P_("{0} and {1} other", "{0} and {1} others", others), listed, others);
            }
            if (listed.empty())
                return names.front();
            return tr_format(_("{0} and {1}"), listed, names.back());
        }

        std::optional<NotificationSpec>
            popup_for(const bluetooth::Message& message, const std::string& otp, bool hide_unknown) {
            if (message.outgoing || message.read)
                return std::nullopt;

            bool unknown = false;
            bool hidden = false;
            {
                std::lock_guard<std::mutex> lock(bluetooth::message_store_mutex());
                const auto& removed = bluetooth::removed_threads();
                hidden = removed.hides(message);
                if (hide_unknown) {
                    const auto thread = bluetooth::message_store().messages(
                        message.thread_key, 200, [&removed](const auto& m) { return !removed.hides(m); });
                    const bool replied =
                        std::any_of(thread.begin(), thread.end(), [](const auto& m) { return m.outgoing; });
                    unknown = bluetooth::contact_store().is_unknown_sender(message.thread_key, replied);
                }
            }
            // hidden sender still pops up with a code,
            if (unknown && otp.empty() && !bluetooth::is_short_code(message.thread_key))
                return std::nullopt;
            if (hidden && otp.empty())
                return std::nullopt;

            // The reply action is only offered for a thread that has a route back.
            bluetooth::Recipient recipient;
            std::string reply_error;
            const bool repliable =
                !unknown && bluetooth::recipient_from_thread_key(message.thread_key, recipient, reply_error);
            return NotificationSpec{_("Messages"),
                                    sender_label(message),
                                    popup_previews_enabled() ? message.body : std::string{},
                                    messages_icons(),
                                    false,
                                    repliable ? message.thread_key : std::string{},
                                    otp,
                                    "",
                                    message.handle};
        }

    } // namespace

    std::optional<NotificationSpec> message_popup(const bluetooth::Message& message, const std::string& otp) {
        return popup_for(message, otp, bluetooth::load_config().hide_unknown_senders);
    }

    void announce_missed_messages(DesktopNotifier& notifier,
                                  const std::vector<bluetooth::Message>& messages,
                                  std::optional<int64_t> newest_known) {
        const auto config = bluetooth::load_config();
        if (!config.missed_message_popups)
            return;

        // A code that arrived while the phone was away has most likely expired,
        // so it neither lets a hidden thread through nor offers a copy action.
        const std::string no_code;
        std::vector<bluetooth::Message> missed;
        for (const auto& message : messages)
            if (popup_for(message, no_code, config.hide_unknown_senders))
                missed.push_back(message);

        const auto plan = bluetooth::plan_catch_up(std::move(missed), newest_known);
        if (const auto* each = std::get_if<bluetooth::CatchUpEach>(&plan)) {
            for (const auto& message : each->messages)
                if (auto popup = popup_for(message, no_code, config.hide_unknown_senders))
                    notifier.notify(*popup);
            return;
        }

        const auto& summary = std::get<bluetooth::CatchUpSummary>(plan);
        notifier.notify({_("Messages"),
                         tr_format(P_("{} new message", "{} new messages", summary.count), summary.count),
                         summary_body(summary.threads),
                         messages_icons(),
                         false,
                         ""});
    }

} // namespace tether
