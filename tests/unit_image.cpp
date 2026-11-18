#include <cstring>
#include <random>
#include <vector>

#include "core/parser/imagemeta.h"
#include "tests/harness.h"

using namespace immd;

// ---------- 合成 JPEG 构造器（含 EXIF 方向 + GPS IFD）----------
namespace {

void push16be(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}

std::vector<uint8_t> seg(uint8_t marker, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> v{0xFF, marker};
  push16be(v, uint16_t(body.size() + 2));
  v.insert(v.end(), body.begin(), body.end());
  return v;
}

std::vector<uint8_t> exif_app1(int orientation, bool with_gps) {
  // TIFF（LE）：IFD0 @8（2 项：Orientation + GPS 指针），GPS IFD @38
  std::vector<uint8_t> t = {'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00};
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t cnt, uint32_t val) {
    t.push_back(uint8_t(tag));
    t.push_back(uint8_t(tag >> 8));
    t.push_back(uint8_t(type));
    t.push_back(uint8_t(type >> 8));
    for (int i = 0; i < 4; ++i) t.push_back(uint8_t(cnt >> (8 * i)));
    for (int i = 0; i < 4; ++i) t.push_back(uint8_t(val >> (8 * i)));
  };
  uint16_t n = with_gps ? 2 : 1;
  t.push_back(uint8_t(n));
  t.push_back(0);
  entry(0x0112, 3, 1, uint32_t(orientation));      // Orientation SHORT
  if (with_gps) entry(0x8825, 4, 1, 38);           // GPS IFD 指针 → offset 38
  t.insert(t.end(), {0, 0, 0, 0});                 // next IFD = 0
  if (with_gps) {                                  // GPS IFD @38
    t.push_back(1);
    t.push_back(0);
    entry(0x0001, 2, 2, uint32_t('N'));            // GPSLatitudeRef "N"
    t.insert(t.end(), {0, 0, 0, 0});
  }
  std::vector<uint8_t> body = {'E', 'x', 'i', 'f', 0, 0};
  body.insert(body.end(), t.begin(), t.end());
  return seg(0xE1, body);
}

std::vector<uint8_t> make_jpeg(uint16_t w, uint16_t h, int orientation, bool gps) {
  std::vector<uint8_t> f = {0xFF, 0xD8};
  auto app0 = seg(0xE0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
  f.insert(f.end(), app0.begin(), app0.end());
  auto app1 = exif_app1(orientation, gps);
  f.insert(f.end(), app1.begin(), app1.end());
  auto com = seg(0xFE, {'s', 'e', 'c', 'r', 'e', 't'});  // 注释段：剥离目标
  f.insert(f.end(), com.begin(), com.end());
  std::vector<uint8_t> sof = {8};
  push16be(sof, h);
  push16be(sof, w);
  sof.insert(sof.end(), {1, 0x01, 0x11, 0x00});
  auto sof0 = seg(0xC0, sof);
  f.insert(f.end(), sof0.begin(), sof0.end());
  auto sos = seg(0xDA, {1, 0x01, 0x00, 0x00, 0x3F, 0x00});
  f.insert(f.end(), sos.begin(), sos.end());
  f.insert(f.end(), {0x12, 0x34, 0x56, 0x78, 0x9A});  // 熵数据（无 0xFF）
  f.insert(f.end(), {0xFF, 0xD9});                    // EOI
  return f;
}

bool contains(const std::vector<uint8_t>& hay, const char* needle) {
  size_t n = strlen(needle);
  for (size_t i = 0; i + n <= hay.size(); ++i)
    if (memcmp(hay.data() + i, needle, n) == 0) return true;
  return false;
}

}  // namespace

TEST(image, sniff_magic_table) {
  struct Case {
    std::vector<uint8_t> bytes;
    const char* mime;
  };
  std::vector<Case> cases = {
      {{0xFF, 0xD8, 0xFF, 0xE0}, "image/jpeg"},
      {{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}, "image/png"},
      {{'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0}, "image/gif"},
      {{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'}, "image/webp"},
      {{0, 0, 0, 24, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c'}, "image/heic"},
      {{0, 0, 0, 24, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm'}, "video/mp4"},
      {{'%', 'P', 'D', 'F', '-'}, "application/pdf"},
      {{'P', 'K', 3, 4, 0}, "application/zip"},
      {{'M', 'Z', 0x90, 0}, "application/x-msdownload"},
  };
  for (auto& c : cases) ASSERT_EQ(sniff_mime(c.bytes.data(), c.bytes.size()), std::string(c.mime));
  // 高危文本类：改后缀伪装不了魔数嗅探（架构 §14 存储型 XSS 面）
  std::string svg = "  <svg xmlns='...'>";
  ASSERT_EQ(sniff_mime(reinterpret_cast<const uint8_t*>(svg.data()), svg.size()),
            std::string("image/svg+xml"));
  std::string html = "<!DOCTYPE html><html>";
  ASSERT_EQ(sniff_mime(reinterpret_cast<const uint8_t*>(html.data()), html.size()),
            std::string("text/html"));
}

TEST(image, jpeg_head_orientation_gps_dims) {
  auto f = make_jpeg(100, 50, 6, true);
  auto h = parse_image_head(f.data(), f.size());
  ASSERT_TRUE(h.ok());
  ASSERT_EQ(int(h.value().kind), int(ImageKind::Jpeg));
  ASSERT_EQ(h.value().width, 100u);
  ASSERT_EQ(h.value().height, 50u);
  ASSERT_EQ(h.value().orientation, 6);
  ASSERT_TRUE(h.value().has_gps);
  ASSERT_FALSE(h.value().bomb);
}

TEST(image, decode_bomb_flagged_before_decode) {
  auto f = make_jpeg(60000, 60000, 1, false);  // 声明 36 亿像素
  auto h = parse_image_head(f.data(), f.size());
  ASSERT_TRUE(h.ok());
  ASSERT_TRUE(h.value().bomb);  // 解码前拒绝（架构 §12：1KB 文件不允许换 3.6GB 内存）
}

TEST(image, png_gif_webp_heic_dims) {
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13,
                              'I', 'H', 'D', 'R', 0, 0, 1, 0, 0, 0, 0, 200};  // 256×200
  auto hp = parse_image_head(png.data(), png.size());
  ASSERT_TRUE(hp.ok());
  ASSERT_EQ(hp.value().width, 256u);
  ASSERT_EQ(hp.value().height, 200u);

  std::vector<uint8_t> gif = {'G', 'I', 'F', '8', '9', 'a', 0x40, 0x01, 0xC8, 0x00};  // 320×200
  auto hg = parse_image_head(gif.data(), gif.size());
  ASSERT_TRUE(hg.ok());
  ASSERT_EQ(hg.value().width, 320u);
  ASSERT_EQ(hg.value().height, 200u);

  // WebP VP8X：canvas 512×256（24bit LE，值-1）
  std::vector<uint8_t> webp = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P',
                               'V', 'P', '8', 'X', 10, 0, 0, 0, 0, 0, 0, 0,
                               0xFF, 0x01, 0x00, 0xFF, 0x00, 0x00};
  auto hw = parse_image_head(webp.data(), webp.size());
  ASSERT_TRUE(hw.ok());
  ASSERT_EQ(hw.value().width, 512u);
  ASSERT_EQ(hw.value().height, 256u);

  // HEIC：ftyp heic + ispe（size=20 定长盒）
  std::vector<uint8_t> heic = {0, 0, 0, 16, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c', 0, 0, 0, 0,
                               0, 0, 0, 20, 'i', 's', 'p', 'e', 0, 0, 0, 0,
                               0, 0, 0x0F, 0x00, 0, 0, 0x0B, 0x40};  // 3840×2880
  auto hh = parse_image_head(heic.data(), heic.size());
  ASSERT_TRUE(hh.ok());
  ASSERT_EQ(hh.value().width, 3840u);
  ASSERT_EQ(hh.value().height, 2880u);
}

TEST(image, strip_metadata_removes_privacy_keeps_structure) {
  auto f = make_jpeg(100, 50, 6, true);
  auto s = jpeg_strip_metadata(f.data(), f.size());
  ASSERT_TRUE(s.ok());
  auto& out = s.value();
  // 隐私段已消失，结构段保留
  ASSERT_FALSE(contains(out, "Exif"));
  ASSERT_FALSE(contains(out, "secret"));
  ASSERT_TRUE(contains(out, "JFIF"));  // APP0 保留
  auto h = parse_image_head(out.data(), out.size());
  ASSERT_TRUE(h.ok());
  ASSERT_EQ(h.value().width, 100u);          // SOF 完好
  ASSERT_EQ(h.value().orientation, 1);       // EXIF 已剥（调用方须先读向再剥，坑 9）
  ASSERT_FALSE(h.value().has_gps);
  // 熵数据逐字节保留（尾部 7 字节 = 熵 5 字节 + EOI）
  ASSERT_TRUE(out.size() >= 7);
  for (int k = 1; k <= 7; ++k) ASSERT_EQ(out[out.size() - k], f[f.size() - k]);
  // 幂等
  auto s2 = jpeg_strip_metadata(out.data(), out.size());
  ASSERT_TRUE(s2.ok());
  ASSERT_TRUE(s2.value() == out);
}

TEST(image, malformed_and_fuzz_lite) {
  const std::vector<std::vector<uint8_t>> bad = {
      {},
      {0xFF, 0xD8},                          // 只有 SOI
      {0xFF, 0xD8, 0x00, 0x11},              // 坏对齐
      {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x01},  // 段长非法
  };
  for (auto& f : bad) {
    if (!f.empty() && f[0] == 0xFF) ASSERT_FALSE(parse_image_head(f.data(), f.size()).ok());
    jpeg_strip_metadata(f.data(), f.size());  // 不崩溃即可
  }
  auto good = make_jpeg(100, 50, 6, true);
  std::mt19937 rng(20260831);
  for (int i = 0; i < 3000; ++i) {
    auto f = good;
    int muts = 1 + rng() % 8;
    for (int m = 0; m < muts; ++m) {
      size_t pos = rng() % f.size();
      switch (rng() % 3) {
        case 0: f[pos] = uint8_t(rng()); break;
        case 1: f.insert(f.begin() + pos, uint8_t(rng())); break;
        case 2: f.erase(f.begin() + pos); break;
      }
    }
    parse_image_head(f.data(), f.size());
    jpeg_strip_metadata(f.data(), f.size());
    sniff_mime(f.data(), f.size());
  }
  for (int i = 0; i < 1000; ++i) {
    std::vector<uint8_t> f(rng() % 300);
    for (auto& b : f) b = uint8_t(rng());
    parse_image_head(f.data(), f.size());
    sniff_mime(f.data(), f.size());
  }
}

