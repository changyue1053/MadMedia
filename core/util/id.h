// OS-sourced 128-bit operation IDs, not authentication tokens.
#pragma once
#include <string>
#include <stdexcept>
#include "core/platform/entropy.h"
namespace immd {
inline std::string gen_id(const char* prefix="t") {
 std::array<unsigned char,16> bytes{};
 if(!madmedia_platform::entropy128(bytes))throw std::runtime_error("task ID entropy unavailable");
 static const char* hex="0123456789abcdef";
 std::string result(prefix);result.push_back('_');result.reserve(result.size()+32);
 for(auto b:bytes){result.push_back(hex[b>>4]);result.push_back(hex[b&15]);}
 return result;
}
}
