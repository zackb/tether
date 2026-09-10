#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tether {

    // The application id flatpak uses in the running sandbox, empty outside one.
    std::string flatpak_app_id();

    // Absolute path of a program on PATH, empty when it is not there.
    std::string which_program(const std::string& name);

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

} // namespace tether
