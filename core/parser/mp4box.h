// MP4 box parser + faststart remux（架构 §13 / 实施计划 P6.1）。
// 这是 parser/ 安全模块的一员：输入是不可信字节——span reader、无指针算术、
// 深度与数量上限、任何畸形输入返回错误绝不崩溃（fuzz 目标，见 tests/fuzz/）。
//
// faststart：MP4 的 moov（索引）默认写在文件尾 → 播放器要下完全文件才能起播。
// remux 把 moov 前置并修补 stco/co64 的绝对偏移（纯流拷贝，不重编码，毫秒级）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/util/error.h"

namespace immd {

struct Mp4Info {
  bool has_ftyp = false;
  bool has_moov = false;
  bool has_mdat = false;
  bool faststart = false;  // moov 位于 mdat 之前（可边下边播）
  int64_t moov_off = -1;
  int64_t moov_size = 0;
  int64_t mdat_off = -1;
  std::string major_brand;  // ftyp
  int64_t duration_ms = 0;  // mvhd
  uint32_t width = 0;       // 首个含尺寸的 tkhd（16.16 定点转整）
  uint32_t height = 0;
};

// 只读检视：秒传/免转码路径的 moov 位置检测 + 消息引用所需元信息（架构 §4/§13）。
Expected<Mp4Info> mp4_inspect(const uint8_t* data, size_t len);

// faststart remux：moov 前置 + stco/co64 偏移修补。
// 已是 faststart 的输入原样拷贝（幂等）；压缩 moov（cmov）返回错误（交服务端转码兜底）。
// stco 偏移在前移后溢出 u32 时**就地升级为 co64**（>4GB 大 mdat 的合法形态）：
// co64 每项多 4 字节 → moov 变大 → 位移 delta 变大 → 可能级联触发更多升级——
// 升级集与 delta 互为因果，用定点迭代收敛（单调增，至多 stco 个数轮）。
Error mp4_faststart(const std::vector<uint8_t>& in, std::vector<uint8_t>* out);

}  // namespace immd

