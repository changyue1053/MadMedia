// 图片处理参考链（架构 §12 / 实施 P5.2-5.3）：解码 → 缩放 → 编码 + 三层产物 + blurhash。
//
// 定位（诚实边界）：桌面/CI 的**参考实现**，用 vendored stb（public domain）打通端到端并可测；
// 移动端生产走系统解码器（ImageIO / MediaCodec，P8.2），输出口径与本链对齐（SSIM 抽查）。
// 安全前置（尺寸/魔数/炸弹）在 `core/parser/imagemeta`，本模块只处理"已判定安全"的像素。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/util/error.h"

namespace immd {

struct DecodedImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // width*height*4，行主序
};

// 从内存缓冲解码为 RGBA8（stb，无文件 IO）。带像素上限二次防线（parser 已预检，这里兜底）。
Expected<DecodedImage> decode_image(const uint8_t* data, size_t len,
                                    int64_t max_pixels = 50LL * 1000 * 1000);

// 高质量缩放（保持长宽比，长边 = max_long_edge；不放大）。
DecodedImage resize_long_edge(const DecodedImage& src, int max_long_edge);

// 编码为 WebP —— stb 不支持 WebP，此处退化为 JPEG（参考链口径；生产 WebP 由 libwebp/系统编码器）。
// quality 1..100。
Expected<std::vector<uint8_t>> encode_jpeg(const DecodedImage& img, int quality);

// blurhash（Wolt 算法，占位模糊图）：componly 3x3 分量，~30 字节字符串。
std::string blurhash_encode(const DecodedImage& img, int comp_x = 4, int comp_y = 3);

// 三层产物（架构 §12）：thumb（长边 320）/ preview（长边 1600）/ blurhash 占位。
struct ImageVariants {
  std::vector<uint8_t> thumb_jpeg;    // 长边 320, q60
  std::vector<uint8_t> preview_jpeg;  // 长边 1600, q75
  std::string blurhash;
  int width = 0;
  int height = 0;
};

Expected<ImageVariants> make_variants(const uint8_t* data, size_t len);

}  // namespace immd

