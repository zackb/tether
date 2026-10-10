#include "util.hpp"

#include <glib.h>
#include <wchar.h>
#include <wctype.h>

namespace tether::tui {

    std::wstring widen(std::string_view utf8) {
        std::wstring out;
        const char* end = utf8.data() + utf8.size();
        for (const char* p = utf8.data(); p < end;) {
            const gunichar c = g_utf8_get_char_validated(p, end - p);
            if (c == static_cast<gunichar>(-1) || c == static_cast<gunichar>(-2)) {
                out += L'�';
                ++p;
                continue;
            }
            out += static_cast<wchar_t>(c);
            p = g_utf8_next_char(p);
        }
        return out;
    }

    std::string narrow(std::wstring_view text) {
        std::string out;
        char buffer[6];
        for (wchar_t c : text) {
            const int n = g_unichar_to_utf8(static_cast<gunichar>(c), buffer);
            out.append(buffer, n);
        }
        return out;
    }

    namespace {

        int char_width(wchar_t c) {
            if (c == L'\t')
                return 1;
            const int w = wcwidth(c);
            return w < 0 ? 1 : w;
        }

    } // namespace

    int width_of(std::wstring_view text) {
        int total = 0;
        for (wchar_t c : text)
            total += char_width(c);
        return total;
    }

    std::wstring fit(std::wstring_view text, int width) {
        if (width <= 0)
            return {};
        if (width_of(text) <= width)
            return std::wstring(text);
        std::wstring out;
        int used = 0;
        for (wchar_t c : text) {
            const int w = char_width(c);
            if (used + w > width - 1)
                break;
            out += c;
            used += w;
        }
        return out + L'…';
    }

    std::vector<std::wstring> wrap(std::string_view utf8, int width) {
        std::vector<std::wstring> lines;
        if (width <= 0)
            width = 1;
        const std::wstring text = widen(utf8);
        size_t start = 0;
        while (true) {
            const size_t newline = text.find(L'\n', start);
            const std::wstring paragraph =
                text.substr(start, newline == std::wstring::npos ? std::wstring::npos : newline - start);

            std::wstring line;
            int line_width = 0;
            size_t i = 0;
            const size_t lines_before = lines.size();
            while (i < paragraph.size()) {
                // Next word, and the spaces in front of it.
                size_t word_start = i;
                while (word_start < paragraph.size() && iswspace(paragraph[word_start]))
                    ++word_start;
                size_t word_end = word_start;
                while (word_end < paragraph.size() && !iswspace(paragraph[word_end]))
                    ++word_end;
                const std::wstring gap = paragraph.substr(i, word_start - i);
                std::wstring word = paragraph.substr(word_start, word_end - word_start);
                i = word_end;
                if (word.empty())
                    break;

                // Indentation at the start of a paragraph is kept; gaps a wrap lands on are not.
                const bool indent = word_start == gap.size() && lines_before == lines.size();
                const int gap_width = line.empty() && !indent ? 0 : width_of(gap);
                const int word_width = width_of(word);
                if (line_width + gap_width + word_width <= width) {
                    if (!line.empty() || indent)
                        line += gap;
                    line += word;
                    line_width += gap_width + word_width;
                    continue;
                }
                if (!line.empty()) {
                    lines.push_back(line);
                    line.clear();
                    line_width = 0;
                }
                // Too wide for any line: break it wherever the edge falls.
                for (wchar_t c : word) {
                    const int w = char_width(c);
                    if (line_width + w > width && !line.empty()) {
                        lines.push_back(line);
                        line.clear();
                        line_width = 0;
                    }
                    line += c;
                    line_width += w;
                }
            }
            lines.push_back(line);
            if (newline == std::wstring::npos)
                break;
            start = newline + 1;
        }
        return lines;
    }

    std::vector<std::wstring> wrap_chars(std::wstring_view text, int width) {
        if (width <= 0)
            width = 1;
        std::vector<std::wstring> lines(1);
        int used = 0;
        for (wchar_t c : text) {
            if (c == L'\n') {
                lines.emplace_back();
                used = 0;
                continue;
            }
            const int w = char_width(c);
            if (used + w > width) {
                lines.emplace_back();
                used = 0;
            }
            lines.back() += c;
            used += w;
        }
        return lines;
    }

    std::vector<std::string> LineBuffer::feed(std::string_view bytes) {
        std::vector<std::string> lines;
        partial_.append(bytes);
        size_t start = 0;
        while (true) {
            const size_t newline = partial_.find('\n', start);
            if (newline == std::string::npos)
                break;
            std::string line = partial_.substr(start, newline - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(std::move(line));
            start = newline + 1;
        }
        partial_.erase(0, start);
        return lines;
    }

    void Keymap::bind(std::vector<std::string> keys, std::string action) { bindings_[std::move(keys)] = std::move(action); }

    bool Keymap::is_prefix(const std::vector<std::string>& keys) const {
        for (auto it = bindings_.lower_bound(keys); it != bindings_.end(); ++it) {
            if (it->first.size() < keys.size() || !std::equal(keys.begin(), keys.end(), it->first.begin()))
                return false;
            if (it->first.size() > keys.size())
                return true;
        }
        return false;
    }

    std::string Keymap::feed(const std::string& key) {
        pending_.push_back(key);
        if (const auto hit = bindings_.find(pending_); hit != bindings_.end() && !is_prefix(pending_)) {
            pending_.clear();
            return hit->second;
        }
        if (is_prefix(pending_))
            return "";
        const bool retry = pending_.size() > 1;
        pending_.clear();
        return retry ? feed(key) : "";
    }

    void LineEdit::set(const std::string& utf8) {
        text_ = widen(utf8);
        cursor_ = text_.size();
    }

    void LineEdit::insert(wchar_t c) { text_.insert(cursor_++, 1, c); }

    bool LineEdit::key(const std::string& token) {
        if (token == "<Left>" || token == "<C-b>") {
            if (cursor_ > 0)
                --cursor_;
        } else if (token == "<Right>" || token == "<C-f>") {
            if (cursor_ < text_.size())
                ++cursor_;
        } else if (token == "<Home>" || token == "<C-a>") {
            cursor_ = 0;
        } else if (token == "<End>" || token == "<C-e>") {
            cursor_ = text_.size();
        } else if (token == "<BS>" || token == "<C-h>") {
            if (cursor_ > 0)
                text_.erase(--cursor_, 1);
        } else if (token == "<Del>") {
            if (cursor_ < text_.size())
                text_.erase(cursor_, 1);
        } else if (token == "<C-w>") {
            size_t start = cursor_;
            while (start > 0 && iswspace(text_[start - 1]))
                --start;
            while (start > 0 && !iswspace(text_[start - 1]))
                --start;
            text_.erase(start, cursor_ - start);
            cursor_ = start;
        } else if (token == "<C-u>") {
            text_.erase(0, cursor_);
            cursor_ = 0;
        } else if (token == "<C-k>") {
            text_.erase(cursor_);
        } else {
            return false;
        }
        return true;
    }

} // namespace tether::tui
