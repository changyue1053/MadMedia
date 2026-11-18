// 重试退避：指数 + 全抖动（架构 §7）。RNG 注入保证测试确定性。
#pragma once

#include <algorithm>
#include <cstdint>
#include <random>

namespace immd {

struct Backoff {
  int64_t base_ms = 500;
  int64_t cap_ms = 30000;

  // attempt 从 1 开始。full jitter：uniform(0, min(cap, base * 2^(attempt-1)))。
  int64_t delay_ms(int attempt, std::mt19937& rng) const {
    if (attempt < 1) attempt = 1;
    int shift = std::min(attempt - 1, 20);
    int64_t ceiling = std::min(cap_ms, base_ms << shift);
    if (ceiling <= 0) ceiling = cap_ms;
    std::uniform_int_distribution<int64_t> d(0, ceiling);
    return d(rng);
  }
};

}  // namespace immd

