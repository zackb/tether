#include "ui_text.hpp"

#include <ctime>
#include <glib.h>
#include <set>
#include <tether/bluetooth/bmessage.hpp>
#include <tether/i18n.hpp>

namespace tether::ui {

    std::string fold(const std::string& text) {
        gchar* normalized = g_utf8_normalize(text.c_str(), -1, G_NORMALIZE_ALL);
        gchar* folded = g_utf8_casefold(normalized ? normalized : text.c_str(), -1);
        std::string out = folded ? folded : "";
        g_free(normalized);
        g_free(folded);
        return out;
    }

    std::string initials(const std::string& name) {
        if (!g_utf8_validate(name.c_str(), -1, nullptr))
            return "";
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
        const std::string joined = first + last;
        gchar* upper = g_utf8_strup(joined.c_str(), -1);
        std::string out = upper ? upper : "";
        g_free(upper);
        return out;
    }

    std::string json_string(const nlohmann::json& j, const char* key, const std::string& fallback) {
        const auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
    }

    std::string airpods_status_text(const nlohmann::json& airpods, bool spoken) {
        std::string text;
        const auto append = [&](const char* label, int level) {
            if (level < 0)
                return;
            if (!text.empty())
                text += spoken ? ", " : "   ";
            text += label + (" " + std::to_string(level) + "%");
        };
        // TRANSLATORS: Left earbud, read aloud by screen readers before its battery level.
        const char* left_spoken = _("Left earbud");
        // TRANSLATORS: Left earbud, abbreviated to fit the device row. One or two letters.
        append(spoken ? left_spoken : _("L"), airpods.value("left", -1));
        // TRANSLATORS: Right earbud, read aloud by screen readers before its battery level.
        const char* right_spoken = _("Right earbud");
        // TRANSLATORS: Right earbud, abbreviated to fit the device row. One or two letters.
        append(spoken ? right_spoken : _("R"), airpods.value("right", -1));
        // TRANSLATORS: The AirPods charging case, on the device row beside the two earbuds.
        append(_("Case"), airpods.value("case", -1));
        if (!text.empty())
            return text;

        const std::string status = json_string(airpods, "status");
        if (status == "busy" || status == "failed")
            return json_string(airpods, "reason");
        return "";
    }

    std::string connection_reason(const nlohmann::json& connection) {
        const bool link_degraded =
            !connection.value("classic_connected", false) || !connection.value("le_connected", false);
        std::string reason = connection.value(link_degraded ? "link_reason" : "profile_reason", "");
        if (reason.empty())
            reason = connection.value("link_reason", "");
        return reason;
    }

    const char* call_state_text(const std::string& state) {
        if (state == "incoming")
            return _("Incoming");
        if (state == "waiting")
            return _("Call waiting");
        if (state == "dialing")
            return _("Dialing");
        if (state == "alerting")
            return _("Ringing");
        if (state == "active")
            return _("On call");
        if (state == "held")
            return _("On hold");
        if (state == "disconnected")
            return _("Ended");
        return "";
    }

    std::string network_text(const nlohmann::json& calls, bool spoken) {
        if (!calls.is_object())
            return {};
        if (!calls.value("indicators", true))
            return {};
        const std::string gap = spoken ? ", " : "  ";
        std::string out = calls.value("operator", "");
        if (!calls.value("service", false))
            out = out.empty() ? _("No service") : out + (spoken ? gap : "  -  ") + _("No service");
        const int signal = calls.value("signal", 0);
        if (calls.value("service", false)) {
            std::string bars;
            if (spoken) {
                // TRANSLATORS: Cellular signal strength read aloud, {} is 0 to 5.
                bars = tether::tr_format(_("signal {} of 5"), signal);
            } else {
                for (int i = 0; i < 5; ++i)
                    bars += i < signal ? "▆" : "▁";
            }
            out += out.empty() ? bars : gap + bars;
        }
        if (calls.value("roaming", false))
            out += gap + _("roaming");
        if (const int battery = calls.value("battery", 0); battery > 0) {
            const std::string level = std::to_string(battery * 20) + "%";
            // TRANSLATORS: The iPhone's battery level read aloud, {} is like "80%".
            out += gap + (spoken ? tether::tr_format(_("battery {}"), level) : level);
        }
        return out;
    }

    std::string format_call_time(int64_t epoch) {
        if (epoch <= 0)
            return "";
        const std::time_t t = static_cast<std::time_t>(epoch);
        const std::time_t now = std::time(nullptr);
        std::tm tm{}, today{};
        localtime_r(&t, &tm);
        localtime_r(&now, &today);
        const bool same_day = tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday;

        char buffer[64];
        // xgettext:no-c-format
        std::strftime(buffer, sizeof(buffer), same_day ? _("%H:%M") : _("%b %d, %H:%M"), &tm);
        return buffer;
    }

    std::string format_clock(int64_t epoch) {
        if (epoch <= 0)
            return "";
        std::time_t t = static_cast<std::time_t>(epoch);
        std::tm tm{};
        localtime_r(&t, &tm);

        char buffer[64];
        // xgettext:no-c-format
        std::strftime(buffer, sizeof(buffer), _("%H:%M"), &tm);
        return buffer;
    }

    std::string display_address(const std::string& key) {
        const size_t colon = key.find(':');
        return colon == std::string::npos ? key : key.substr(colon + 1);
    }

    std::vector<ContactEntry> contact_entries(const nlohmann::json& contacts,
                                              std::map<std::string, std::string>& names) {
        std::vector<ContactEntry> out;
        names.clear();
        if (!contacts.is_array())
            return out;

        std::set<std::string> seen;
        for (const auto& card : contacts) {
            const std::string name = card.value("name", "");
            if (!card.contains("addresses") || !card["addresses"].is_array())
                continue;
            for (const auto& entry : card["addresses"]) {
                if (!entry.is_string())
                    continue;

                bluetooth::Recipient recipient;
                std::string err;
                if (!bluetooth::recipient_from_thread_key(entry.get<std::string>(), recipient, err))
                    continue;
                const std::string key = bluetooth::thread_key_for(recipient);
                if (key.empty() || !seen.insert(key).second)
                    continue;
                if (!name.empty())
                    names.emplace(key, name);

                ContactEntry contact;
                contact.display = name.empty() ? recipient.address : name + " · " + recipient.address;
                contact.address = recipient.address;
                // The normalized form is in the haystack too, so "5551234567"
                // finds a contact whose number is stored as "+1 (555) 123-4567".
                contact.search = fold(name + " " + recipient.address + " " + key.substr(key.find(':') + 1));
                contact.is_tel = recipient.kind == bluetooth::RecipientKind::Tel;
                out.push_back(std::move(contact));
            }
        }
        return out;
    }

} // namespace tether::ui
