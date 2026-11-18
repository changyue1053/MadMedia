// 时钟注入位：退避计时、限速、EWMA 全部经由 Clock 取时，测试用 VirtualClock 消灭真实等待。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace immd {

class Clock {
 public:
  virtual ~Clock() = default;
  // 单调时钟毫秒（不受系统时间跳变影响）。
  virtual int64_t now_ms() const = 0;
};

class RealClock final : public Clock {
 public:
  int64_t now_ms() const override {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  }
  static RealClock& instance() {
    static RealClock c;
    return c;
  }
};

class VirtualClock final : public Clock {
 public:
  explicit VirtualClock(int64_t start_ms = 0) : ms_(start_ms) {}
  int64_t now_ms() const override { return ms_.load(std::memory_order_relaxed); }
  void advance_ms(int64_t d) { ms_.fetch_add(d, std::memory_order_relaxed); }

 private:
  std::atomic<int64_t> ms_;
};

}  // namespace immd

