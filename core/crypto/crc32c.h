// CRC32C（Castagnoli）：分片级校验（架构 §7）。硬件指令（ARMv8 CRC / SSE4.2）可用则用，
// 软件查表兜底；两条路径在单测中互相对拍。
// 约定：crc32c(0, data, len) 即标准校验值（"123456789" → 0xE3069283）。
#pragma once

#include <cstddef>
#include <cstdint>

namespace immd {

uint32_t crc32c(uint32_t crc, const void* data, size_t len);
uint32_t crc32c_sw(uint32_t crc, const void* data, size_t len);  // 软件路径（对拍用）
bool crc32c_hw_available();

}  // namespace immd

