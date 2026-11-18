// 《媒体协议契约 v1》的编解码层（架构 §5）：纯函数，无网络依赖，单测全覆盖。
// 任何字段缺失/类型错 → Err::Protocol（响应是不可信输入）。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/scheduler/types.h"
#include "core/util/error.h"

namespace immd {

struct InitAnswer {
  enum class Kind { Instant, Challenge, Upload } kind = Kind::Upload;
  // Instant
  std::string media_id;
  // Challenge
  std::string challenge_id;
  std::string nonce;
  std::vector<std::pair<int64_t, int64_t>> ranges;
  // Upload
  std::string upload_id;
  int64_t part_size = 0;
  uint32_t nparts = 0;
  std::vector<uint32_t> received;  // 服务端真相：已收分片
};

struct CompleteAnswer {
  std::string media_id;              // 成功时
  std::vector<uint32_t> missing;     // 409 时：缺失分片
};

struct DlAnswer {
  std::string url;
  int64_t size = 0;
  std::string sha256_hex;
};

std::string build_init(const TaskRecord& r, const Config& cfg, int64_t suggest_part_size);
Expected<InitAnswer> parse_init(const std::string& body);

std::string build_part_urls(const std::string& upload_id, const std::vector<uint32_t>& parts);
Expected<std::map<uint32_t, std::string>> parse_part_urls(const std::string& body);

std::string build_complete(const std::string& upload_id, const std::string& sha256_hex);
// 流式封箱：录制结束才知道 size/nparts，complete 一并补报（服务端据此校验连续性与总量）
std::string build_complete_streaming(const std::string& upload_id, const std::string& sha256_hex,
                                     int64_t size, uint32_t nparts);
Expected<CompleteAnswer> parse_complete_ok(const std::string& body);
Expected<CompleteAnswer> parse_complete_missing(const std::string& body);  // 409

std::string build_challenge(const std::string& challenge_id, const std::string& proof_hex);

std::string build_dl(const std::string& media_id);
Expected<DlAnswer> parse_dl(const std::string& body);

}  // namespace immd

