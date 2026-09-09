#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tether {

    // How this build reached the machine. It decides who owns updating and
    // removing it: a distro package belongs to the package manager, a portable
    // build to whoever unpacked it.
    enum class Flavor {
        Distro,   // .deb, .rpm, pacman, or make install
        AppImage, // running out of an AppImage mount
        Flatpak,  // running inside the flatpak sandbox
        Unknown,
    };

    enum class PackageManager { Apt, Dnf, Pacman, Zypper, None };

    // The application id flatpak uses in the running sandbox, empty outside one.
    std::string flatpak_app_id();

    // Absolute path of a program on PATH, empty when it is not there.
    std::string which_program(const std::string& name);

    Flavor running_flavor();
    PackageManager detect_package_manager();

    // ExecStart= line for the systemd unit: an absolute program and its
    // arguments, pointed at the tetherd this build would start. systemd runs no
    // shell, so this is never quoted for one.
    std::string tetherd_exec_command();

    // The user unit with its ExecStart filled in.
    std::string render_tetherd_unit(const std::string& exec_command);

    struct ServiceInstall {
        std::string path; // unit written, or the one that was removed
        std::vector<std::string> errors;
        bool changed = false; // a file was written or removed
    };

    // Writes the tetherd user unit into $XDG_CONFIG_HOME/systemd/user. Needed by
    // portable builds, which cannot write the system directory the distro
    // packages install into. Never enables or starts anything: that is the
    // user's call, and 'tether --install-service' prints the commands.
    ServiceInstall install_tetherd_service(const std::filesystem::path& config_dir);
    ServiceInstall install_tetherd_service();

    // Removes the unit this user's install wrote, if it is there.
    ServiceInstall uninstall_tetherd_service(const std::filesystem::path& config_dir);
    ServiceInstall uninstall_tetherd_service();

    // What updating or removing this build takes.
    struct Plan {
        bool runnable = false; // tether can carry it out itself
        std::string command;   // the command that does it
    };

    // scripts/install.sh keeps a copy of itself here when it installs tether,
    // which is what makes 'tether --self-update' possible.
    std::filesystem::path installer_path(const std::filesystem::path& data_dir);

    // installer is the path install.sh left behind, or empty when it never ran.
    Plan update_plan(Flavor flavor, PackageManager pm, const std::filesystem::path& installer);
    Plan remove_plan(Flavor flavor, PackageManager pm, const std::filesystem::path& installer);

} // namespace tether
