// 任务模型与内核配置（架构 §8/§10）。所有可调参数集中在 Config——云控下发的落点。
#pragma once

#include <cstdint>
#include <string>

#include "core/util/bitmap.h"

namespace immd {

enum class TaskKind : int { Upload = 0, Download = 1 };

// 状态机（架构 §10）：Failed/Paused 保留断点可恢复；Ready/Canceled 是终态。
enum class TaskState : int {
  Queued = 0,        // 已落账，等待调度
  Preparing = 1,     // 快照 + hash（计算域）/ 下载：取凭证
  Transferring = 2,  // 分片传输中
  Finalizing = 3,    // 上传：complete 校验；下载：整体校验
  Ready = 4,         // 终态：media_id 就绪 / 文件落地
  Failed = 5,        // 可恢复：保留断点，等待手动/条件重试
  Paused = 6,        // 用户暂停：保留断点
  Canceled = 7,      // 终态：废单
};

enum class Priority : int {
  P0_Voice = 0,
  P1_Thumb = 1,
  P2_Image = 2,
  P3_Video = 3,
  P4_File = 4,
  P5_Background = 5,
};

const char* state_name(TaskState s);
const char* kind_name(TaskKind k);

// journal 行 = 任务的全部可恢复现场（架构 §10 DDL 的内存形态）。
struct TaskRecord {
  std::string task_id;  // 即协议 client_task_id（幂等锚点）
  TaskKind kind = TaskKind::Upload;
  TaskState state = TaskState::Queued;
  Priority priority = Priority::P4_File;

  std::string orig_path;     // 用户原始文件（仅记录，不再读取）
  std::string staging_path;  // 沙箱快照副本（上传期间的唯一读源，防原文件被改写）
  std::string dest_path;     // 下载目标
  std::string mime;
  bool streaming = false;    // 流式上传（语音边录边传，架构 §14）：init 无 hash/size，complete 补报
  int64_t size = 0;
  std::string sha256_hex;
  std::string quick_hash_hex;

  std::string media_id;
  std::string upload_id;
  int64_t part_size = 0;
  uint32_t nparts = 0;
  PartBitmap bitmap;
  int64_t bytes_done = 0;

  int retry_count = 0;
  int64_t next_retry_at = 0;
  std::string last_error;
  int64_t created_at = 0;
  int64_t updated_at = 0;
};

struct Config {
  std::string base_url;             // mock/真实控制面，如 http://127.0.0.1:8787
  std::string tenant_id = "t_dev";
  std::string data_dir;             // journal.db / staging/ / cache/ 的根目录

  // 并发预算（架构 §8：任务/分片/连接三级）
  int max_active_tasks = 3;
  int max_parts_per_task = 3;
  int max_total_streams = 4;

  // 分片与自适应（架构 §7）
  int64_t part_min = 512 * 1024;
  int64_t part_max = 8 * 1024 * 1024;
  int64_t part_first = 1 * 1024 * 1024;  // 慢启动首选
  int target_part_seconds = 10;

  // 重试预算
  int part_retry_limit = 5;
  int64_t retry_base_ms = 500;
  int64_t retry_cap_ms = 30000;
  int64_t task_retry_budget_ms = 15 * 60 * 1000;

  // 限速（0 = 不限）
  int64_t rate_limit_bps = 0;

  // READY 策略（架构 §6 合规窗口两档）：true = UPLOADED 即 READY
  bool ready_on_uploaded = true;

  int64_t http_timeout_ms = 30000;
  int64_t connect_timeout_ms = 5000;

  // 媒体缓存（架构 §18 端侧）：内容寻址 LRU；0 = 关闭。
  // 命中即零网络下载；下载校验通过后异步回填。
  int64_t cache_capacity = 0;

  // 端上 faststart remux 的体积上限（架构 §13）：≤ 上限的尾置 moov MP4 在快照期就地前移；
  // 超限跳过（内存代价不可控），交服务端转码/remux 兜底。0 = 关闭端上 remux。
  int64_t remux_max_bytes = 64 * 1024 * 1024;

  // 测试注入位
  int64_t force_part_size = 0;  // >0 时绕过自适应（e2e 用小分片构造多片场景）
};

}  // namespace immd

