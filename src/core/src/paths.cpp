#include "tether/paths.hpp"

#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

namespace tether::paths {

    namespace {

        std::filesystem::path home_dir() {
            if (const char* home = getenv("HOME"); home && *home)
                return home;
            if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir)
                return pw->pw_dir;
            return {};
        }

        bool dir_exists(const std::filesystem::path& path) {
            std::error_code ec;
            return std::filesystem::exists(path, ec);
        }

        std::filesystem::path xdg_dir(const char* variable, const char* fallback) {
            const std::filesystem::path home = home_dir();
            const std::filesystem::path legacy = home.empty() ? std::filesystem::path{} : home / fallback / "tether";

            const char* value = getenv(variable);
            // A relative XDG value is invalid and ignored.
            if (!value || !*value || !std::filesystem::path(value).is_absolute())
                return legacy;

            const std::filesystem::path dir = std::filesystem::path(value) / "tether";
            if (!legacy.empty() && !dir_exists(dir) && dir_exists(legacy))
                return legacy;
            return dir;
        }

    } // namespace

    std::filesystem::path config_dir() { return xdg_dir("XDG_CONFIG_HOME", ".config"); }

    std::filesystem::path data_dir() { return xdg_dir("XDG_DATA_HOME", ".local/share"); }

    std::filesystem::path state_dir() { return xdg_dir("XDG_STATE_HOME", ".local/state"); }

} // namespace tether::paths
