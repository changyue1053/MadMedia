#include "core/pipeline/image/image_pipeline.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 仅取 stb 的声明（实现在 stb_impl.cpp 唯一 TU）。此处沿用同样的编译期开关以保持签名一致。
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb/stb_image.h"
#define STBI_WRITE_NO_STDIO
#include "third_party/stb/stb_image_write.h"
#include "third_party/stb/stb_image_resize2.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace immd {

Expected<DecodedImage> decode_image(const uint8_t* data, size_t len, int64_t max_pixels) {
  if (len == 0 || len > (size_t)INT_MAX) return make_err(Err::Protocol, "image: bad length");
  // 先读尺寸做炸弹二次防线（parser 是第一道；解码器入口再兜一次）
  int w = 0, h = 0, comp = 0;
  if (!stbi_info_from_memory(data, (int)len, &w, &h, &comp))
    return make_err(Err::Protocol, "image: undecodable header");
  if (w <= 0 || h <= 0) return make_err(Err::Protocol, "image: bad dimensions");
  if (int64_t(w) * int64_t(h) > max_pixels) return make_err(Err::Rejected, "image: decode bomb");

  int got_w = 0, got_h = 0, got_c = 0;
  uint8_t* px = stbi_load_from_memory(data, (int)len, &got_w, &got_h, &got_c, 4);  // 强制 RGBA
  if (!px) return make_err(Err::Protocol, "image: decode failed");
  DecodedImage img;
  img.width = got_w;
  img.height = got_h;
  img.rgba.assign(px, px + size_t(got_w) * size_t(got_h) * 4);
  stbi_image_free(px);
  return img;
}

DecodedImage resize_long_edge(const DecodedImage& src, int max_long_edge) {
  int long_edge = std::max(src.width, src.height);
  if (long_edge <= max_long_edge || max_long_edge <= 0) return src;  // 不放大
  double scale = double(max_long_edge) / double(long_edge);
  int dw = std::max(1, int(std::lround(src.width * scale)));
  int dh = std::max(1, int(std::lround(src.height * scale)));
  DecodedImage out;
  out.width = dw;
  out.height = dh;
  out.rgba.resize(size_t(dw) * size_t(dh) * 4);
  stbir_resize_uint8_srgb(src.rgba.data(), src.width, src.height, 0, out.rgba.data(), dw, dh, 0,
                          STBIR_RGBA);
  return out;
}

namespace {
void jpeg_sink(void* ctx, void* data, int size) {
  auto* out = static_cast<std::vector<uint8_t>*>(ctx);
  auto* p = static_cast<uint8_t*>(data);
  out->insert(out->end(), p, p + size);
}
}  // namespace

Expected<std::vector<uint8_t>> encode_jpeg(const DecodedImage& img, int quality) {
  if (img.width <= 0 || img.height <= 0) return make_err(Err::Protocol, "image: empty for encode");
  quality = std::clamp(quality, 1, 100);
  // stb JPEG 编码吃 RGB（3 通道）；从 RGBA 丢弃 alpha（聊天图无透明需求，preview/thumb 语义）
  std::vector<uint8_t> rgb(size_t(img.width) * size_t(img.height) * 3);
  for (size_t i = 0, n = size_t(img.width) * size_t(img.height); i < n; ++i) {
    rgb[i * 3] = img.rgba[i * 4];
    rgb[i * 3 + 1] = img.rgba[i * 4 + 1];
    rgb[i * 3 + 2] = img.rgba[i * 4 + 2];
  }
  std::vector<uint8_t> out;
  if (!stbi_write_jpg_to_func(jpeg_sink, &out, img.width, img.height, 3, rgb.data(), quality))
    return make_err(Err::Io, "image: jpeg encode failed");
  return out;
}

