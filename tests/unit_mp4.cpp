#include <cstring>
#include <random>
#include <vector>

#include "core/parser/mp4box.h"
#include "tests/harness.h"

using namespace immd;

// ---------- 测试用 MP4 构造器（自包含，不依赖 ffmpeg）----------
namespace {

void be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24));
  v.push_back(uint8_t(x >> 16));
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}

std::vector<uint8_t> box(const char* type, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> v;
  be32(v, uint32_t(8 + body.size()));
  v.insert(v.end(), type, type + 4);
  v.insert(v.end(), body.begin(), body.end());
  return v;
}

std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> parts) {
  std::vector<uint8_t> v;
  for (auto& p : parts) v.insert(v.end(), p.begin(), p.end());
  return v;
}

std::vector<uint8_t> make_ftyp() {
  std::vector<uint8_t> body;
  body.insert(body.end(), {'i', 's', 'o', 'm'});
  be32(body, 0x200);
  body.insert(body.end(), {'i', 's', 'o', 'm'});
  return box("ftyp", body);
}

std::vector<uint8_t> make_mvhd(uint32_t timescale, uint32_t duration) {
  std::vector<uint8_t> body;
  be32(body, 0);  // version 0 + flags
  be32(body, 0);  // ctime
  be32(body, 0);  // mtime
  be32(body, timescale);
  be32(body, duration);
  return box("mvhd", body);
}

std::vector<uint8_t> make_tkhd(uint32_t w, uint32_t h) {
  std::vector<uint8_t> body(20, 0);  // 前段字段不关心
  be32(body, w << 16);               // 16.16 定点，位于 box 末尾
  be32(body, h << 16);
  return box("tkhd", body);
}

std::vector<uint8_t> make_stco(std::vector<uint32_t> entries) {
  std::vector<uint8_t> body;
  be32(body, 0);
  be32(body, uint32_t(entries.size()));
  for (auto e : entries) be32(body, e);
  return box("stco", body);
}

// 尾置 moov 的标准合成文件：ftyp + mdat(payload) + moov(mvhd + trak(tkhd + mdia/minf/stbl/stco))
struct Synth {
  std::vector<uint8_t> file;
  std::vector<uint8_t> payload;
  uint32_t payload_off = 0;
  int64_t moov_size = 0;
};

Synth make_tail_moov_mp4() {
  Synth s;
  std::mt19937 rng(7);
  s.payload.resize(64);
  for (auto& b : s.payload) b = uint8_t(rng());
  auto ftyp = make_ftyp();
  auto mdat = box("mdat", s.payload);
  s.payload_off = uint32_t(ftyp.size() + 8);  // mdat 头后
  auto stbl = box("stbl", make_stco({s.payload_off}));
  auto minf = box("minf", stbl);
  auto mdia = box("mdia", minf);
  auto trak = box("trak", cat({make_tkhd(640, 360), mdia}));
  auto moov = box("moov", cat({make_mvhd(1000, 5000), trak}));
  s.moov_size = int64_t(moov.size());
  s.file = cat({ftyp, mdat, moov});
  return s;
}

uint32_t find_stco_entry(const std::vector<uint8_t>& f) {
  for (size_t i = 0; i + 16 <= f.size(); ++i) {
    if (f[i] == 's' && f[i + 1] == 't' && f[i + 2] == 'c' && f[i + 3] == 'o') {
      size_t o = i + 4 + 8;  // fourcc 后：version/flags + count，再到首个 entry
      return (uint32_t(f[o]) << 24) | (uint32_t(f[o + 1]) << 16) | (uint32_t(f[o + 2]) << 8) | f[o + 3];
    }
  }
  return 0;
}

// ---------- co64 升级测试的多 trak 构造 ----------

std::vector<uint8_t> catv(const std::vector<std::vector<uint8_t>>& parts) {
  std::vector<uint8_t> v;
  for (auto& p : parts) v.insert(v.end(), p.begin(), p.end());
  return v;
}

