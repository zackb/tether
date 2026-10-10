#include "../src/tui/util.hpp"

#include <clocale>
#include <gtest/gtest.h>

using namespace tether::tui;

TEST(TuiLineBuffer, HoldsPartialLinesUntilComplete) {
    LineBuffer buffer;
    EXPECT_TRUE(buffer.feed("{\"a\":").empty());
    const auto lines = buffer.feed("1}\n{\"b\":2}\r\n{\"c\"");
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "{\"a\":1}");
    EXPECT_EQ(lines[1], "{\"b\":2}");
    const auto rest = buffer.feed(":3}\n");
    ASSERT_EQ(rest.size(), 1u);
    EXPECT_EQ(rest[0], "{\"c\":3}");
}

TEST(TuiKeymap, MatchesSequencesAndRetriesBrokenOnes) {
    Keymap keys;
    keys.bind({"j"}, "down");
    keys.bind({"g", "g"}, "top");
    keys.bind({"g", "t"}, "next_tab");
    keys.bind({"d", "d"}, "delete");
    keys.bind({"<C-w>", "l"}, "focus_detail");

    EXPECT_EQ(keys.feed("j"), "down");
    EXPECT_EQ(keys.feed("g"), "");
    EXPECT_TRUE(keys.pending());
    EXPECT_EQ(keys.feed("g"), "top");
    EXPECT_FALSE(keys.pending());
    EXPECT_EQ(keys.feed("d"), "");
    EXPECT_EQ(keys.feed("d"), "delete");
    EXPECT_EQ(keys.feed("<C-w>"), "");
    EXPECT_EQ(keys.feed("l"), "focus_detail");
    // A key that breaks a pending sequence still does its own thing.
    EXPECT_EQ(keys.feed("g"), "");
    EXPECT_EQ(keys.feed("j"), "down");
    EXPECT_EQ(keys.feed("x"), "");
    EXPECT_FALSE(keys.pending());
}

TEST(TuiWrap, BreaksAtWordsAndKeepsNewlines) {
    const auto lines = wrap("hello there world\nnext", 11);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], L"hello there");
    EXPECT_EQ(lines[1], L"world");
    EXPECT_EQ(lines[2], L"next");

    const auto long_word = wrap("abcdefghij", 4);
    ASSERT_EQ(long_word.size(), 3u);
    EXPECT_EQ(long_word[2], L"ij");

    EXPECT_EQ(wrap("", 10).size(), 1u);
}

TEST(TuiWrap, CountsWideCharactersAsTwoColumns) {
    std::setlocale(LC_ALL, "C.UTF-8");
    EXPECT_EQ(width_of(L"日本"), 4);
    const auto lines = wrap_chars(L"日本語", 4);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], L"日本");
    EXPECT_EQ(fit(L"abcdef", 4), L"abc…");
    EXPECT_EQ(fit(L"abc", 4), L"abc");
}

TEST(TuiLineEdit, EditsAroundTheCursor) {
    LineEdit edit;
    edit.set("send one two");
    EXPECT_TRUE(edit.key("<C-w>"));
    EXPECT_EQ(edit.text(), "send one ");
    edit.key("<Home>");
    edit.insert(L'x');
    EXPECT_EQ(edit.text(), "xsend one ");
    edit.key("<C-k>");
    EXPECT_EQ(edit.text(), "x");
    edit.key("<BS>");
    EXPECT_TRUE(edit.empty());
    EXPECT_FALSE(edit.key("q"));
    edit.set("héllo");
    EXPECT_EQ(edit.cursor(), 5u);
    EXPECT_EQ(narrow(widen("héllo")), "héllo");
}

TEST(TuiWrap, KeepsParagraphIndentation) {
    const auto lines = wrap("restart:\n    pkill tetherd", 40);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[1], L"    pkill tetherd");
}
