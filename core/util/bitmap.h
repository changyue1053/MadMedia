// 分片位图：断点续传的账本载体（架构 §10）。
// 端上位图只是缓存，服务端 received_parts 是真相源——union_with 即"以服务端为准"的收敛动作。
#pragma once

#include <cstdint>
#include <vector>

namespace immd {

class PartBitmap {
 public:
  PartBitmap() = default;
  explicit PartBitmap(uint32_t nparts) : n_(nparts), bits_((nparts + 7) / 8, 0) {}

  static PartBitmap from_blob(const std::vector<uint8_t>& blob, uint32_t nparts) {
    PartBitmap b(nparts);
    for (size_t i = 0; i < b.bits_.size() && i < blob.size(); ++i) b.bits_[i] = blob[i];
    return b;
  }

  void set(uint32_t i) {
    if (i < n_) bits_[i >> 3] |= uint8_t(1u << (i & 7));
  }
  void clear(uint32_t i) {
    if (i < n_) bits_[i >> 3] &= uint8_t(~(1u << (i & 7)));
  }
  bool get(uint32_t i) const { return i < n_ && (bits_[i >> 3] >> (i & 7)) & 1; }

  uint32_t nparts() const { return n_; }
  uint32_t count() const {
    uint32_t c = 0;
    for (uint32_t i = 0; i < n_; ++i) c += get(i);
    return c;
  }
  bool complete() const { return n_ > 0 && count() == n_; }

  std::vector<uint32_t> missing() const {
    std::vector<uint32_t> m;
    for (uint32_t i = 0; i < n_; ++i)
      if (!get(i)) m.push_back(i);
    return m;
  }

  void union_with(const PartBitmap& o) {
    for (size_t i = 0; i < bits_.size() && i < o.bits_.size(); ++i) bits_[i] |= o.bits_[i];
  }

  const std::vector<uint8_t>& blob() const { return bits_; }

 private:
  uint32_t n_ = 0;
  std::vector<uint8_t> bits_;
};

}  // namespace immd

