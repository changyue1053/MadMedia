#include "core/journal/journal.h"

#include <sqlite3.h>

#include "core/util/log.h"

namespace immd {

namespace {

constexpr const char* kDDL = R"sql(
CREATE TABLE IF NOT EXISTS transfer_task (
  task_id       TEXT PRIMARY KEY,
  kind          INTEGER NOT NULL,
  state         INTEGER NOT NULL,
  priority      INTEGER NOT NULL,
  orig_path     TEXT,
  staging_path  TEXT,
  dest_path     TEXT,
  mime          TEXT,
  size          INTEGER DEFAULT 0,
  sha256        TEXT,
  quick_hash    TEXT,
  media_id      TEXT,
  upload_id     TEXT,
  part_size     INTEGER DEFAULT 0,
  nparts        INTEGER DEFAULT 0,
  parts_bitmap  BLOB,
  bytes_done    INTEGER DEFAULT 0,
  retry_count   INTEGER DEFAULT 0,
  next_retry_at INTEGER DEFAULT 0,
  last_error    TEXT,
  created_at    INTEGER DEFAULT 0,
  updated_at    INTEGER DEFAULT 0,
  streaming     INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_task_sched ON transfer_task(state, priority, next_retry_at);
)sql";

constexpr const char* kUpsert = R"sql(
INSERT INTO transfer_task (task_id, kind, state, priority, orig_path, staging_path, dest_path,
  mime, size, sha256, quick_hash, media_id, upload_id, part_size, nparts, parts_bitmap,
  bytes_done, retry_count, next_retry_at, last_error, created_at, updated_at, streaming)
VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23)
ON CONFLICT(task_id) DO UPDATE SET
  kind=?2, state=?3, priority=?4, orig_path=?5, staging_path=?6, dest_path=?7, mime=?8,
  size=?9, sha256=?10, quick_hash=?11, media_id=?12, upload_id=?13, part_size=?14,
  nparts=?15, parts_bitmap=?16, bytes_done=?17, retry_count=?18, next_retry_at=?19,
  last_error=?20, updated_at=?22, streaming=?23
)sql";

constexpr const char* kSelect =
    "SELECT task_id, kind, state, priority, orig_path, staging_path, dest_path, mime, size, "
    "sha256, quick_hash, media_id, upload_id, part_size, nparts, parts_bitmap, bytes_done, "
    "retry_count, next_retry_at, last_error, created_at, updated_at, streaming FROM transfer_task";

std::string col_text(sqlite3_stmt* st, int i) {
  const unsigned char* p = sqlite3_column_text(st, i);
  return p ? reinterpret_cast<const char*>(p) : "";
}

TaskRecord row_to_record(sqlite3_stmt* st) {
  TaskRecord r;
  r.task_id = col_text(st, 0);
  r.kind = static_cast<TaskKind>(sqlite3_column_int(st, 1));
  r.state = static_cast<TaskState>(sqlite3_column_int(st, 2));
  r.priority = static_cast<Priority>(sqlite3_column_int(st, 3));
  r.orig_path = col_text(st, 4);
  r.staging_path = col_text(st, 5);
  r.dest_path = col_text(st, 6);
  r.mime = col_text(st, 7);
  r.size = sqlite3_column_int64(st, 8);
  r.sha256_hex = col_text(st, 9);
  r.quick_hash_hex = col_text(st, 10);
  r.media_id = col_text(st, 11);
  r.upload_id = col_text(st, 12);
  r.part_size = sqlite3_column_int64(st, 13);
  r.nparts = static_cast<uint32_t>(sqlite3_column_int64(st, 14));
  const void* blob = sqlite3_column_blob(st, 15);
  int blob_n = sqlite3_column_bytes(st, 15);
  std::vector<uint8_t> bm;
  if (blob && blob_n > 0) bm.assign(static_cast<const uint8_t*>(blob), static_cast<const uint8_t*>(blob) + blob_n);
  r.bitmap = PartBitmap::from_blob(bm, r.nparts);
  r.bytes_done = sqlite3_column_int64(st, 16);
  r.retry_count = sqlite3_column_int(st, 17);
  r.next_retry_at = sqlite3_column_int64(st, 18);
  r.last_error = col_text(st, 19);
  r.created_at = sqlite3_column_int64(st, 20);
  r.updated_at = sqlite3_column_int64(st, 21);
  r.streaming = sqlite3_column_int(st, 22) != 0;
  return r;
}

}  // namespace