std::vector<uint8_t> make_trak(std::vector<uint32_t> stco_entries) {
  auto stbl = box("stbl", make_stco(std::move(stco_entries)));
  return box("trak", cat({make_tkhd(640, 360), box("mdia", box("minf", stbl))}));
}

struct MultiSynth {
  std::vector<uint8_t> file;
  std::vector<uint8_t> payload;
  uint32_t payload_off = 0;
  size_t moov_size = 0;  // 原始 moov 长（升级前的位移基数）
};

// 尾置 moov、每 trak 一组自定义 stco entries。entries 值可为"假偏移"（测数值修补语义）。
MultiSynth make_multi(const std::vector<std::vector<uint32_t>>& traks) {
  MultiSynth s;
  std::mt19937 rng(7);
  s.payload.resize(64);
  for (auto& b : s.payload) b = uint8_t(rng());
  auto ftyp = make_ftyp();
  auto mdat = box("mdat", s.payload);
  s.payload_off = uint32_t(ftyp.size() + 8);
  std::vector<std::vector<uint8_t>> moov_parts = {make_mvhd(1000, 5000)};
  for (auto& t : traks) moov_parts.push_back(make_trak(t));
  auto moov = box("moov", catv(moov_parts));
  s.moov_size = moov.size();
  s.file = cat({ftyp, mdat, moov});
  return s;
}

// 在 [from, to) 窗口内找第 nth 个 fourcc，返回 fourcc 起始位置（无则 npos）。
size_t find_fourcc(const std::vector<uint8_t>& f, size_t from, size_t to, const char* cc, int nth = 1) {
  for (size_t i = from; i + 4 <= to && i + 4 <= f.size(); ++i) {
    if (memcmp(f.data() + i, cc, 4) == 0 && --nth == 0) return i;
  }
  return size_t(-1);
}

uint64_t u64_at(const std::vector<uint8_t>& f, size_t o) {
  uint64_t v = 0;
  for (int k = 0; k < 8; ++k) v = (v << 8) | f[o + k];
  return v;
}

uint32_t u32_at(const std::vector<uint8_t>& f, size_t o) {
  return (uint32_t(f[o]) << 24) | (uint32_t(f[o + 1]) << 16) | (uint32_t(f[o + 2]) << 8) | f[o + 3];
}

}  // namespace

TEST(mp4, inspect_extracts_meta_and_layout) {
  auto s = make_tail_moov_mp4();
  auto info = mp4_inspect(s.file.data(), s.file.size());
  ASSERT_TRUE(info.ok());
  auto& i = info.value();
  ASSERT_TRUE(i.has_ftyp);
  ASSERT_EQ(i.major_brand, std::string("isom"));
  ASSERT_TRUE(i.has_moov);
  ASSERT_TRUE(i.has_mdat);
  ASSERT_FALSE(i.faststart);  // 尾置 moov
  ASSERT_EQ(i.duration_ms, 5000);
  ASSERT_EQ(i.width, 640u);
  ASSERT_EQ(i.height, 360u);
}

TEST(mp4, faststart_moves_moov_and_patches_stco) {
  auto s = make_tail_moov_mp4();
  std::vector<uint8_t> out;
  ASSERT_FALSE(mp4_faststart(s.file, &out));
  ASSERT_EQ(out.size(), s.file.size());  // 纯重排，长度不变

  auto info = mp4_inspect(out.data(), out.size());
  ASSERT_TRUE(info.ok());
  ASSERT_TRUE(info.value().faststart);  // moov 已前置
  ASSERT_EQ(info.value().duration_ms, 5000);

  // stco 修补：entry = 原偏移 + moov 大小；且新偏移处的字节就是原 payload
  uint32_t entry = find_stco_entry(out);
  ASSERT_EQ(entry, s.payload_off + uint32_t(s.moov_size));
  ASSERT_TRUE(entry + s.payload.size() <= out.size());
  for (size_t k = 0; k < s.payload.size(); ++k) ASSERT_EQ(out[entry + k], s.payload[k]);
}

