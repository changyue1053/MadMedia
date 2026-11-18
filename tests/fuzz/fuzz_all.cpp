// libFuzzer 入口（实施计划 P9.3）：parser 三件套 + 协议 JSON。
// 构建：-DIMMD_FUZZ=ON —— 需要开源 LLVM clang（Apple clang 不带 fuzzer runtime）。
// CI 长跑目标：累计 24h 零 crash / 零 sanitizer 报告（发布门槛，计划 §6）。
// 日常兜底：同一组入口在 unit 测试的 fuzz-lite（确定性种子变异）里每次提交都跑。
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/parser/imagemeta.h"
#include "core/parser/mp4box.h"
#include "core/util/json.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace immd;
  mp4_inspect(data, size);
  {
    std::vector<uint8_t> in(data, data + size), out;
    mp4_faststart(in, &out);
  }
  parse_image_head(data, size);
  jpeg_strip_metadata(data, size);
  sniff_mime(data, size);
  Json::parse(std::string(reinterpret_cast<const char*>(data), size));
  return 0;
}

