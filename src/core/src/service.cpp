#include "tether/service.hpp"
#include "tether/packaging.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace tether {

    namespace {

        namespace fs = std::filesystem;

        // The token render_tetherd_unit() replaces. CMake leaves it in the
        // embedded copy of the unit; the copy the distro packages install has
        // the real path already.
        constexpr const char* EXEC_TOKEN = "__TETHERD_EXEC__";

        bool write_file(const fs::path& path, const std::string& content, std::string& err) {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            if (ec) {
                err = path.parent_path().string() + ": " + ec.message();
                return false;
            }

            std::ofstream out(path, std::ios::trunc);
            if (!out.is_open()) {
                err = path.string() + ": cannot open for writing";
                return false;
            }
            out << content;
            if (!out) {
                err = path.string() + ": write failed";
                return false;
            }
            return true;
        }

        fs::path user_config_dir() {
            const char* config_home = std::getenv("XDG_CONFIG_HOME");
            if (config_home && *config_home == '/')
                return fs::path(config_home);

            const char* home = std::getenv("HOME");
            if (!home || !*home)
                return {};
            return fs::path(home) / ".config";
        }

    } // namespace

    std::string flatpak_app_id() {
        std::ifstream in("/.flatpak-info");
        if (!in.is_open())
            return {};
        for (std::string line; std::getline(in, line);) {
            if (line.rfind("name=", 0) == 0)
                return line.substr(5);
        }
        return {};
    }

    std::string which_program(const std::string& name) {
        const char* path = std::getenv("PATH");
        if (!path || !*path)
            path = "/usr/local/bin:/usr/bin:/bin";

        std::istringstream parts(path);
        std::string dir;
        std::error_code ec;
        while (std::getline(parts, dir, ':')) {
            if (dir.empty())
                continue;
            const fs::path candidate = fs::path(dir) / name;
            if (fs::is_regular_file(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0)
                return candidate.string();
        }
        return {};
    }

    std::string tetherd_exec_command() {
        if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage)
            return std::string(appimage) + " --daemon";

        if (const std::string app_id = flatpak_app_id(); !app_id.empty()) {
            const std::string flatpak = which_program("flatpak");
            return (flatpak.empty() ? "/usr/bin/flatpak" : flatpak) + " run --command=tetherd " + app_id;
        }

        std::error_code ec;
        const fs::path self = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            const fs::path sibling = self.parent_path() / "tetherd";
            if (fs::is_regular_file(sibling, ec))
                return sibling.string();
        }

        const std::string found = which_program("tetherd");
        return found.empty() ? "/usr/bin/tetherd" : found;
    }

    std::string render_tetherd_unit(const std::string& exec_command) {
        std::string unit = packaging::TETHERD_UNIT;
        const auto at = unit.find(EXEC_TOKEN);
        if (at != std::string::npos)
            unit.replace(at, std::strlen(EXEC_TOKEN), exec_command);
        return unit;
    }

    ServiceInstall install_tetherd_service(const fs::path& config_dir) {
        ServiceInstall result;
        if (config_dir.empty()) {
            result.errors.push_back("No config directory to write the unit into.");
            return result;
        }

        const fs::path unit = config_dir / "systemd" / "user" / "tetherd.service";
        result.path = unit.string();

        std::string err;
        if (!write_file(unit, render_tetherd_unit(tetherd_exec_command()), err)) {
            result.errors.push_back(err);
            return result;
        }

        result.changed = true;
        return result;
    }

    ServiceInstall install_tetherd_service() {
        const fs::path config = user_config_dir();
        if (config.empty()) {
            ServiceInstall result;
            result.errors.push_back("No $HOME, so there is nowhere to write the unit.");
            return result;
        }
        return install_tetherd_service(config);
    }

    ServiceInstall uninstall_tetherd_service(const fs::path& config_dir) {
        ServiceInstall result;
        if (config_dir.empty())
            return result;

        const fs::path unit = config_dir / "systemd" / "user" / "tetherd.service";
        result.path = unit.string();

        std::error_code ec;
        if (!fs::exists(unit, ec))
            return result;

        if (!fs::remove(unit, ec)) {
            result.errors.push_back(unit.string() + ": " + ec.message());
            return result;
        }

        result.changed = true;
        return result;
    }

    ServiceInstall uninstall_tetherd_service() {
        const fs::path config = user_config_dir();
        if (config.empty())
            return {};
        return uninstall_tetherd_service(config);
    }

} // namespace tether
