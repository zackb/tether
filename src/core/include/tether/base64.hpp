#pragma once

#include <string>
#include <vector>

namespace tether {
    std::string base64_encode(const std::vector<unsigned char>& data);
    std::string base64_encode(const unsigned char* data, size_t len);
    std::vector<unsigned char> base64_decode(const std::string& input);
}
