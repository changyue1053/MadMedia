// 任务 ID：128bit 随机 hex。它同时是协议幂等键 client_task_id（架构 §5），崩溃重试凭它拿回同一单。
#pragma once

#include <cstdint>
#include <random>
#include <string>

namespace immd {

inline std::string gen_id(const char* prefix = "t") {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  static const char* hex = "0123456789abcdef";
  std::string s(prefix);
  s.push_back('_');
  for (int w = 0; w < 2; ++w) {
    uint64_t v = rng();
    for (int i = 0; i < 16; ++i) {
      s.push_back(hex[v & 0xf]);
      v >>= 4;
    }
  }
  return s;
}

}  // namespace immd

