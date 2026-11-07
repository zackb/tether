#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tether {

    struct ExtensionHostInstall {
        std::string wrapper;              // script the browsers execute
        std::vector<std::string> written; // manifest paths written
        std::vector<std::string> skipped; // browsers not installed for this user
        std::vector<std::string> errors;
    };

    // Installs the native messaging manifests into the per-user browser directories. For portable builds, which cannot
    // write the system directories the distro packages use.
    ExtensionHostInstall install_extension_host(const std::filesystem::path& home);
    ExtensionHostInstall install_extension_host();

    // Inside a Flatpak, one `flatpak override` command per browser directory
    // the sandbox needs before the manifests can be written. Empty outside one.
    std::vector<std::string> extension_host_overrides();

} // namespace tether
