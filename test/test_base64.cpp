#include <gtest/gtest.h>
#include <tether/base64.hpp>
#include <string>
#include <vector>

TEST(Base64Test, EncodeSimple) {
    std::string text = "hello world";
    std::string enc = tether::base64_encode(reinterpret_cast<const unsigned char*>(text.data()), text.size());
    EXPECT_EQ(enc, "aGVsbG8gd29ybGQ=");
}

TEST(Base64Test, DecodeSimple) {
    std::string enc = "aGVsbG8gd29ybGQ=";
    std::vector<unsigned char> dec = tether::base64_decode(enc);
    std::string dec_str(dec.begin(), dec.end());
    EXPECT_EQ(dec_str, "hello world");
}

TEST(Base64Test, DecodeWithPadding) {
    // hello -> aGVsbG8=
    std::string enc = "aGVsbG8=";
    std::vector<unsigned char> dec = tether::base64_decode(enc);
    std::string dec_str(dec.begin(), dec.end());
    EXPECT_EQ(dec_str, "hello");
}

TEST(Base64Test, EdgeCases) {
    EXPECT_EQ(tether::base64_encode(nullptr, 0), "");
    EXPECT_TRUE(tether::base64_decode("").empty());
}
