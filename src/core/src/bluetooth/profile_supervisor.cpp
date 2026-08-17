#include "tether/bluetooth/profile_supervisor.hpp"
#include "tether/log.hpp"

#include <algorithm>
#include <cctype>

namespace tether::bluetooth {

    namespace {

        std::string lowered(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return text;
        }

        bool contains(const std::string& haystack, const char* needle) {
            return haystack.find(needle) != std::string::npos;
        }

    } // namespace

    const char* to_string(ObexError error) {
        switch (error) {
        case ObexError::None:
            return "none";
        case ObexError::Forbidden:
            return "forbidden";
        case ObexError::Busy:
            return "busy";
        case ObexError::NoRecord:
            return "no_record";
        case ObexError::Unavailable:
            return "unavailable";
        default:
            return "other";
        }
    }

    ObexError classify_obex_error(const std::string& message) {
        if (message.empty())
            return ObexError::None;

        const std::string text = lowered(message);

        // permission toggle is off on the phone
        if (contains(text, "forbidden") || contains(text, "0x43"))
            return ObexError::Forbidden;

        if (contains(text, "connection refused") || contains(text, "(111)"))
            return ObexError::Busy;

        if (contains(text, "unable to find service record") || contains(text, "no such service"))
            return ObexError::NoRecord;

        if (contains(text, "host is down") || contains(text, "no route to host") || contains(text, "not connected") ||
            contains(text, "page timeout"))
            return ObexError::Unavailable;

        return ObexError::Other;
    }

    std::string obex_error_advice(ObexError error, const std::string& profile) {
        const std::string label = profile == "map" ? "Messages" : "Contacts";
        switch (error) {
        case ObexError::Forbidden:
            return "Enable \"" +
                   (profile == "map" ? std::string("Show Message Notifications") : std::string("Sync Contacts")) +
                   "\" for this computer in the iPhone's Settings > Bluetooth > (i). "
                   "This is a permission, not a pairing problem.";
        case ObexError::Busy:
            return label + " is unavailable because another computer is using the iPhone's " + profile +
                   " session. Disconnect it there, then this will connect on its own.";
        case ObexError::NoRecord:
            // obexd reports a missing record whenever its SDP fetch is refused, so the real cause is usually one layer
            // down.
            return label +
                   " could not be fetched from the iPhone. Usually another computer holds the phone's "
                   "Bluetooth session, or \"" +
                   (profile == "map" ? std::string("Show Message Notifications") : std::string("Sync Contacts")) +
                   "\" is off in Settings > Bluetooth > (i). Check both; re-pairing is not the fix.";
        case ObexError::Unavailable:
            return label + " is unreachable; waiting for the iPhone to come back.";
        case ObexError::Other:
            return label + " could not be opened.";
        default:
            return {};
        }
    }

    ProfileSupervisor::ProfileSupervisor(ProfileOps& ops) : ops_(ops) {}

    void ProfileSupervisor::reset() {
        if (!map_path_.empty())
            ops_.remove_session(map_path_);
        if (!pbap_path_.empty())
            ops_.remove_session(pbap_path_);
        map_path_.clear();
        pbap_path_.clear();
        map_retried_ = false;
        pbap_retried_ = false;
        next_attempt_ = 0;
        status_.map_open = false;
        status_.pbap_open = false;
    }

    bool
        ProfileSupervisor::open_profile(const std::string& target, std::string& path, ObexError& error, bool& retried) {
        std::string err;
        path = ops_.create_session(target, err);
        if (!path.empty()) {
            error = ObexError::None;
            retried = false;
            return true;
        }

        error = classify_obex_error(err);
        debug::log(WARN, "bluetooth: {} session failed ({}): {}", target, to_string(error), err);

        // A stale session from a previous run can present as Forbidden. Drop only
        // this profile's session and try once more, rather than restarting obexd.
        if (error == ObexError::Forbidden && !retried) {
            retried = true;
            ops_.remove_session(path);
            path = ops_.create_session(target, err);
            if (!path.empty()) {
                error = ObexError::None;
                return true;
            }
            error = classify_obex_error(err);
        }
        return false;
    }

    bool ProfileSupervisor::tick(int64_t now, bool link_ready) {
        ProfileStatus previous = status_;

        if (!link_ready) {
            // Nothing to do until the Classic bearer is up; hold what we have and
            // let reset() handle an actual disconnect.
            status_.reason = "Waiting for the Bluetooth link.";
            return !(status_ == previous);
        }

        if (status_.map_open && status_.pbap_open) {
            status_.reason = "Messages and contacts are connected.";
            return !(status_ == previous);
        }

        if (now < next_attempt_)
            return !(status_ == previous);

        if (!status_.map_open)
            status_.map_open = open_profile("map", map_path_, status_.map_error, map_retried_);
        if (!status_.pbap_open)
            status_.pbap_open = open_profile("pbap", pbap_path_, status_.pbap_error, pbap_retried_);

        if (status_.map_open || status_.pbap_open)
            opened_once_ = true;
        next_attempt_ = now + (opened_once_ ? PROFILE_STEADY_POLL_SECONDS : PROFILE_INITIAL_POLL_SECONDS);

        if (status_.map_open && status_.pbap_open) {
            status_.reason = "Messages and contacts are connected.";
        } else if (!status_.map_open && status_.map_error != ObexError::None) {
            // MAP is the feature people notice, so its problem leads.
            status_.reason = obex_error_advice(status_.map_error, "map");
        } else if (!status_.pbap_open && status_.pbap_error != ObexError::None) {
            status_.reason = obex_error_advice(status_.pbap_error, "pbap");
        } else {
            status_.reason = "Opening messages and contacts...";
        }

        return !(status_ == previous);
    }

} // namespace tether::bluetooth
