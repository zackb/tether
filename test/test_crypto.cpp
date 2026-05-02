#include <gtest/gtest.h>
#include <tether/crypto.hpp>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iostream>

class CryptoTest : public ::testing::Test {
protected:
    void SetUp() override {
        setenv("HOME", "/tmp/tether_tests", 1);
        std::filesystem::remove_all("/tmp/tether_tests");
        std::filesystem::create_directories("/tmp/tether_tests");
    }

    void TearDown() override {
        std::filesystem::remove_all("/tmp/tether_tests");
    }
};

TEST_F(CryptoTest, CompleteCryptographicLifecycle) {
    EXPECT_TRUE(tether::Crypto::instance().init());

    EXPECT_TRUE(std::filesystem::exists("/tmp/tether_tests/.config/tether/key.pem"));
    EXPECT_TRUE(std::filesystem::exists("/tmp/tether_tests/.config/tether/cert.pem"));
    EXPECT_TRUE(std::filesystem::exists("/tmp/tether_tests/.config/tether/known_hosts.json"));

    EXPECT_NE(tether::Crypto::instance().get_server_context(), nullptr);
    EXPECT_NE(tether::Crypto::instance().get_client_context(), nullptr);

    std::string test_fp = "012345abcdef";
    EXPECT_FALSE(tether::Crypto::instance().is_host_known(test_fp));

    tether::Crypto::instance().add_known_host("Unit Test Device", test_fp);
    EXPECT_TRUE(tether::Crypto::instance().is_host_known(test_fp));

    std::string dump = tether::Crypto::instance().get_known_hosts_dump();
    EXPECT_NE(dump.find("Unit Test Device"), std::string::npos);
}

TEST_F(CryptoTest, KnownHostsPersistsAcrossInit) {
    EXPECT_TRUE(tether::Crypto::instance().init());

    tether::Crypto::instance().add_known_host("Persistent Device", "persistent_fp");
    EXPECT_TRUE(tether::Crypto::instance().is_host_known("persistent_fp"));

    std::string dump = tether::Crypto::instance().get_known_hosts_dump();
    EXPECT_NE(dump.find("Persistent Device"), std::string::npos);
}

TEST_F(CryptoTest, MultipleKnownHosts) {
    EXPECT_TRUE(tether::Crypto::instance().init());

    tether::Crypto::instance().add_known_host("Device1", "fp1");
    tether::Crypto::instance().add_known_host("Device2", "fp2");
    tether::Crypto::instance().add_known_host("Device3", "fp3");

    EXPECT_TRUE(tether::Crypto::instance().is_host_known("fp1"));
    EXPECT_TRUE(tether::Crypto::instance().is_host_known("fp2"));
    EXPECT_TRUE(tether::Crypto::instance().is_host_known("fp3"));
    EXPECT_FALSE(tether::Crypto::instance().is_host_known("fp4"));
}

TEST_F(CryptoTest, GetMyFingerprint) {
    EXPECT_TRUE(tether::Crypto::instance().init());
    std::string fp = tether::Crypto::instance().get_my_fingerprint();
    EXPECT_FALSE(fp.empty());
    EXPECT_EQ(fp.size(), 64);
}
