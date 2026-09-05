#include "prefs.hpp"

#include <filesystem>
#include <fstream>
#include <glib.h>
#include <tether/log.hpp>
#include <tether/paths.hpp>

namespace tether::ui {

    namespace {

        std::string prefs_path() {
            const std::filesystem::path dir = tether::paths::config_dir();
            if (dir.empty())
                return {};
            return (dir / "gtk.json").string();
        }

    } // namespace

    nlohmann::json& prefs() {
        static nlohmann::json loaded = [] {
            const std::string path = prefs_path();
            if (!path.empty()) {
                try {
                    std::ifstream in(path);
                    if (in) {
                        nlohmann::json parsed = nlohmann::json::parse(in);
                        if (parsed.is_object())
                            return parsed;
                    }
                } catch (const std::exception& e) {
                    debug::log(WARN, "prefs: ignoring {} ({})", path, e.what());
                }
            }
            return nlohmann::json::object();
        }();
        return loaded;
    }

    void prefs_save() {
        const std::string path = prefs_path();
        if (path.empty())
            return;
        try {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            std::ofstream out(path);
            out << prefs().dump(2) << "\n";
        } catch (const std::exception& e) {
            debug::log(ERR, "prefs: could not write {} ({})", path, e.what());
        }
    }

} // namespace tether::ui
