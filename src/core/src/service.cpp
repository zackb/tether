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

        constexpr const char* INSTALLER_URL = "https://raw.githubusercontent.com/zackb/tether/main/scripts/install.sh";

        std::string curl_installer(const char* verb) {
            return std::string("curl -fsSL ") + INSTALLER_URL + " | sh -s -- " + verb;
        }

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

    Flavor running_flavor() {
        if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage)
            return Flavor::AppImage;

        if (!flatpak_app_id().empty())
            return Flavor::Flatpak;

        std::error_code ec;
        const std::string self = fs::read_symlink("/proc/self/exe", ec).string();
        if (!ec && (self.rfind("/usr/", 0) == 0 || self.rfind("/opt/", 0) == 0))
            return Flavor::Distro;

        return Flavor::Unknown;
    }

    PackageManager detect_package_manager() {
        // Order matters only where two are installed: pacman first, because on
        // Arch tether comes from the AUR and apt is never there.
        if (!which_program("pacman").empty())
            return PackageManager::Pacman;
        if (!which_program("apt-get").empty())
            return PackageManager::Apt;
        if (!which_program("dnf").empty())
            return PackageManager::Dnf;
        if (!which_program("zypper").empty())
            return PackageManager::Zypper;
        return PackageManager::None;
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
        const char* config_home = std::getenv("XDG_CONFIG_HOME");
        if (config_home && *config_home == '/')
            return install_tetherd_service(fs::path(config_home));

        const char* home = std::getenv("HOME");
        if (!home || !*home) {
            ServiceInstall result;
            result.errors.push_back("No $HOME, so there is nowhere to write the unit.");
            return result;
        }
        return install_tetherd_service(fs::path(home) / ".config");
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
        const char* config_home = std::getenv("XDG_CONFIG_HOME");
        if (config_home && *config_home == '/')
            return uninstall_tetherd_service(fs::path(config_home));

        const char* home = std::getenv("HOME");
        if (!home || !*home)
            return {};
        return uninstall_tetherd_service(fs::path(home) / ".config");
    }

    fs::path installer_path(const fs::path& data_dir) {
        return data_dir.empty() ? fs::path{} : data_dir / "install.sh";
    }

    Plan update_plan(Flavor flavor, PackageManager pm, const fs::path& installer) {
        std::error_code ec;
        if (!installer.empty() && fs::is_regular_file(installer, ec)) {
            return {true, installer.string() + " update"};
        }

        switch (flavor) {
        case Flavor::Flatpak: {
            const std::string app_id = flatpak_app_id();
            return {false, "flatpak update " + (app_id.empty() ? std::string("com.tether.desktop") : app_id)};
        }
        case Flavor::Distro:
            if (pm == PackageManager::Pacman)
                return {false, "yay -Syu tether"};
            return {false, curl_installer("update")};
        case Flavor::AppImage:
        case Flavor::Unknown:
            break;
        }

        return {false, curl_installer("update")};
    }

    Plan remove_plan(Flavor flavor, PackageManager pm, const fs::path& installer) {
        std::error_code ec;
        if (!installer.empty() && fs::is_regular_file(installer, ec)) {
            return {true, installer.string() + " uninstall"};
        }

        switch (flavor) {
        case Flavor::Flatpak: {
            const std::string app_id = flatpak_app_id();
            return {false, "flatpak uninstall " + (app_id.empty() ? std::string("com.tether.desktop") : app_id)};
        }
        case Flavor::Distro:
            switch (pm) {
            case PackageManager::Pacman:
                return {false, "sudo pacman -Rns tether"};
            case PackageManager::Apt:
                return {false, "sudo apt-get remove tether"};
            case PackageManager::Dnf:
                return {false, "sudo dnf remove tether"};
            case PackageManager::Zypper:
                return {false, "sudo zypper remove tether"};
            case PackageManager::None:
                break;
            }
            return {false, "sudo cmake --build build/release --target uninstall"};
        case Flavor::AppImage:
        case Flavor::Unknown:
            break;
        }

        return {false, curl_installer("uninstall")};
    }

} // namespace tether
