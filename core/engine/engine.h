// 调度 Actor 引擎（架构 §3/§8/§9/§10）：任务状态的唯一主人。
// 全部状态变更发生在调度线程；传输/计算是无状态执行器，只回投事件；journal 先行。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/journal/journal.h"
#include "core/scheduler/types.h"
#include "core/util/error.h"

namespace immd {

struct UploadRequest {
  std::string file_path;
  std::string mime = "application/octet-stream";
  Priority priority = Priority::P4_File;
  std::string task_id;  // 空则内核生成
};

struct DownloadRequest {
  std::string media_id;
  std::string dest_path;
  Priority priority = Priority::P4_File;
  std::string sha256_hex;  // 非空则下载后校验（消息引用里携带，架构 §4）
  std::string task_id;
};

// Initial task-record commit only, not file snapshot or transfer completion.
enum class JournalReceipt { None = 0, Normal = 1, Full = 2, Failed = 3 };

struct ProgressEvent {
  std::string task_id;
  TaskKind kind = TaskKind::Upload;
  TaskState state = TaskState::Queued;
  int64_t bytes_done = 0;
  int64_t bytes_total = 0;
  std::string media_id;  // Ready（上传/秒传）时携带
  Err error = Err::Ok;   // Failed 时携带
  std::string message;
  JournalReceipt journal_receipt = JournalReceipt::None;
};

// 回调线程约定（三内核一致）：内核回调线程发出，业务自己切主线程；回调内禁止调用引擎 API 之外的内核对象。
using ProgressCallback = std::function<void(const ProgressEvent&)>;

class Engine {
 public:
  static Expected<std::unique_ptr<Engine>> start(Config cfg, ProgressCallback cb);
  ~Engine();  // 停机不清账：journal 保留现场，重启 recover() 恢复

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // 均为投递语义：快速失败（队列满 → Err::Busy），结果经 ProgressCallback 异步交付。
  Expected<std::string> upload(UploadRequest r);
  Expected<std::string> download(DownloadRequest r);
  Error pause(const std::string& task_id);
  Error resume(const std::string& task_id);
  Error cancel(const std::string& task_id);
  Error recover();  // 扫描 journal 未完成任务恢复调度（启动后显式调用，时机业务可控）

  // 运行时全局上行限速（0 = 不限）。这是让路信号的落点（架构 §7）：
  // IMLink 心跳 RTT 劣化 → 降档；通话场景 → 涓流；恢复 → 置 0。
  Error set_rate_limit(int64_t bytes_per_sec);

  // ---- 流式上传（语音边录边传，架构 §14）----
  // begin 立即开单（无 hash/size）；append 投递字节（内核按分片粒度边录边传）；
  // finish 补最后一片 + 单遍 hash + complete——"松手到可发"亚秒级。
  // 语义：不走秒传；进程重启不恢复（录音已断，recover 标记 Failed）；cancel 即废单。
  struct StreamBeginRequest {
    std::string mime = "audio/opus";
    Priority priority = Priority::P0_Voice;
  };
  Expected<std::string> stream_begin(StreamBeginRequest r);
  Error stream_append(const std::string& task_id, const void* data, size_t len);
  Error stream_finish(const std::string& task_id);

  // 同步查询（排障/测试用；生产 UI 走 ProgressCallback）
  Expected<TaskRecord> query(const std::string& task_id);
  // Read the journal only, not live in-memory task state. Same thread restrictions.
  Expected<TaskRecord> query_persisted(const std::string& task_id);

 private:
  Engine() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace immd

