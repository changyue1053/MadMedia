#include "core/journal/journal.h"
#include "core/util/id.h"
#include "tests/harness.h"
#include "tests/testutil.h"

using namespace immd;

static TaskRecord sample_record() {
  TaskRecord r;
  r.task_id = gen_id("t");
  r.kind = TaskKind::Upload;
  r.state = TaskState::Transferring;
  r.priority = Priority::P2_Image;
  r.orig_path = "/photos/a.jpg";
  r.staging_path = "/staging/a.jpg";
  r.mime = "image/webp";
  r.size = 10 * 1024 * 1024;
  r.sha256_hex = "ab12";
  r.upload_id = "u_1";
  r.part_size = 4 * 1024 * 1024;
  r.nparts = 3;
  r.bitmap = PartBitmap(3);
  r.bitmap.set(0);
  r.bitmap.set(2);
  r.bytes_done = 8 * 1024 * 1024;
  r.retry_count = 1;
  r.created_at = 111;
  r.updated_at = 222;
  return r;
}

TEST(journal, save_load_roundtrip) {
  auto dir = immd_test::make_temp_dir();
  auto j = Journal::open(dir + "/journal.db");
  ASSERT_TRUE(j.ok());
  auto r = sample_record();
  ASSERT_FALSE(j.value()->save(r));
  auto got = j.value()->load(r.task_id);
  ASSERT_TRUE(got.ok());
  auto& g = got.value();
  ASSERT_EQ(int(g.state), int(TaskState::Transferring));
  ASSERT_EQ(g.size, r.size);
  ASSERT_EQ(g.nparts, 3u);
  ASSERT_TRUE(g.bitmap.get(0));
  ASSERT_FALSE(g.bitmap.get(1));
  ASSERT_TRUE(g.bitmap.get(2));
  ASSERT_EQ(g.upload_id, std::string("u_1"));
  ASSERT_EQ(g.bytes_done, 8 * 1024 * 1024);
}

TEST(journal, crash_reopen_recovers_unfinished_only) {
  auto dir = immd_test::make_temp_dir();
  std::string db = dir + "/journal.db";
  auto r1 = sample_record();                 // Transferring → 应被恢复
  auto r2 = sample_record();
  r2.state = TaskState::Ready;               // 终态 → 不恢复
  auto r3 = sample_record();
  r3.state = TaskState::Paused;              // 暂停 → 恢复清单里（保持暂停语义由引擎决定）
  {
    auto j = Journal::open(db);
    ASSERT_TRUE(j.ok());
    ASSERT_FALSE(j.value()->save(r1));
    ASSERT_FALSE(j.value()->save(r2));
    ASSERT_FALSE(j.value()->save(r3));
    // 不做任何清理直接析构 = 模拟 kill -9 后重启
  }
  auto j2 = Journal::open(db);
  ASSERT_TRUE(j2.ok());
  auto un = j2.value()->load_unfinished();
  ASSERT_TRUE(un.ok());
  ASSERT_EQ(un.value().size(), size_t(2));
  bool saw1 = false, saw3 = false;
  for (auto& t : un.value()) {
    if (t.task_id == r1.task_id) saw1 = true;
    if (t.task_id == r3.task_id) saw3 = true;
  }
  ASSERT_TRUE(saw1);
  ASSERT_TRUE(saw3);
}

TEST(journal, upsert_updates_and_remove) {
  auto dir = immd_test::make_temp_dir();
  auto j = Journal::open(dir + "/journal.db");
  ASSERT_TRUE(j.ok());
  auto r = sample_record();
  ASSERT_FALSE(j.value()->save(r));
  r.state = TaskState::Ready;
  r.media_id = "m_9";
  r.bitmap.set(1);
  ASSERT_FALSE(j.value()->save(r));  // upsert 更新
  auto got = j.value()->load(r.task_id);
  ASSERT_TRUE(got.ok());
  ASSERT_EQ(int(got.value().state), int(TaskState::Ready));
  ASSERT_EQ(got.value().media_id, std::string("m_9"));
  ASSERT_TRUE(got.value().bitmap.complete());
  ASSERT_FALSE(j.value()->remove(r.task_id));
  ASSERT_FALSE(j.value()->load(r.task_id).ok());
}

