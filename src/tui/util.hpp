#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

// Terminal-independent pieces of the TUI, kept apart so they can be tested.
namespace tether::tui {

    std::wstring widen(std::string_view utf8);
    std::string narrow(std::wstring_view text);

    // Terminal columns a string occupies.
    int width_of(std::wstring_view text);

    // Cut to at most `width` columns, ending in "…" when something was dropped.
    std::wstring fit(std::wstring_view text, int width);

    // Word-wrapped to `width` columns. Explicit newlines are kept; a word wider
    // than the line is broken mid-word.
    std::vector<std::wstring> wrap(std::string_view utf8, int width);

    // Same, but always breaks at exactly `width`, so a prefix wraps the same way
    // as the whole text. Used where a cursor has to be placed.
    std::vector<std::wstring> wrap_chars(std::wstring_view text, int width);

    // Splits a byte stream into newline-terminated lines. A trailing partial line
    // is held until the rest of it arrives.
    class LineBuffer {
    public:
        std::vector<std::string> feed(std::string_view bytes);
        void clear() { partial_.clear(); }

    private:
        std::string partial_;
    };

    // Matches multi-key sequences ("gg", "dd", "<C-w>l") as keys arrive. Keys are
    // tokens: a printable character, or a name such as "<CR>", "<Esc>", "<C-d>".
    class Keymap {
    public:
        void bind(std::vector<std::string> keys, std::string action);

        // The action a key completes, or empty while a sequence is still pending
        // or nothing matched. A key that breaks a pending sequence is tried again
        // on its own, so "g" then "j" still moves down.
        std::string feed(const std::string& key);
        bool pending() const { return !pending_.empty(); }
        void reset() { pending_.clear(); }

    private:
        bool is_prefix(const std::vector<std::string>& keys) const;

        std::map<std::vector<std::string>, std::string> bindings_;
        std::vector<std::string> pending_;
    };

    // A single editable buffer with a cursor, for the composer and prompts.
    class LineEdit {
    public:
        // Whether the key was an editing key.
        bool key(const std::string& token);
        void insert(wchar_t c);
        void set(const std::string& utf8);
        void clear() { set(""); }
        std::string text() const { return narrow(text_); }
        const std::wstring& wide() const { return text_; }
        size_t cursor() const { return cursor_; }
        bool empty() const { return text_.empty(); }

    private:
        std::wstring text_;
        size_t cursor_ = 0;
    };

} // namespace tether::tui