Expected<std::unique_ptr<Journal>> Journal::open(const std::string& db_path) {
  auto j = std::unique_ptr<Journal>(new Journal());
  if (sqlite3_open(db_path.c_str(), &j->db_) != SQLITE_OK) {
    return make_err(Err::Io, "journal open failed: " + db_path);
  }
  // WAL + NORMAL：掉电最多丢最后一笔账（重启后 init 幂等对账兜底），绝不坏库。
  if (Error e = j->exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=5000;")) return e;
  if (Error e = j->exec(kDDL)) return e;
  if (sqlite3_prepare_v2(j->db_, kUpsert, -1, &j->upsert_, nullptr) != SQLITE_OK) {
    return make_err(Err::Io, std::string("journal prepare failed: ") + sqlite3_errmsg(j->db_));
  }
  return j;
}

Journal::~Journal() {
  if (upsert_) sqlite3_finalize(upsert_);
  if (db_) sqlite3_close(db_);
}

Error Journal::exec(const char* sql) {
  char* msg = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
    std::string m = msg ? msg : "unknown";
    sqlite3_free(msg);
    return make_err(Err::Io, "journal exec: " + m);
  }
  return {};
}

Error Journal::save(const TaskRecord& r) {
  sqlite3_reset(upsert_);
  sqlite3_clear_bindings(upsert_);
  auto bt = [&](int i, const std::string& s) { sqlite3_bind_text(upsert_, i, s.c_str(), -1, SQLITE_TRANSIENT); };
  bt(1, r.task_id);
  sqlite3_bind_int(upsert_, 2, static_cast<int>(r.kind));
  sqlite3_bind_int(upsert_, 3, static_cast<int>(r.state));
  sqlite3_bind_int(upsert_, 4, static_cast<int>(r.priority));
  bt(5, r.orig_path);
  bt(6, r.staging_path);
  bt(7, r.dest_path);
  bt(8, r.mime);
  sqlite3_bind_int64(upsert_, 9, r.size);
  bt(10, r.sha256_hex);
  bt(11, r.quick_hash_hex);
  bt(12, r.media_id);
  bt(13, r.upload_id);
  sqlite3_bind_int64(upsert_, 14, r.part_size);
  sqlite3_bind_int64(upsert_, 15, r.nparts);
  const auto& blob = r.bitmap.blob();
  sqlite3_bind_blob(upsert_, 16, blob.empty() ? nullptr : blob.data(), static_cast<int>(blob.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(upsert_, 17, r.bytes_done);
  sqlite3_bind_int(upsert_, 18, r.retry_count);
  sqlite3_bind_int64(upsert_, 19, r.next_retry_at);
  bt(20, r.last_error);
  sqlite3_bind_int64(upsert_, 21, r.created_at);
  sqlite3_bind_int64(upsert_, 22, r.updated_at);
  sqlite3_bind_int(upsert_, 23, r.streaming ? 1 : 0);
  if (sqlite3_step(upsert_) != SQLITE_DONE) {
    return make_err(Err::Io, std::string("journal save: ") + sqlite3_errmsg(db_));
  }
  return {};
}

Expected<TaskRecord> Journal::load(const std::string& task_id) {
  std::string sql = std::string(kSelect) + " WHERE task_id = ?1";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return make_err(Err::Io, sqlite3_errmsg(db_));
  sqlite3_bind_text(st, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  Expected<TaskRecord> out = make_err(Err::NotFound, "task not found: " + task_id);
  if (sqlite3_step(st) == SQLITE_ROW) out = row_to_record(st);
  sqlite3_finalize(st);
  return out;
}

Expected<std::vector<TaskRecord>> Journal::load_unfinished() {
  std::string sql = std::string(kSelect) + " WHERE state NOT IN (4, 7) ORDER BY priority, created_at";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return make_err(Err::Io, sqlite3_errmsg(db_));
  std::vector<TaskRecord> out;
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_record(st));
  sqlite3_finalize(st);
  return out;
}

Expected<std::vector<TaskRecord>> Journal::load_all() {
  std::string sql = std::string(kSelect) + " ORDER BY created_at";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return make_err(Err::Io, sqlite3_errmsg(db_));
  std::vector<TaskRecord> out;
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(row_to_record(st));
  sqlite3_finalize(st);
  return out;
}

Error Journal::remove(const std::string& task_id) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM transfer_task WHERE task_id = ?1", -1, &st, nullptr) != SQLITE_OK)
    return make_err(Err::Io, sqlite3_errmsg(db_));
  sqlite3_bind_text(st, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) return make_err(Err::Io, "journal remove failed");
  return {};
}

}  // namespace immd