TEST(mp4, faststart_is_idempotent) {
  auto s = make_tail_moov_mp4();
  std::vector<uint8_t> once, twice;
  ASSERT_FALSE(mp4_faststart(s.file, &once));
  ASSERT_FALSE(mp4_faststart(once, &twice));
  ASSERT_TRUE(once == twice);  // 已 faststart → 原样返回
}

TEST(mp4, compressed_moov_rejected) {
  auto s = make_tail_moov_mp4();
  // 构造含 cmov 的 moov：应显式拒绝（交服务端转码兜底）
  auto moov = box("moov", cat({make_mvhd(1000, 1000), box("cmov", std::vector<uint8_t>(8, 0))}));
  auto f = cat({make_ftyp(), box("mdat", std::vector<uint8_t>(16, 0)), moov});
  std::vector<uint8_t> out;
  Error e = mp4_faststart(f, &out);
  ASSERT_TRUE(bool(e));
  ASSERT_EQ(int(e.code), int(Err::Protocol));
}

TEST(mp4, co64_upgrade_when_offset_overflows) {
  // 假偏移贴近 u32 上限（余量 15B < delta）：前移后溢出 → stco 必须就地升级为 co64
  const uint32_t kNearMax = 0xFFFFFFF0u;
  auto s = make_multi({{kNearMax}});
  std::vector<uint8_t> out;
  ASSERT_FALSE(mp4_faststart(s.file, &out));

  uint64_t delta = uint64_t(s.moov_size) + 4;  // 1 项 stco→co64：moov 增 4 字节
  ASSERT_EQ(out.size(), s.file.size() + 4);

  // moov 现在紧跟 ftyp，窗口内应只有 co64、没有 stco
  size_t moov_from = 20, moov_to = moov_from + size_t(delta);  // ftyp 20 字节
  ASSERT_EQ(find_fourcc(out, moov_from, moov_to, "stco"), size_t(-1));
  size_t co = find_fourcc(out, moov_from, moov_to, "co64");
  ASSERT_NE(co, size_t(-1));
  ASSERT_EQ(u32_at(out, co + 4 + 4), 1u);                       // entry_count
  ASSERT_EQ(u64_at(out, co + 4 + 8), uint64_t(kNearMax) + delta);  // 64 位修补值

  auto info = mp4_inspect(out.data(), out.size());
  ASSERT_TRUE(info.ok());
  ASSERT_TRUE(info.value().faststart);  // 容器盒尺寸链修补正确（否则 walk 会崩解）

  // 幂等：升级产物再过一遍 = 原样
  std::vector<uint8_t> again;
  ASSERT_FALSE(mp4_faststart(out, &again));
  ASSERT_TRUE(again == out);
}

TEST(mp4, mixed_traks_upgrade_only_overflowing_and_keep_payload_valid) {
  // trak1 真偏移（可解引用验证字节）、trak2 假大偏移（触发升级）——
  // 定点交互的关键断言：trak1 的修补量必须包含 trak2 升级带来的 +4
  const uint32_t kNearMax = 0xFFFFFFF0u;
  auto probe = make_multi({{0}, {kNearMax}});  // 先建一次拿 payload_off（entries 值不影响布局）
  auto s = make_multi({{probe.payload_off}, {kNearMax}});
  std::vector<uint8_t> out;
  ASSERT_FALSE(mp4_faststart(s.file, &out));

  uint64_t delta = uint64_t(s.moov_size) + 4;
  size_t moov_from = 20, moov_to = moov_from + size_t(delta);
  // trak1 仍是 stco（未溢出不升级），值 = 原偏移 + delta（含 trak2 的 +4）
  size_t st = find_fourcc(out, moov_from, moov_to, "stco");
  ASSERT_NE(st, size_t(-1));
  uint32_t patched = u32_at(out, st + 4 + 8);
  ASSERT_EQ(patched, s.payload_off + uint32_t(delta));
  // 新偏移处就是原 payload 字节（语义级验证，不止数值）
  ASSERT_TRUE(patched + s.payload.size() <= out.size());
  for (size_t k = 0; k < s.payload.size(); ++k) ASSERT_EQ(out[patched + k], s.payload[k]);
  // trak2 已是 co64
  size_t co = find_fourcc(out, moov_from, moov_to, "co64");
  ASSERT_NE(co, size_t(-1));
  ASSERT_EQ(u64_at(out, co + 4 + 8), uint64_t(kNearMax) + delta);
  // 窗口内不得再有第二个 stco
  ASSERT_EQ(find_fourcc(out, moov_from, moov_to, "stco", 2), size_t(-1));
}

