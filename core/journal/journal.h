// 任务账本（架构 §10）：SQLite/WAL 小库，只在调度线程访问（单写者）。
// 纪律：状态变更与账本落盘同步完成——崩溃后账本即现场。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/scheduler/types.h"
#include "core/util/error.h"

struct sqlite3;
struct sqlite3_stmt;

namespace immd {

class Journal {
 public:
  static Expected<std::unique_ptr<Journal>> open(const std::string& db_path);
  ~Journal();

  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  Error save(const TaskRecord& r);                       // 全量 upsert（写频率低，简单优先）
  Expected<TaskRecord> load(const std::string& task_id);
  Expected<std::vector<TaskRecord>> load_unfinished();   // state ∉ {Ready, Canceled}
  Expected<std::vector<TaskRecord>> load_all();          // 排障（immd-cli journal）
  Error remove(const std::string& task_id);

 private:
  Journal() = default;
  Error exec(const char* sql);
  sqlite3* db_ = nullptr;
  sqlite3_stmt* upsert_ = nullptr;
};

}  // namespace immd

