#include <random>

#include "core/scheduler/adaptive.h"
#include "core/scheduler/fsm.h"
#include "core/scheduler/retry.h"
#include "core/util/id.h"
#include "tests/harness.h"

using namespace immd;

TEST(fsm, table_covers_all_pairs) {
  // 逐项断言合法集合；其余全部非法（表驱动：漏一条即红灯）
  struct T {
    TaskState f, t;
  };
  const T legal[] = {
      {TaskState::Queued, TaskState::Preparing},    {TaskState::Queued, TaskState::Failed},
      {TaskState::Queued, TaskState::Paused},       {TaskState::Queued, TaskState::Canceled},
      {TaskState::Preparing, TaskState::Transferring}, {TaskState::Preparing, TaskState::Ready},
      {TaskState::Preparing, TaskState::Failed},    {TaskState::Preparing, TaskState::Paused},
      {TaskState::Preparing, TaskState::Canceled},  {TaskState::Transferring, TaskState::Finalizing},
      {TaskState::Transferring, TaskState::Failed}, {TaskState::Transferring, TaskState::Paused},
      {TaskState::Transferring, TaskState::Canceled}, {TaskState::Finalizing, TaskState::Ready},
      {TaskState::Finalizing, TaskState::Failed},   {TaskState::Finalizing, TaskState::Canceled},
      {TaskState::Finalizing, TaskState::Transferring},  // complete 409 缺片 → 回补
      {TaskState::Failed, TaskState::Queued},       {TaskState::Failed, TaskState::Canceled},
      {TaskState::Paused, TaskState::Queued},       {TaskState::Paused, TaskState::Canceled},
  };
  auto is_legal = [&](TaskState f, TaskState t) {
    for (auto& e : legal)
      if (e.f == f && e.t == t) return true;
    return false;
  };
  for (int f = 0; f < 8; ++f)
    for (int t = 0; t < 8; ++t) {
      auto ff = TaskState(f), tt = TaskState(t);
      ASSERT_EQ(fsm_can(ff, tt), is_legal(ff, tt));
    }
  ASSERT_TRUE(fsm_terminal(TaskState::Ready));
  ASSERT_TRUE(fsm_terminal(TaskState::Canceled));
  ASSERT_TRUE(fsm_resumable(TaskState::Failed));
  ASSERT_TRUE(fsm_resumable(TaskState::Paused));
  ASSERT_FALSE(fsm_resumable(TaskState::Ready));
}

TEST(retry, full_jitter_within_bounds) {
  Backoff b;  // base 500, cap 30000
  std::mt19937 rng(1);
  for (int attempt = 1; attempt <= 10; ++attempt) {
    int64_t ceiling = std::min<int64_t>(30000, 500LL << (attempt - 1));
    for (int i = 0; i < 200; ++i) {
      int64_t d = b.delay_ms(attempt, rng);
      ASSERT_GE(d, 0);
      ASSERT_LE(d, ceiling);
    }
  }
  // 确定性：同种子同序列
  std::mt19937 r1(7), r2(7);
  for (int i = 1; i < 6; ++i) ASSERT_EQ(b.delay_ms(i, r1), b.delay_ms(i, r2));
}

TEST(adaptive, ewma_converges_and_suggests) {
  ThroughputEwma e;
  ASSERT_FALSE(e.has_sample());
  // 无样本 → 慢启动首选 1MB
  ASSERT_EQ(suggest_part_size(e.bytes_per_sec(), 10, 512 << 10, 8 << 20, 1 << 20), int64_t(1 << 20));
  // 持续 1MB/s 样本 → 收敛到 ~1MB/s，10s 目标 → 10MB 被 clamp 到 8MB
  for (int i = 0; i < 30; ++i) e.on_sample(1 << 20, 1000);
  double bps = e.bytes_per_sec();
  ASSERT_GE(bps, 0.9 * (1 << 20));
  ASSERT_LE(bps, 1.1 * (1 << 20));
  ASSERT_EQ(suggest_part_size(bps, 10, 512 << 10, 8 << 20, 1 << 20), int64_t(8 << 20));
  // 弱网：20KB/s → 200KB 被 clamp 到下限 512KB
  e.reset();
  e.on_sample(20 << 10, 1000);
  ASSERT_EQ(suggest_part_size(e.bytes_per_sec(), 10, 512 << 10, 8 << 20, 1 << 20), int64_t(512 << 10));
  // 切网 reset 后回慢启动
  e.reset();
  ASSERT_FALSE(e.has_sample());
}

TEST(id, unique_and_prefixed) {
  auto a = gen_id("t"), b = gen_id("t");
  ASSERT_NE(a, b);
  ASSERT_EQ(a.substr(0, 2), std::string("t_"));
  ASSERT_EQ(a.size(), size_t(2 + 32));
}

