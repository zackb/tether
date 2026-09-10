#include "../src/cli/verbs.hpp"

#include <gtest/gtest.h>

namespace {

    std::vector<std::string> expand(std::initializer_list<const char*> args) {
        std::vector<std::string> in(args.begin(), args.end());
        return tether::cli::expand_verbs(in);
    }

} // namespace

TEST(Verbs, RewritesASubcommandIntoItsFlag) {
    EXPECT_EQ(expand({"tether", "devices"}), (std::vector<std::string>{"tether", "--list-devices"}));
    EXPECT_EQ(expand({"tether", "paste"}), (std::vector<std::string>{"tether", "--get-clipboard"}));
    EXPECT_EQ(expand({"tether", "discover"}), (std::vector<std::string>{"tether", "--discover"}));
}

TEST(Verbs, KeepsTheArgumentsThatFollow) {
    EXPECT_EQ(expand({"tether", "accept", "9a4f21"}), (std::vector<std::string>{"tether", "--accept", "9a4f21"}));
    EXPECT_EQ(expand({"tether", "paste", "--host", "10.0.0.2"}),
              (std::vector<std::string>{"tether", "--get-clipboard", "--host", "10.0.0.2"}));
}

TEST(Verbs, MapsBtToTheMatchingBluetoothFlag) {
    EXPECT_EQ(expand({"tether", "bt", "setup"}), (std::vector<std::string>{"tether", "--bt-setup"}));
    EXPECT_EQ(expand({"tether", "bt", "pair", "AA:BB"}), (std::vector<std::string>{"tether", "--bt-pair", "AA:BB"}));
    EXPECT_EQ(expand({"tether", "bt", "airpods-mode", "anc"}),
              (std::vector<std::string>{"tether", "--bt-airpods-mode", "anc"}));
}

TEST(Verbs, LeavesFlagsAlone) {
    const auto flags = expand({"tether", "--bt-send", "thread", "hello"});
    EXPECT_EQ(flags, (std::vector<std::string>{"tether", "--bt-send", "thread", "hello"}));

    // Value of a flag, not a subcommand: only the first argument is a verb.
    EXPECT_EQ(expand({"tether", "-s", "paste"}), (std::vector<std::string>{"tether", "-s", "paste"}));
}

TEST(Verbs, LeavesSomethingItDoesNotKnowForTheParserToReject) {
    EXPECT_EQ(expand({"tether", "frobnicate"}), (std::vector<std::string>{"tether", "frobnicate"}));
    EXPECT_EQ(expand({"tether", "bt"}), (std::vector<std::string>{"tether", "bt"}));
    EXPECT_EQ(expand({"tether", "bt", "--help"}), (std::vector<std::string>{"tether", "bt", "--help"}));
    EXPECT_EQ(expand({"tether"}), (std::vector<std::string>{"tether"}));
}

TEST(Verbs, EveryVerbHasAFlagAndHelp) {
    for (size_t i = 0; i < tether::cli::kVerbCount; ++i) {
        const auto& verb = tether::cli::kVerbs[i];
        EXPECT_NE(verb.verb[0], '-') << verb.verb;
        EXPECT_EQ(verb.flag[0], '-') << verb.verb;
        EXPECT_NE(verb.usage[0], '\0') << verb.verb;
    }
}
