// SHA-256 + HMAC-SHA256：秒传指纹（架构 §5/§16）与挑战应答的密码学原语。
// 自带实现（NIST 向量验证），内核零 OpenSSL 依赖；流式接口支持单遍 IO 边读边算。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace immd {

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const void* data, size_t len);
  std::array<uint8_t, 32> finish();

  static std::array<uint8_t, 32> digest(const void* data, size_t len);

 private:
  void process_block(const uint8_t* p);
  uint32_t h_[8];
  uint64_t total_ = 0;
  uint8_t buf_[64];
  size_t buflen_ = 0;
};

std::array<uint8_t, 32> hmac_sha256(const void* key, size_t key_len, const void* data, size_t data_len);

std::string to_hex(const uint8_t* p, size_t n);
template <size_t N>
std::string to_hex(const std::array<uint8_t, N>& a) {
  return to_hex(a.data(), N);
}

}  // namespace immd

