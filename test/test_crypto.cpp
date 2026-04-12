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
    // 1. Initialization creates physical assets natively
    EXPECT_TRUE(tether::Crypto::instance().init());
    
    EXPECT_TRUE(std::filesystem::exists("/tmp/tether_tests/.config/tether/key.pem"));
    EXPECT_TRUE(std::filesystem::exists("/tmp/tether_tests/.config/tether/cert.pem"));
    EXPECT_TRUE(std::filesystem::exists("/tmp/tether_tests/.config/tether/known_hosts.json"));

    // 2. OpenSSL Contexts are successfully mapped into memory
    EXPECT_NE(tether::Crypto::instance().get_server_context(), nullptr);
    EXPECT_NE(tether::Crypto::instance().get_client_context(), nullptr);

    // 3. Known Hosts JSON abstraction operates securely
    std::string test_fp = "012345abcdef";
    EXPECT_FALSE(tether::Crypto::instance().is_host_known(test_fp));
    
    tether::Crypto::instance().add_known_host("Unit Test Device", test_fp);
    EXPECT_TRUE(tether::Crypto::instance().is_host_known(test_fp));
    
    std::string dump = tether::Crypto::instance().get_known_hosts_dump();
    EXPECT_NE(dump.find("Unit Test Device"), std::string::npos);
}
