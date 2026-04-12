#include <gtest/gtest.h>
#include <tether/file_transfer.hpp>
#include <tether/base64.hpp>
#include <filesystem>
#include <fstream>
#include <string>

class FileTransferTest : public ::testing::Test {
protected:
    void SetUp() override {
        setenv("HOME", "/tmp/tether_tests", 1);
        std::filesystem::remove_all("/tmp/tether_tests");
        std::filesystem::create_directories("/tmp/tether_tests/Downloads");
    }

    void TearDown() override {
        std::filesystem::remove_all("/tmp/tether_tests");
    }
};

TEST_F(FileTransferTest, SingleChunkTransfer) {
    tether::FileReceiveManager mgr;
    
    std::string t_id = "test_trans_1";
    mgr.handle_start(t_id, "hello.txt", 11);
    
    std::string content = "hello world";
    std::string b64 = tether::base64_encode(reinterpret_cast<const unsigned char*>(content.data()), content.size());
    
    mgr.handle_chunk(t_id, 0, b64);
    EXPECT_TRUE(mgr.handle_end(t_id));
    
    // Ensure output matches native directory targeting logic
    EXPECT_TRUE(std::filesystem::exists("/tmp/tether_tests/Downloads/hello.txt"));
    
    std::ifstream iff("/tmp/tether_tests/Downloads/hello.txt");
    std::string res;
    std::getline(iff, res);
    EXPECT_EQ(res, "hello world");
}

TEST_F(FileTransferTest, NameDeduplicatorAppendsCounterGracefully) {
    tether::FileReceiveManager mgr;
    
    // First transmission
    std::string t_id1 = "test_trans_1";
    mgr.handle_start(t_id1, "image.png", 3);
    mgr.handle_chunk(t_id1, 0, tether::base64_encode((const unsigned char*)"ABC", 3));
    mgr.handle_end(t_id1);
    
    // Identical name collision transmission
    std::string t_id2 = "test_trans_2";
    mgr.handle_start(t_id2, "image.png", 3);
    mgr.handle_chunk(t_id2, 0, tether::base64_encode((const unsigned char*)"DEF", 3));
    mgr.handle_end(t_id2);
    
    EXPECT_TRUE(std::filesystem::exists("/tmp/tether_tests/Downloads/image.png"));
    EXPECT_TRUE(std::filesystem::exists("/tmp/tether_tests/Downloads/image(1).png"));
    
    // Verify isolated integrity natively
    std::ifstream i2("/tmp/tether_tests/Downloads/image(1).png");
    std::string r2;
    std::getline(i2, r2);
    EXPECT_EQ(r2, "DEF");
}
