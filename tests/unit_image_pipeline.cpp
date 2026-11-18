#include <vector>

#include "core/parser/imagemeta.h"
#include "core/pipeline/image/image_pipeline.h"
#include "tests/harness.h"

using namespace immd;

namespace {

// 解码炸弹 JPEG：结构合法，声明 60000×60000 像素（36 亿）
std::vector<uint8_t> make_bomb_jpeg_bytes() {
  std::vector<uint8_t> f = {0xFF, 0xD8};
  const uint8_t sof[] = {0xFF, 0xC0, 0x00, 0x0B, 0x08, 0xEA, 0x60, 0xEA, 0x60, 0x01, 0x01, 0x11, 0x00};
  f.insert(f.end(), sof, sof + sizeof(sof));
  const uint8_t sos[] = {0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00};
  f.insert(f.end(), sos, sos + sizeof(sos));
  f.insert(f.end(), {0x12, 0x34, 0x56, 0xFF, 0xD9});
  return f;
}

// 造一张 W×H 的 RGBA 渐变图（内容确定，便于往返断言）
DecodedImage gradient(int w, int h) {
  DecodedImage img;
  img.width = w;
  img.height = h;
  img.rgba.resize(size_t(w) * size_t(h) * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      size_t i = (size_t(y) * w + x) * 4;
      img.rgba[i] = uint8_t(x * 255 / (w > 1 ? w - 1 : 1));
      img.rgba[i + 1] = uint8_t(y * 255 / (h > 1 ? h - 1 : 1));
      img.rgba[i + 2] = 128;
      img.rgba[i + 3] = 255;
    }
  return img;
}

}  // namespace

TEST(image_pipeline, decode_resize_encode_roundtrip) {
  // 用管线自己的编码器造一张 JPEG（也顺带验证 encode），再走 decode
  auto src = gradient(200, 120);
  auto jpg = encode_jpeg(src, 90);
  ASSERT_TRUE(jpg.ok());
  ASSERT_TRUE(jpg.value().size() > 100);
  // 编码产物的魔数应被嗅探为 jpeg（与 parser 口径一致）
  ASSERT_EQ(sniff_mime(jpg.value().data(), jpg.value().size()), std::string("image/jpeg"));

  auto dec = decode_image(jpg.value().data(), jpg.value().size());
  ASSERT_TRUE(dec.ok());
  ASSERT_EQ(dec.value().width, 200);
  ASSERT_EQ(dec.value().height, 120);
}

TEST(image_pipeline, resize_keeps_aspect_and_never_upscales) {
  auto src = gradient(1000, 400);
  auto small = resize_long_edge(src, 200);  // 长边 1000→200，短边等比
  ASSERT_EQ(small.width, 200);
  ASSERT_EQ(small.height, 80);
  // 不放大：目标 > 原长边时原样返回
  auto same = resize_long_edge(src, 5000);
  ASSERT_EQ(same.width, 1000);
  ASSERT_EQ(same.height, 400);
}

TEST(image_pipeline, three_variants_and_blurhash) {
  auto src = gradient(2000, 1500);
  auto jpg = encode_jpeg(src, 92);
  ASSERT_TRUE(jpg.ok());
  auto v = make_variants(jpg.value().data(), jpg.value().size());
  ASSERT_TRUE(v.ok());
  auto& var = v.value();
  ASSERT_EQ(var.width, 2000);
  ASSERT_EQ(var.height, 1500);

  // thumb 长边 = 320，preview 长边 = 1600（原图 2000 → 缩），且 thumb < preview 体积
  auto td = decode_image(var.thumb_jpeg.data(), var.thumb_jpeg.size());
  ASSERT_TRUE(td.ok());
  ASSERT_EQ(std::max(td.value().width, td.value().height), 320);
  auto pd = decode_image(var.preview_jpeg.data(), var.preview_jpeg.size());
  ASSERT_TRUE(pd.ok());
  ASSERT_EQ(std::max(pd.value().width, pd.value().height), 1600);
  ASSERT_TRUE(var.thumb_jpeg.size() < var.preview_jpeg.size());

  // blurhash：非空、以 base83 字符起头、长度符合 4×3 分量（1+1+4 + (12-1)*2 = 28）
  ASSERT_FALSE(var.blurhash.empty());
  ASSERT_EQ(var.blurhash.size(), size_t(28));
}

TEST(image_pipeline, decode_bomb_rejected_at_decoder) {
  // 解码器入口的二次防线：合法 JPEG 头但声明 60000×60000 → Rejected（parser 是第一道）
  auto bomb = make_bomb_jpeg_bytes();
  auto dec = decode_image(bomb.data(), bomb.size());
  ASSERT_FALSE(dec.ok());
  ASSERT_EQ(int(dec.error().code), int(Err::Rejected));
}

TEST(image_pipeline, garbage_input_errors_not_crash) {
  std::vector<uint8_t> junk = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xD8, 0x00};
  ASSERT_FALSE(decode_image(junk.data(), junk.size()).ok());
  ASSERT_FALSE(decode_image(nullptr, 0).ok());
}

