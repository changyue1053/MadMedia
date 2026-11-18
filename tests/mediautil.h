// 合成媒体构造器（e2e 用）：尾置 moov 的 MP4 / 解码炸弹 JPEG / 伪装可执行文件。
// 与 unit_mp4.cpp 的构造器同构（unit 侧保持独立以隔离回归面）。
#pragma once

#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

namespace immd_test {

inline void be32v(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24));
  v.push_back(uint8_t(x >> 16));
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}

inline std::vector<uint8_t> mp4_box(const char* type, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> v;
  be32v(v, uint32_t(8 + body.size()));
  v.insert(v.end(), type, type + 4);
  v.insert(v.end(), body.begin(), body.end());
  return v;
}

inline std::vector<uint8_t> mp4_cat(std::initializer_list<std::vector<uint8_t>> parts) {
  std::vector<uint8_t> v;
  for (auto& p : parts) v.insert(v.end(), p.begin(), p.end());
  return v;
}

// 尾置 moov 的最小合法 MP4：ftyp + mdat(payload) + moov(mvhd/trak/.../stco)。
// stco 指向 mdat 内 payload 的绝对偏移——remux 后可按新偏移做字节级验证。
inline std::vector<uint8_t> make_tail_moov_mp4(size_t payload_bytes, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> payload(payload_bytes);
  for (auto& b : payload) b = uint8_t(rng());
  std::vector<uint8_t> ftyp_body = {'i', 's', 'o', 'm'};
  be32v(ftyp_body, 0x200);
  ftyp_body.insert(ftyp_body.end(), {'i', 's', 'o', 'm'});
  auto ftyp = mp4_box("ftyp", ftyp_body);
  auto mdat = mp4_box("mdat", payload);
  uint32_t payload_off = uint32_t(ftyp.size() + 8);

  std::vector<uint8_t> mvhd_body;
  be32v(mvhd_body, 0);
  be32v(mvhd_body, 0);
  be32v(mvhd_body, 0);
  be32v(mvhd_body, 1000);  // timescale
  be32v(mvhd_body, 5000);  // duration = 5s
  std::vector<uint8_t> tkhd_body(20, 0);
  be32v(tkhd_body, 640u << 16);
  be32v(tkhd_body, 360u << 16);
  std::vector<uint8_t> stco_body;
  be32v(stco_body, 0);
  be32v(stco_body, 1);
  be32v(stco_body, payload_off);
  auto stbl = mp4_box("stbl", mp4_box("stco", stco_body));
  auto trak =
      mp4_box("trak", mp4_cat({mp4_box("tkhd", tkhd_body), mp4_box("mdia", mp4_box("minf", stbl))}));
  auto moov = mp4_box("moov", mp4_cat({mp4_box("mvhd", mvhd_body), trak}));
  return mp4_cat({ftyp, mdat, moov});
}

// 解码炸弹 JPEG：结构合法（SOI/SOF0/SOS/EOI），声明 60000×60000 像素（36 亿，解码即数 GB）。
inline std::vector<uint8_t> make_bomb_jpeg() {
  std::vector<uint8_t> f = {0xFF, 0xD8};  // SOI
  // SOF0：len=11, precision=8, height=60000(0xEA60), width=60000, 1 分量
  const uint8_t sof[] = {0xFF, 0xC0, 0x00, 0x0B, 0x08, 0xEA, 0x60, 0xEA, 0x60, 0x01, 0x01, 0x11, 0x00};
  f.insert(f.end(), sof, sof + sizeof(sof));
  const uint8_t sos[] = {0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00};
  f.insert(f.end(), sos, sos + sizeof(sos));
  f.insert(f.end(), {0x12, 0x34, 0x56});  // 熵数据占位
  f.insert(f.end(), {0xFF, 0xD9});        // EOI
  return f;
}

// 伪装文件：PE 可执行头（MZ），调用方声明成 image/jpeg —— 魔数嗅探必须纠偏。
inline std::vector<uint8_t> make_fake_exe(size_t bytes, uint32_t seed) {
  std::vector<uint8_t> f = {'M', 'Z', 0x90, 0x00, 0x03, 0x00, 0x00, 0x00};
  std::mt19937 rng(seed);
  while (f.size() < bytes) f.push_back(uint8_t(rng()));
  return f;
}

inline void write_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

}  // namespace immd_test

