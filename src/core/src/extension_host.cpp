#include "tether/extension_host.hpp"
#include "tether/packaging.hpp"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string_view>

namespace tether {

    namespace {

        namespace fs = std::filesystem;

        struct Target {
            const char* profile; // browser's own directory, relative to $HOME
            const char* dir;     // where its native messaging manifests live
            bool chrome;         // chrome manifest schema rather than mozilla's
        };

        // Flatpak browsers keep a private $HOME in ~/.var/app, each one needs its own copy of the manifest.
        constexpr Target kTargets[] = {
            {".mozilla", ".mozilla/native-messaging-hosts", false},
            {".thunderbird", ".thunderbird/native-messaging-hosts", false},
            {".betterbird", ".betterbird/native-messaging-hosts", false},
            {".var/app/org.mozilla.firefox", ".var/app/org.mozilla.firefox/.mozilla/native-messaging-hosts", false},
            {".var/app/org.mozilla.Thunderbird",
             ".var/app/org.mozilla.Thunderbird/.thunderbird/native-messaging-hosts",
             false},
            {".var/app/eu.betterbird.Betterbird",
             ".var/app/eu.betterbird.Betterbird/.betterbird/native-messaging-hosts",
             false},
            {".config/chromium", ".config/chromium/NativeMessagingHosts", true},
            {".config/google-chrome", ".config/google-chrome/NativeMessagingHosts", true},
            {".var/app/org.chromium.Chromium",
             ".var/app/org.chromium.Chromium/config/chromium/NativeMessagingHosts",
             true},
            {".var/app/com.google.Chrome",
             ".var/app/com.google.Chrome/config/google-chrome/NativeMessagingHosts",
             true},
        };

        // In flatpak $HOME is the app's own private directory, but the
        // browsers that have to read these manifests live outside it.
        fs::path host_home(const fs::path& home) {
            static constexpr std::string_view MARKER = "/.var/app/";
            const std::string text = home.string();
            const auto at = text.find(MARKER);
            return at == std::string::npos ? home : fs::path(text.substr(0, at));
        }

        // The application id flatpak uses in the running sandbox.
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

        // The shell frag the wrapper execs, quoted.
        std::string launch_command() {
            if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage)
                return "\"" + std::string(appimage) + "\"";

            if (const std::string app_id = flatpak_app_id(); !app_id.empty())
                return "flatpak run --command=tether " + app_id;

            std::error_code ec;
            const fs::path self = fs::read_symlink("/proc/self/exe", ec);
            return "\"" + (ec ? std::string("tether") : self.string()) + "\"";
        }

        bool write_file(const fs::path& path, const std::string& content, std::string& err) {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            if (ec) {
                err = path.parent_path().string() + ": " + ec.message();
                return false;
            }

            if (fs::is_symlink(path, ec))
                fs::remove(path, ec);

            std::ofstream out(path, std::ios::trunc);
            if (!out.is_open()) {
                err = path.string() + ": cannot open for writing";
                return false;
            }
            out << content;
            out.close();
            if (!out) {
                err = path.string() + ": write failed";
                return false;
            }
            return true;
        }

        // The packaged manifest names the packaged host, repoint it at ours.
        std::string manifest_for(bool chrome, const std::string& wrapper, std::string& err) {
            try {
                auto manifest = nlohmann::json::parse(chrome ? packaging::NATIVE_HOST_MANIFEST_CHROME
                                                             : packaging::NATIVE_HOST_MANIFEST_MOZILLA);
                manifest["path"] = wrapper;
                return manifest.dump(2) + "\n";
            } catch (const std::exception& e) {
                err = e.what();
                return {};
            }
        }

    } // namespace

    ExtensionHostInstall install_extension_host(const fs::path& requested_home) {
        ExtensionHostInstall result;
        const fs::path home = host_home(requested_home);

        // A Flatpak can always write its own ~/.var/app dir, which browsers on the host can exec.
        const std::string app_id = flatpak_app_id();
        const fs::path wrapper = app_id.empty() ? home / ".local/bin/tether-native-host"
                                                : home / ".var/app" / app_id / "tether-native-host";
        result.wrapper = wrapper.string();

        const std::string script = "#!/bin/sh\n"
                                   "exec " +
                                   launch_command() + " --native-host 2>> /tmp/tether-native.log\n";

        std::string err;
        if (!write_file(wrapper, script, err)) {
            result.errors.push_back(err);
            return result;
        }
        std::error_code ec;
        fs::permissions(wrapper,
                        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                            fs::perms::others_exec,
                        fs::perm_options::replace,
                        ec);
        if (ec) {
            result.errors.push_back(result.wrapper + ": " + ec.message());
            return result;
        }

        std::string chrome_manifest = manifest_for(true, result.wrapper, err);
        std::string mozilla_manifest = manifest_for(false, result.wrapper, err);
        if (chrome_manifest.empty() || mozilla_manifest.empty()) {
            result.errors.push_back("built-in native messaging manifest is unreadable: " + err);
            return result;
        }

        for (const auto& target : kTargets) {
            std::error_code exists_ec;
            if (!fs::exists(home / target.profile, exists_ec)) {
                result.skipped.push_back((home / target.profile).string());
                continue;
            }

            const fs::path path = home / target.dir / "com.tether.extension.json";
            if (write_file(path, target.chrome ? chrome_manifest : mozilla_manifest, err))
                result.written.push_back(path.string());
            else
                result.errors.push_back(err);
        }

        return result;
    }

    ExtensionHostInstall install_extension_host() {
        const char* home = std::getenv("HOME");
        if (!home || !*home) {
            ExtensionHostInstall result;
            result.errors.emplace_back("HOME is not set, so there is nowhere to install the manifests.");
            return result;
        }
        return install_extension_host(fs::path(home));
    }

} // namespace tether
