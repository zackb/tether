#include <gtest/gtest.h>
#include <tether/extension_host.hpp>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {

    std::string read(const fs::path& path) {
        std::ifstream in(path);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

} // namespace

TEST(ExtensionHost, WritesManifestsOnlyWhereTheBrowserIs) {
    const fs::path home = fs::temp_directory_path() / "tether-extension-host-test";
    fs::remove_all(home);
    fs::create_directories(home / ".mozilla");
    fs::create_directories(home / ".config/google-chrome");

    const auto result = tether::install_extension_host(home);
    ASSERT_TRUE(result.errors.empty()) << result.errors.front();

    const fs::path mozilla = home / ".mozilla/native-messaging-hosts/com.tether.extension.json";
    const fs::path chrome = home / ".config/google-chrome/NativeMessagingHosts/com.tether.extension.json";
    EXPECT_TRUE(fs::exists(mozilla));
    EXPECT_TRUE(fs::exists(chrome));

    // thunderbird not installed skipped, not created
    EXPECT_FALSE(fs::exists(home / ".thunderbird"));
    EXPECT_EQ(result.written.size(), 2u);

    // manifest keeps its own schema and points at the wrapper
    const auto moz = nlohmann::json::parse(read(mozilla));
    const auto chr = nlohmann::json::parse(read(chrome));
    EXPECT_EQ(moz["path"], result.wrapper);
    EXPECT_EQ(chr["path"], result.wrapper);
    EXPECT_TRUE(moz.contains("allowed_extensions"));
    EXPECT_TRUE(chr.contains("allowed_origins"));

    // wrapper has to be executable, or the browser cannot start the host
    ASSERT_TRUE(fs::exists(result.wrapper));
    EXPECT_TRUE((fs::status(result.wrapper).permissions() & fs::perms::owner_exec) != fs::perms::none);

    fs::remove_all(home);
}

// Inside a Flatpak the manifests have to go in the real home, not the app's
// private one, or no browser on the host ever reads them.
TEST(ExtensionHost, WritesOutsideTheFlatpakPrivateHome) {
    const fs::path real = fs::temp_directory_path() / "tether-extension-host-flatpak";
    fs::remove_all(real);
    fs::create_directories(real / ".mozilla");

    const fs::path sandboxed = real / ".var/app/io.github.zackb.tether";
    fs::create_directories(sandboxed);

    const auto result = tether::install_extension_host(sandboxed);
    ASSERT_TRUE(result.errors.empty()) << result.errors.front();

    EXPECT_TRUE(fs::exists(real / ".mozilla/native-messaging-hosts/com.tether.extension.json"));
    EXPECT_FALSE(fs::exists(sandboxed / ".mozilla"));
    EXPECT_EQ(result.wrapper, (real / ".local/bin/tether-native-host").string());

    fs::remove_all(real);
}