// ---------------- blurhash（https://github.com/woltapp/blurhash 的 C 参考的精简版）----------------
namespace {
const char* kB83 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz#$%*+,-.:;=?@[]^_{|}~";

void b83_encode(int value, int length, std::string* out) {
  for (int i = 1; i <= length; ++i) {
    int digit = (value / int(std::pow(83, length - i))) % 83;
    out->push_back(kB83[digit]);
  }
}

double srgb_to_linear(int v) {
  double x = v / 255.0;
  return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
}

int linear_to_srgb(double v) {
  double x = std::clamp(v, 0.0, 1.0);
  double s = x <= 0.0031308 ? x * 12.92 : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
  return int(s * 255 + 0.5);
}

int encode_ac(double r, double g, double b, double max) {
  auto quant = [&](double c) {
    return int(std::clamp(std::floor(std::copysign(std::pow(std::fabs(c / max), 0.5), c) * 9 + 9.5),
                          0.0, 18.0));
  };
  return quant(r) * 19 * 19 + quant(g) * 19 + quant(b);
}
}  // namespace

std::string blurhash_encode(const DecodedImage& img, int comp_x, int comp_y) {
  comp_x = std::clamp(comp_x, 1, 9);
  comp_y = std::clamp(comp_y, 1, 9);
  if (img.width <= 0 || img.height <= 0) return {};
  std::vector<std::array<double, 3>> factors;
  factors.reserve(size_t(comp_x) * size_t(comp_y));
  const int W = img.width, H = img.height;
  for (int y = 0; y < comp_y; ++y) {
    for (int x = 0; x < comp_x; ++x) {
      double r = 0, g = 0, b = 0;
      double norm = (x == 0 && y == 0) ? 1.0 : 2.0;
      for (int j = 0; j < H; ++j) {
        for (int i = 0; i < W; ++i) {
          double basis = std::cos(M_PI * x * i / W) * std::cos(M_PI * y * j / H);
          size_t idx = (size_t(j) * W + i) * 4;
          r += basis * srgb_to_linear(img.rgba[idx]);
          g += basis * srgb_to_linear(img.rgba[idx + 1]);
          b += basis * srgb_to_linear(img.rgba[idx + 2]);
        }
      }
      double scale = norm / (double(W) * double(H));
      factors.push_back({r * scale, g * scale, b * scale});
    }
  }
  std::string out;
  int size_flag = (comp_x - 1) + (comp_y - 1) * 9;
  b83_encode(size_flag, 1, &out);
  double max_ac = 0;
  for (size_t i = 1; i < factors.size(); ++i)
    for (int c = 0; c < 3; ++c) max_ac = std::max(max_ac, std::fabs(factors[i][c]));
  int quant_max = std::clamp(int(std::floor(max_ac * 166 - 0.5)), 0, 82);
  double max_val = (quant_max + 1) / 166.0;
  if (factors.size() == 1) max_val = 1.0;
  b83_encode(factors.size() > 1 ? quant_max : 0, 1, &out);
  // DC
  int dc = (linear_to_srgb(factors[0][0]) << 16) + (linear_to_srgb(factors[0][1]) << 8) +
           linear_to_srgb(factors[0][2]);
  b83_encode(dc, 4, &out);
  for (size_t i = 1; i < factors.size(); ++i)
    b83_encode(encode_ac(factors[i][0], factors[i][1], factors[i][2], max_val), 2, &out);
  return out;
}

Expected<ImageVariants> make_variants(const uint8_t* data, size_t len) {
  auto dec = decode_image(data, len);
  if (!dec.ok()) return dec.error();
  const DecodedImage& full = dec.value();

  ImageVariants v;
  v.width = full.width;
  v.height = full.height;

  DecodedImage thumb = resize_long_edge(full, 320);
  auto tj = encode_jpeg(thumb, 60);
  if (!tj.ok()) return tj.error();
  v.thumb_jpeg = std::move(tj.value());

  DecodedImage preview = resize_long_edge(full, 1600);
  auto pj = encode_jpeg(preview, 75);
  if (!pj.ok()) return pj.error();
  v.preview_jpeg = std::move(pj.value());

  v.blurhash = blurhash_encode(thumb);  // blurhash 从缩略图算，省算力
  return v;
}

}  // namespace immd

