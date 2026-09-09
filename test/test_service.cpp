#include "scoped_env.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <tether/service.hpp>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

    class ServiceTest : public ::testing::Test {
    protected:
        void SetUp() override { fs::create_directories(config_); }

        void TearDown() override {
            std::error_code ec;
            fs::remove_all(root_, ec);
        }

        const fs::path root_ =
            fs::temp_directory_path() / ("tether-service-" + std::to_string(::getpid()) + "-" +
                                         ::testing::UnitTest::GetInstance()->current_test_info()->name());
        const fs::path config_ = root_ / "config";
        const fs::path unit_ = config_ / "systemd/user/tetherd.service";
    };

    std::string read(const fs::path& path) {
        std::ifstream in(path);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

} // namespace

TEST_F(ServiceTest, RenderTetherdUnitFillsInExecStart) {
    const std::string unit = tether::render_tetherd_unit("/usr/bin/tetherd");

    EXPECT_NE(unit.find("ExecStart=/usr/bin/tetherd"), std::string::npos) << unit;
    EXPECT_EQ(unit.find("__TETHERD_EXEC__"), std::string::npos) << unit;
    EXPECT_NE(unit.find("WantedBy=default.target"), std::string::npos) << unit;
}

TEST_F(ServiceTest, InstallWritesTheUnitAndUninstallTakesItBack) {
    const auto installed = tether::install_tetherd_service(config_);

    ASSERT_TRUE(installed.errors.empty()) << installed.errors.front();
    EXPECT_TRUE(installed.changed);
    EXPECT_EQ(installed.path, unit_.string());
    ASSERT_TRUE(fs::exists(unit_));
    EXPECT_NE(read(unit_).find("ExecStart="), std::string::npos);

    const auto removed = tether::uninstall_tetherd_service(config_);
    EXPECT_TRUE(removed.errors.empty());
    EXPECT_TRUE(removed.changed);
    EXPECT_FALSE(fs::exists(unit_));
}

TEST_F(ServiceTest, UninstallingWhatWasNeverInstalledIsNotAnError) {
    const auto removed = tether::uninstall_tetherd_service(config_);

    EXPECT_TRUE(removed.errors.empty());
    EXPECT_FALSE(removed.changed);
}

TEST_F(ServiceTest, ExecStartPointsAtTheAppImageWhenRunningFromOne) {
    const tether::testing::ScopedEnv appimage("APPIMAGE", std::string("/opt/tether.AppImage"));

    EXPECT_EQ(tether::running_flavor(), tether::Flavor::AppImage);
    EXPECT_EQ(tether::tetherd_exec_command(), "/opt/tether.AppImage --daemon");
}

TEST_F(ServiceTest, TheInstallerOwnsUpdateAndRemovalWhenItIsThere) {
    const fs::path installer = root_ / "install.sh";
    std::ofstream(installer) << "#!/bin/sh\n";

    for (const auto flavor : {tether::Flavor::AppImage, tether::Flavor::Distro, tether::Flavor::Unknown}) {
        const auto update = tether::update_plan(flavor, tether::PackageManager::Apt, installer);
        EXPECT_TRUE(update.runnable);
        EXPECT_EQ(update.command, installer.string() + " update");

        const auto remove = tether::remove_plan(flavor, tether::PackageManager::Apt, installer);
        EXPECT_TRUE(remove.runnable);
        EXPECT_EQ(remove.command, installer.string() + " uninstall");
    }
}

TEST_F(ServiceTest, WithoutTheInstallerThePackageManagerIsNamedInstead) {
    const fs::path absent = root_ / "nothing-here.sh";

    const auto apt = tether::remove_plan(tether::Flavor::Distro, tether::PackageManager::Apt, absent);
    EXPECT_FALSE(apt.runnable);
    EXPECT_EQ(apt.command, "sudo apt-get remove tether");

    const auto pacman = tether::remove_plan(tether::Flavor::Distro, tether::PackageManager::Pacman, absent);
    EXPECT_EQ(pacman.command, "sudo pacman -Rns tether");

    const auto arch_update = tether::update_plan(tether::Flavor::Distro, tether::PackageManager::Pacman, absent);
    EXPECT_EQ(arch_update.command, "yay -Syu tether");

    // Nothing owns a portable build but the installer that fetched it.
    const auto portable = tether::update_plan(tether::Flavor::AppImage, tether::PackageManager::None, absent);
    EXPECT_FALSE(portable.runnable);
    EXPECT_NE(portable.command.find("install.sh"), std::string::npos) << portable.command;
}

TEST_F(ServiceTest, InstallerLivesBesideTheRestOfTethersData) {
    EXPECT_EQ(tether::installer_path("/home/x/.local/share/tether"), "/home/x/.local/share/tether/install.sh");
    EXPECT_TRUE(tether::installer_path("").empty());
}

TEST_F(ServiceTest, WhichProgramFindsOnlyWhatIsExecutable) {
    const fs::path bin = root_ / "bin";
    fs::create_directories(bin);
    std::ofstream(bin / "tether-fake") << "#!/bin/sh\n";
    std::ofstream(bin / "tether-data") << "not a program\n";
    fs::permissions(bin / "tether-fake", fs::perms::owner_all);

    const tether::testing::ScopedEnv path("PATH", bin.string());

    EXPECT_EQ(tether::which_program("tether-fake"), (bin / "tether-fake").string());
    EXPECT_TRUE(tether::which_program("tether-data").empty());
    EXPECT_TRUE(tether::which_program("tether-missing").empty());
}
