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

TEST(Base64Test, EncodeVector) {
    std::vector<unsigned char> data = {'h', 'i'};
    std::string enc = tether::base64_encode(data);
    EXPECT_EQ(enc, "aGk=");
}

TEST(Base64Test, RoundTripBinary) {
    std::vector<unsigned char> original = {0x00, 0xFF, 0x01, 0x02, 0x03};
    std::string enc = tether::base64_encode(original);
    auto dec = tether::base64_decode(enc);
    EXPECT_EQ(dec, original);
}

TEST(Base64Test, SingleCharEncode) {
    std::string enc = tether::base64_encode(reinterpret_cast<const unsigned char*>("a"), 1);
    EXPECT_EQ(enc, "YQ==");
}

TEST(Base64Test, TwoCharEncode) {
    std::string enc = tether::base64_encode(reinterpret_cast<const unsigned char*>("ab"), 2);
    EXPECT_EQ(enc, "YWI=");
}

TEST(Base64Test, DecodeInvalidChars) {
    auto dec = tether::base64_decode("!!!");
    EXPECT_TRUE(dec.empty());
}