TEST(mp4, cascading_upgrade_fixpoint_converges) {
  // 级联：trak2 第一轮就溢出（+4 后）把 delta 推大，恰好使 trak1 第二轮也溢出。
  // entry1 = max - base - 3：+base ≤ max（第一轮不升级），+base+4 > max（第二轮升级）
  auto probe = make_multi({{0}, {0}});
  uint64_t base = probe.moov_size;
  uint32_t entry1 = uint32_t(0xFFFFFFFFull - base - 3);
  uint32_t entry2 = uint32_t(0xFFFFFFFFull - base + 1);  // 第一轮即溢出
  auto s = make_multi({{entry1}, {entry2}});
  ASSERT_EQ(uint64_t(s.moov_size), base);
  std::vector<uint8_t> out;
  ASSERT_FALSE(mp4_faststart(s.file, &out));

  uint64_t delta = base + 8;  // 两个 box 各 1 项升级
  ASSERT_EQ(out.size(), s.file.size() + 8);
  size_t moov_from = 20, moov_to = moov_from + size_t(delta);
  ASSERT_EQ(find_fourcc(out, moov_from, moov_to, "stco"), size_t(-1));  // 全部升级
  size_t co1 = find_fourcc(out, moov_from, moov_to, "co64", 1);
  size_t co2 = find_fourcc(out, moov_from, moov_to, "co64", 2);
  ASSERT_NE(co1, size_t(-1));
  ASSERT_NE(co2, size_t(-1));
  ASSERT_EQ(u64_at(out, co1 + 4 + 8), uint64_t(entry1) + delta);
  ASSERT_EQ(u64_at(out, co2 + 4 + 8), uint64_t(entry2) + delta);
  auto info = mp4_inspect(out.data(), out.size());
  ASSERT_TRUE(info.ok());
  ASSERT_TRUE(info.value().faststart);
}

TEST(mp4, malformed_inputs_error_not_crash) {
  const std::vector<std::vector<uint8_t>> bad = {
      {},                                  // 空
      {0, 0, 0, 1},                        // 太短
      cat({box("free", {})}),              // 无 moov/mdat
      [] {                                 // box size 越界
        std::vector<uint8_t> v;
        be32(v, 0xFFFFFF);
        v.insert(v.end(), {'m', 'o', 'o', 'v'});
        return v;
      }(),
  };
  for (auto& f : bad) {
    auto r = mp4_inspect(f.data(), f.size());
    ASSERT_FALSE(r.ok());
  }
}

TEST(mp4, fuzz_lite_never_crashes) {
  auto s = make_tail_moov_mp4();
  std::mt19937 rng(20260831);
  for (int i = 0; i < 3000; ++i) {
    auto f = s.file;
    int muts = 1 + rng() % 8;
    for (int m = 0; m < muts; ++m) {
      size_t pos = rng() % f.size();
      switch (rng() % 3) {
        case 0: f[pos] = uint8_t(rng()); break;
        case 1: f.insert(f.begin() + pos, uint8_t(rng())); break;
        case 2: f.erase(f.begin() + pos); break;
      }
    }
    mp4_inspect(f.data(), f.size());  // 只要求不崩溃不越界（ASan/UBSan 矩阵同跑）
    std::vector<uint8_t> out;
    mp4_faststart(f, &out);
  }
  for (int i = 0; i < 1000; ++i) {  // 纯垃圾
    std::vector<uint8_t> f(rng() % 512);
    for (auto& b : f) b = uint8_t(rng());
    mp4_inspect(f.data(), f.size());
  }
}

