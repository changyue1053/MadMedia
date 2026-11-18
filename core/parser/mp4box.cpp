#include "core/parser/mp4box.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <set>

namespace immd {

namespace {

constexpr int kMaxDepth = 16;
constexpr int kMaxBoxes = 8192;  // fuzz CPU 上限：畸形文件不允许拖住解析器

// 带边界检查的只读 reader：parser 模块唯一的字节访问方式（无指针算术裸奔）。
struct Reader {
  const uint8_t* p;
  size_t len;

  bool ok(size_t off, size_t n) const { return off <= len && n <= len - off; }
  uint32_t u32(size_t off) const {
    return (uint32_t(p[off]) << 24) | (uint32_t(p[off + 1]) << 16) | (uint32_t(p[off + 2]) << 8) |
           uint32_t(p[off + 3]);
  }
  uint64_t u64(size_t off) const { return (uint64_t(u32(off)) << 32) | u32(off + 4); }
  std::string fourcc(size_t off) const { return std::string(reinterpret_cast<const char*>(p + off), 4); }
};

struct Box {
  std::string type;
  size_t off;        // box 起始（含头）
  size_t header;     // 头长（8 或 16）
  size_t size;       // 总长（含头）
  size_t body_off() const { return off + header; }
  size_t body_size() const { return size - header; }
};

// 解析 [off, end) 内的一个 box 头；畸形返回 false。
bool read_box(const Reader& r, size_t off, size_t end, Box* b) {
  if (off + 8 > end || !r.ok(off, 8)) return false;
  uint64_t size = r.u32(off);
  size_t header = 8;
  std::string type = r.fourcc(off + 4);
  if (size == 1) {  // largesize
    if (off + 16 > end || !r.ok(off, 16)) return false;
    size = r.u64(off + 8);
    header = 16;
  } else if (size == 0) {  // 到文件尾
    size = end - off;
  }
  if (size < header || size > end - off) return false;
  for (char c : type)  // fourcc 必须可打印（粗校验，挡纯垃圾）
    if (uint8_t(c) < 0x20 || uint8_t(c) > 0x7e) return false;
  b->type = type;
  b->off = off;
  b->header = header;
  b->size = size_t(size);
  return true;
}

bool is_container(const std::string& t) {
  static const char* kC[] = {"moov", "trak", "mdia", "minf", "stbl", "edts", "dinf", "udta", "mvex"};
  for (auto* c : kC)
    if (t == c) return true;
  return false;
}

// 深度优先遍历；cb 返回 false 表示终止遍历。
bool walk(const Reader& r, size_t off, size_t end, int depth, int* budget,
          const std::function<bool(const Box&, int)>& cb) {
  if (depth > kMaxDepth) return true;  // 超深：不再深入（不算错误，防栈）
  while (off + 8 <= end) {
    if (--(*budget) < 0) return false;
    Box b;
    if (!read_box(r, off, end, &b)) return false;
    if (!cb(b, depth)) return false;
    if (is_container(b.type)) {
      if (!walk(r, b.body_off(), b.off + b.size, depth + 1, budget, cb)) return false;
    }
    off = b.off + b.size;
  }
  return true;
}

}  // namespace

Expected<Mp4Info> mp4_inspect(const uint8_t* data, size_t len) {
  if (len < 8) return make_err(Err::Protocol, "mp4: too short");
  Reader r{data, len};
  Mp4Info info;
  int budget = kMaxBoxes;
  bool sane = walk(r, 0, len, 0, &budget, [&](const Box& b, int depth) {
    if (depth == 0) {
      if (b.type == "ftyp") {
        info.has_ftyp = true;
        if (b.body_size() >= 4) info.major_brand = r.fourcc(b.body_off());
      } else if (b.type == "moov") {
        info.has_moov = true;
        info.moov_off = int64_t(b.off);
        info.moov_size = int64_t(b.size);
      } else if (b.type == "mdat" && info.mdat_off < 0) {
        info.has_mdat = true;
        info.mdat_off = int64_t(b.off);
      }
    }
    if (b.type == "mvhd" && b.body_size() >= 20) {
      size_t o = b.body_off();
      uint8_t version = r.p[o];
      if (version == 0 && b.body_size() >= 4 + 4 + 4 + 4 + 4) {
        uint32_t timescale = r.u32(o + 12), duration = r.u32(o + 16);
        if (timescale > 0) info.duration_ms = int64_t(duration) * 1000 / timescale;
      } else if (version == 1 && b.body_size() >= 4 + 8 + 8 + 4 + 8) {
        uint32_t timescale = r.u32(o + 20);
        uint64_t duration = r.u64(o + 24);
        if (timescale > 0) info.duration_ms = int64_t(duration * 1000 / timescale);
      }
    }
    if (b.type == "tkhd" && info.width == 0 && b.body_size() >= 16) {
      // width/height 是 tkhd 的最后两个 16.16 定点字段——对 v0/v1 布局均成立
      size_t tail = b.off + b.size;
      if (r.ok(tail - 8, 8)) {
        uint32_t w = r.u32(tail - 8) >> 16, h = r.u32(tail - 4) >> 16;
        if (w > 0 && h > 0) {
          info.width = w;
          info.height = h;
        }
      }
    }
    return true;
  });
  if (!sane && !(info.has_moov || info.has_mdat)) return make_err(Err::Protocol, "mp4: malformed boxes");
  if (!info.has_moov && !info.has_mdat) return make_err(Err::Protocol, "mp4: no moov/mdat");
  info.faststart = info.has_moov && info.has_mdat && info.moov_off < info.mdat_off;
  return info;
}

namespace {

/// moov 内的 chunk-offset box 落点（升级判定与重建的输入）。
struct ChunkBox {
  size_t off = 0;  // moov 缓冲内 box 起始
  bool is_stco = false;
  uint32_t count = 0;
  uint64_t max_entry = 0;
};

void put_u32(std::vector<uint8_t>& v, uint64_t x) {
  v.push_back(uint8_t(x >> 24));
  v.push_back(uint8_t(x >> 16));
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}
void put_u64(std::vector<uint8_t>& v, uint64_t x) {
  for (int k = 0; k < 8; ++k) v.push_back(uint8_t(x >> (56 - k * 8)));
}

// 重建 moov 子树：升级集内的 stco 改写为 co64（值 +delta），其余 chunk box 原型修补，
// 容器盒尺寸随子树重算。返回 false 即结构非法（外层转 Protocol 错误）。
bool rebuild_subtree(const Reader& r, size_t off, size_t end, uint64_t delta,
                     const std::set<size_t>& upgrades, int depth, int* budget,
                     std::vector<uint8_t>* out) {
  if (depth > kMaxDepth) return false;
  while (off + 8 <= end) {
    if (--(*budget) < 0) return false;
    Box b;
    if (!read_box(r, off, end, &b)) return false;
    if (is_container(b.type)) {
      std::vector<uint8_t> children;
      if (!rebuild_subtree(r, b.body_off(), b.off + b.size, delta, upgrades, depth + 1, budget,
                           &children))
        return false;
      uint64_t new_size = uint64_t(b.header) + children.size();
      if (b.header == 8) {
        if (new_size > 0xFFFFFFFFull) return false;  // moov 本体 >4GB：不属于本函数的现实域
        put_u32(*out, new_size);
        out->insert(out->end(), b.type.begin(), b.type.end());
      } else {  // largesize 形态保形
        put_u32(*out, 1);
        out->insert(out->end(), b.type.begin(), b.type.end());
        put_u64(*out, new_size);
      }
      out->insert(out->end(), children.begin(), children.end());
    } else if (b.type == "stco" || b.type == "co64") {
      if (b.body_size() < 8) return false;
      size_t o = b.body_off();
      uint32_t n = r.u32(o + 4);
      size_t esz = (b.type == "stco") ? 4 : 8;
      if (uint64_t(n) * esz + 8 > b.body_size()) return false;
      bool upgrade = b.type == "stco" && upgrades.count(b.off) > 0;
      size_t new_esz = upgrade ? 8 : esz;
      uint64_t new_size = uint64_t(b.header) + 8 + uint64_t(n) * new_esz;
      if (b.header == 8) {
        put_u32(*out, new_size);
      } else {
        // largesize 头的 chunk box 极罕见：保形输出
        put_u32(*out, 1);
      }
      const char* type = (upgrade || esz == 8) ? "co64" : "stco";
      out->insert(out->end(), type, type + 4);
      if (b.header == 16) put_u64(*out, new_size);
      put_u32(*out, r.u32(o));      // version/flags 原样
      put_u32(*out, n);             // entry_count
      for (uint32_t i = 0; i < n; ++i) {
        size_t eo = o + 8 + size_t(i) * esz;
        uint64_t v = (esz == 4) ? r.u32(eo) : r.u64(eo);
        v += delta;
        if (new_esz == 4) {
          if (v > 0xFFFFFFFFull) return false;  // 定点集算漏了才会到这（防御断言位）
          put_u32(*out, v);
        } else {
          put_u64(*out, v);
        }
      }
    } else {
      out->insert(out->end(), r.p + b.off, r.p + b.off + b.size);  // 其余 box 原样
    }
    off = b.off + b.size;
  }
  return true;
}

}  // namespace

Error mp4_faststart(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
  auto insp = mp4_inspect(in.data(), in.size());
  if (!insp.ok()) return insp.error();
  const Mp4Info& info = insp.value();
  if (!info.has_moov || !info.has_mdat) return make_err(Err::Protocol, "mp4: incomplete");
  if (info.faststart) {  // 幂等：已就位原样返回
    *out = in;
    return {};
  }
  Reader r{in.data(), in.size()};

  // 1) 拷贝 moov 子树，拒绝压缩 moov（cmov：偏移在压缩数据里，无法流修补 → 交转码兜底）
  std::vector<uint8_t> moov(in.begin() + info.moov_off, in.begin() + info.moov_off + info.moov_size);
  Reader mr{moov.data(), moov.size()};
  std::vector<ChunkBox> chunk_boxes;
  {
    int budget = kMaxBoxes;
    bool has_cmov = false;
    Error scan_err;
    walk(mr, 0, moov.size(), 0, &budget, [&](const Box& b, int) {
      if (b.type == "cmov") {
        has_cmov = true;
        return false;
      }
      if (b.type == "stco" || b.type == "co64") {
        if (b.body_size() < 8) {
          scan_err = make_err(Err::Protocol, "mp4: bad chunk offset box");
          return false;
        }
        size_t o = b.body_off();
        uint32_t n = mr.u32(o + 4);
        size_t esz = (b.type == "stco") ? 4 : 8;
        if (uint64_t(n) * esz + 8 > b.body_size()) {
          scan_err = make_err(Err::Protocol, "mp4: chunk offset count overflow");
          return false;
        }
        ChunkBox cb;
        cb.off = b.off;
        cb.is_stco = (b.type == "stco");
        cb.count = n;
        for (uint32_t i = 0; i < n; ++i) {
          uint64_t v = (esz == 4) ? mr.u32(o + 8 + size_t(i) * 4) : mr.u64(o + 8 + size_t(i) * 8);
          cb.max_entry = std::max(cb.max_entry, v);
        }
        chunk_boxes.push_back(cb);
      }
      return true;
    });
    if (has_cmov) return make_err(Err::Protocol, "mp4: compressed moov unsupported");
    if (scan_err) return scan_err;
  }

  // 2) 定点迭代求升级集：delta = 原 moov 长 + 升级增量（co64 每项多 4B）；
  //    delta 变大又可能把新的 stco 推过 u32 上限 → 循环至稳定（单调增，必收敛）。
  std::set<size_t> upgrades;
  uint64_t delta = uint64_t(info.moov_size);
  bool grew = true;
  while (grew) {  // 单调增集合，至多迭代 stco 个数轮
    grew = false;
    uint64_t added = 0;
    for (const auto& cb : chunk_boxes)
      if (upgrades.count(cb.off)) added += 4ull * cb.count;
    delta = uint64_t(info.moov_size) + added;
    for (const auto& cb : chunk_boxes) {
      if (cb.is_stco && !upgrades.count(cb.off) && cb.max_entry + delta > 0xFFFFFFFFull) {
        upgrades.insert(cb.off);
        grew = true;  // 升级集变化 → 下一轮重算 delta 再判
      }
    }
  }

  // 3) 重建 moov（无升级时退化为等长改写，行为与旧实现一致）
  std::vector<uint8_t> new_moov;
  new_moov.reserve(moov.size() + 64);
  {
    int budget = kMaxBoxes;
    if (!rebuild_subtree(mr, 0, moov.size(), delta, upgrades, 0, &budget, &new_moov))
      return make_err(Err::Protocol, "mp4: moov rebuild failed");
  }
  // 不变量：重建后的 moov 长度必须恰好等于 delta（定点计算与重建器互为校验）
  if (new_moov.size() != delta)
    return make_err(Err::Protocol, "mp4: rebuilt moov size mismatch (fixpoint bug)");
  const std::vector<uint8_t>& moov_out = new_moov;

  // 3) 重排输出：ftyp（若有，保持最前）→ 修补后的 moov → 其余 box 按原序（跳过原 moov）
  out->clear();
  out->reserve(in.size());
  size_t off = 0;
  int guard = kMaxBoxes;
  // 先 ftyp
  while (off + 8 <= in.size() && --guard > 0) {
    Box b;
    if (!read_box(r, off, in.size(), &b)) return make_err(Err::Protocol, "mp4: malformed top-level");
    if (b.type == "ftyp") {
      out->insert(out->end(), in.begin() + b.off, in.begin() + b.off + b.size);
      break;
    }
    off = b.off + b.size;
  }
  out->insert(out->end(), moov_out.begin(), moov_out.end());
  off = 0;
  guard = kMaxBoxes;
  while (off + 8 <= in.size() && --guard > 0) {
    Box b;
    if (!read_box(r, off, in.size(), &b)) return make_err(Err::Protocol, "mp4: malformed top-level");
    if (b.type != "ftyp" && b.type != "moov")
      out->insert(out->end(), in.begin() + b.off, in.begin() + b.off + b.size);
    off = b.off + b.size;
  }
  return {};
}

}  // namespace immd

