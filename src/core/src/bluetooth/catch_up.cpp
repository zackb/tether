#include "tether/bluetooth/catch_up.hpp"

#include <algorithm>
#include <unordered_set>

namespace tether::bluetooth {

    CatchUp plan_catch_up(std::vector<Message> missed, std::optional<int64_t> newest_known, size_t popup_max) {
        if (!newest_known)
            return CatchUpEach{};
        std::erase_if(missed, [&](const Message& m) { return m.timestamp <= *newest_known; });

        std::stable_sort(
            missed.begin(), missed.end(), [](const Message& a, const Message& b) { return a.timestamp < b.timestamp; });

        if (missed.size() <= popup_max)
            return CatchUpEach{std::move(missed)};

        CatchUpSummary summary;
        summary.count = missed.size();
        std::unordered_set<std::string> seen;
        for (auto it = missed.rbegin(); it != missed.rend(); ++it)
            if (seen.insert(it->thread_key).second)
                summary.threads.push_back(*it);
        return summary;
    }

} // namespace tether::bluetooth
