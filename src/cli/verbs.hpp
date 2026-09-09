#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace tether::cli {

    struct Verb {
        const char* verb;
        const char* flag;  // what it stands for, and where its help text comes from
        const char* usage; // how it reads on the command line
    };

    // Subcommands, for people who reach for 'tether status' before 'tether
    // --status'. Every flag keeps working exactly as it did.
    extern const Verb kVerbs[];
    extern const size_t kVerbCount;

    // Rewrites a leading subcommand into the flag it stands for, and 'bt <name>'
    // into '--bt-<name>'. Anything else, flags included, is passed through
    // untouched. args[0] is the program name.
    std::vector<std::string> expand_verbs(const std::vector<std::string>& args);

} // namespace tether::cli
