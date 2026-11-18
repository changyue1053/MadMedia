// 弱网自适应（架构 §7）：EWMA 吞吐估计 → 目标时长分片决策；切网 reset() 重探。
// 注意 S3 语义：分片大小在 init 后对该上传会话固定——自适应作用于"下一个任务"与首选建议。
#pragma once

#include <algorithm>
#include <cstdint>

namespace immd {

class ThroughputEwma {
 public:
  explicit ThroughputEwma(double alpha = 0.3) : alpha_(alpha) {}

  void on_sample(int64_t bytes, int64_t elapsed_ms) {
    if (bytes <= 0 || elapsed_ms <= 0) return;
    double bps = double(bytes) * 1000.0 / double(elapsed_ms);
    bps_ = has_ ? alpha_ * bps + (1 - alpha_) * bps_ : bps;
    has_ = true;
  }
  void reset() {
    has_ = false;
    bps_ = 0;
  }
  bool has_sample() const { return has_; }
  double bytes_per_sec() const { return bps_; }

 private:
  double alpha_;
  bool has_ = false;
  double bps_ = 0;
};

// bps<=0（无样本）→ 返回 first（慢启动）；否则 clamp(bps * target_s, minb, maxb)。
inline int64_t suggest_part_size(double bps, int target_seconds, int64_t minb, int64_t maxb, int64_t first) {
  if (bps <= 0) return std::min(std::max(first, minb), maxb);
  int64_t want = static_cast<int64_t>(bps * target_seconds);
  return std::min(std::max(want, minb), maxb);
}

}  // namespace immd

