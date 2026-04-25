#pragma once

#include <format>
#include <iostream>
#include <filesystem>

template <>
struct std::formatter<std::filesystem::path> : std::formatter<std::string> {
    auto format(const std::filesystem::path& p, std::format_context& ctx) const {
        return std::formatter<std::string>::format(p.string(), ctx);
    }
};

enum LogLevel { TRACE = 0, DEBUG, INFO, WARN, ERR, CRIT };

namespace debug {
    inline bool verbose = false;
    inline bool quiet = false;
    template <typename... Args> void log(LogLevel level, const std::string& fmt, Args&&... args) {

        if (quiet && level < ERR) {
            return;
        }
        if (!verbose && level == TRACE) {
            return;
        }
        std::cerr << '[';

        switch (level) {
        case TRACE:
            std::cerr << "TRACE";
            break;
        case DEBUG:
            std::cerr << "DEBUG";
            break;
        case INFO:
            std::cerr << "INFO";
            break;
        case WARN:
            std::cerr << "WARN";
            break;
        case ERR:
            std::cerr << "ERR";
            break;
        case CRIT:
            std::cerr << "CRIT";
            break;
        default:
            break;
        }

        std::cerr << "] ";

        std::cerr << std::vformat(fmt, std::make_format_args(args...)) << std::endl;
    }
} // namespace debug
