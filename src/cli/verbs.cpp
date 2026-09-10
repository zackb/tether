#include "verbs.hpp"

namespace tether::cli {

    const Verb kVerbs[] = {
        {"status", "--status", "tether status"},
        {"devices", "--list-devices", "tether devices"},
        {"pending", "--pending", "tether pending"},
        {"accept", "--accept", "tether accept <fingerprint>"},
        {"forget", "--forget", "tether forget <fingerprint>"},
        {"pair", "--pair", "tether pair --host <ip>"},
        {"discover", "--discover", "tether discover"},
        {"send", "--send-file", "tether send <path>"},
        {"copy", "--set-clipboard", "tether copy [text]"},
        {"paste", "--get-clipboard", "tether paste"},
        {"service", "--install-service", "tether service"},
        {"version", "--version", "tether version"},
        {"help", "--help", "tether help"},
    };

    const size_t kVerbCount = sizeof(kVerbs) / sizeof(kVerbs[0]);

    std::vector<std::string> expand_verbs(const std::vector<std::string>& args) {
        if (args.size() < 2 || args[1].empty() || args[1][0] == '-')
            return args;

        std::vector<std::string> out;
        out.reserve(args.size());
        out.push_back(args[0]);

        // 'tether bt <name> ...' is '--bt-<name> ...', so every Bluetooth flag
        // has a subcommand without a table to keep in step with it.
        if (args[1] == "bt") {
            if (args.size() < 3 || args[2].empty() || args[2][0] == '-')
                return args;
            out.push_back("--bt-" + args[2]);
            out.insert(out.end(), args.begin() + 3, args.end());
            return out;
        }

        for (size_t i = 0; i < kVerbCount; ++i) {
            if (args[1] != kVerbs[i].verb)
                continue;
            out.push_back(kVerbs[i].flag);
            out.insert(out.end(), args.begin() + 2, args.end());
            return out;
        }

        return args;
    }

} // namespace tether::cli
