#include "tether/bluetooth/vcard.hpp"

#include <algorithm>
#include <cctype>

namespace tether::bluetooth {

    namespace {

        std::string trim(const std::string& text) {
            size_t begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return {};
            size_t end = text.find_last_not_of(" \t\r\n");
            return text.substr(begin, end - begin + 1);
        }

        std::string upper(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            return text;
        }

    } // namespace

    std::vector<std::string> vcard_unfold(const std::string& text) {
        std::vector<std::string> lines;
        size_t start = 0;
        while (start <= text.size()) {
            size_t nl = text.find('\n', start);
            std::string line = nl == std::string::npos ? text.substr(start) : text.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            // A leading space or tab means this line continues the previous one.
            if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && !lines.empty())
                lines.back() += line.substr(1);
            else
                lines.push_back(std::move(line));

            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
        return lines;
    }

    bool vcard_split_property(const std::string& line, std::string& name, std::string& value) {
        size_t colon = line.find(':');
        if (colon == std::string::npos)
            return false;
        std::string left = line.substr(0, colon);
        size_t semi = left.find(';');
        name = upper(trim(semi == std::string::npos ? left : left.substr(0, semi)));
        value = trim(line.substr(colon + 1));
        return !name.empty();
    }

    std::string vcard_name_from_n(const std::string& value) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            size_t semi = value.find(';', start);
            parts.push_back(value.substr(start, semi == std::string::npos ? std::string::npos : semi - start));
            if (semi == std::string::npos)
                break;
            start = semi + 1;
        }
        std::string given = parts.size() > 1 ? trim(parts[1]) : std::string{};
        std::string family = parts.empty() ? std::string{} : trim(parts[0]);
        if (!given.empty() && !family.empty())
            return given + " " + family;
        return given.empty() ? family : given;
    }

    std::string vcard_unescape(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] != '\\' || i + 1 >= value.size()) {
                out += value[i];
                continue;
            }
            switch (value[i + 1]) {
            case 'n':
            case 'N':
                out += '\n';
                break;
            case ',':
                out += ',';
                break;
            case ';':
                out += ';';
                break;
            case '\\':
                out += '\\';
                break;
            default:
                // Not an escape we know; keep both characters verbatim.
                out += value[i];
                out += value[i + 1];
                break;
            }
            ++i;
        }
        return out;
    }

    std::vector<VCard> parse_vcards(const std::string& text) {
        std::vector<VCard> cards;
        if (text.empty())
            return cards;

        VCard current;
        std::string structured_name;
        bool in_card = false;

        auto flush = [&] {
            if (current.name.empty())
                current.name = structured_name;
            if (!current.empty())
                cards.push_back(current);
            current = VCard{};
            structured_name.clear();
        };

        for (const auto& raw : vcard_unfold(text)) {
            const std::string line = trim(raw);
            const std::string token = upper(line);

            if (token == "BEGIN:VCARD") {
                // An unterminated card still yields what it had, rather than
                // being silently merged into the next one.
                if (in_card)
                    flush();
                in_card = true;
                continue;
            }
            if (token == "END:VCARD") {
                if (in_card)
                    flush();
                in_card = false;
                continue;
            }
            if (!in_card)
                continue;

            std::string name, value;
            if (!vcard_split_property(line, name, value) || value.empty())
                continue;

            if (name == "TEL")
                current.tels.push_back(vcard_unescape(value));
            else if (name == "EMAIL")
                current.emails.push_back(vcard_unescape(value));
            else if (name == "FN")
                current.name = vcard_unescape(value);
            else if (name == "N")
                structured_name = vcard_name_from_n(vcard_unescape(value));
        }

        if (in_card)
            flush();
        return cards;
    }

} // namespace tether::bluetooth
