// 图片安全 parser（架构 §12/§14 / 实施计划 P5.1-5.2 的 parser 半边）。
// 职责：解码之前回答四个安全问题——这是什么（魔数，不信后缀）、多大（炸弹预检）、
// 怎么摆（EXIF 方向）、带没带隐私（GPS）；并提供 JPEG 元数据剥离。
// 像素级解码/缩放属平台桥（系统解码器优先，架构 §11），不在本模块。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/util/error.h"

namespace immd {

enum class ImageKind { Unknown, Jpeg, Png, Gif, WebP, Heic };

struct ImageHead {
  ImageKind kind = ImageKind::Unknown;
  std::string mime = "application/octet-stream";
  uint32_t width = 0;
  uint32_t height = 0;
  int orientation = 1;   // EXIF 1..8（解码时归一方向的输入）
  bool has_gps = false;  // 隐私面：上传前必须剥离
  bool bomb = false;     // 声明像素数 > 上限：解码前拒绝（架构 §12 解码炸弹）
};

// 魔数嗅探：覆盖图片 + 高危类型（SVG/HTML 是存储型 XSS 面，架构 §14）+ 常见容器。
// 返回 MIME；未知返回 application/octet-stream。
std::string sniff_mime(const uint8_t* data, size_t len);

// 头部解析（不解码像素）。畸形输入 → Err::Protocol。
Expected<ImageHead> parse_image_head(const uint8_t* data, size_t len,
                                     int64_t max_pixels = 50LL * 1000 * 1000);

// JPEG 元数据剥离：移除 APP1..APP15 与 COM（EXIF/GPS/XMP/厂商注释），
// 保留 APP0(JFIF) 与全部结构段（DQT/DHT/SOF/SOS/熵数据）。
// 注意：剥离后 orientation 信息随 APP1 消失——调用方必须先读 head、解码时归一方向
//（顺序错了图片躺倒，架构 §24 坑 9）。
Expected<std::vector<uint8_t>> jpeg_strip_metadata(const uint8_t* data, size_t len);

}  // namespace immd

