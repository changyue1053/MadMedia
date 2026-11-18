#include "core/crypto/crc32c.h"

#include <array>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define IMMD_CRC_HW 1
#elif defined(__SSE4_2__)
#include <nmmintrin.h>
#define IMMD_CRC_HW 1
#else
#define IMMD_CRC_HW 0
#endif

namespace immd {

namespace {
const std::array<uint32_t, 256>& table() {
  static const std::array<uint32_t, 256> t = [] {
    std::array<uint32_t, 256> tt{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
      tt[i] = c;
    }
    return tt;
  }();
  return t;
}
}  // namespace

uint32_t crc32c_sw(uint32_t crc, const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = ~crc;
  for (size_t i = 0; i < len; ++i) c = table()[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return ~c;
}

bool crc32c_hw_available() { return IMMD_CRC_HW != 0; }

uint32_t crc32c(uint32_t crc, const void* data, size_t len) {
#if IMMD_CRC_HW
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = ~crc;
#if defined(__ARM_FEATURE_CRC32)
  while (len >= 8) {
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    c = __crc32cd(c, v);
    p += 8;
    len -= 8;
  }
  while (len > 0) {
    c = __crc32cb(c, *p++);
    --len;
  }
#else
  while (len >= 8) {
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    c = static_cast<uint32_t>(_mm_crc32_u64(c, v));
    p += 8;
    len -= 8;
  }
  while (len > 0) {
    c = _mm_crc32_u8(c, *p++);
    --len;
  }
#endif
  return ~c;
#else
  return crc32c_sw(crc, data, len);
#endif
}

}  // namespace immd

