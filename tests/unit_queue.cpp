#include <atomic>
#include <thread>
#include <vector>

#include "core/util/clock.h"
#include "core/util/queue.h"
#include "tests/harness.h"

using namespace immd;

TEST(queue, mpsc_ordering_and_completeness) {
  BoundedQueue<int> q(1024);
  constexpr int kProducers = 4, kPerProducer = 25000;
  std::vector<std::thread> ps;
  for (int p = 0; p < kProducers; ++p) {
    ps.emplace_back([&q, p] {
      for (int i = 0; i < kPerProducer; ++i) q.push(p * kPerProducer + i);
    });
  }
  std::vector<int> seen(kProducers * kPerProducer, 0);
  int last_per_producer[kProducers];
  for (auto& v : last_per_producer) v = -1;
  for (int n = 0; n < kProducers * kPerProducer; ++n) {
    auto v = q.pop(-1);
    ASSERT_TRUE(v.has_value());
    seen[*v]++;
    int p = *v / kPerProducer, i = *v % kPerProducer;
    ASSERT_TRUE(i > last_per_producer[p]);  // 单生产者内保序
    last_per_producer[p] = i;
  }
  for (auto& t : ps) t.join();
  for (int c : seen) ASSERT_EQ(c, 1);  // 不丢不重
}

TEST(queue, try_push_fails_fast_when_full) {
  BoundedQueue<int> q(2);
  ASSERT_TRUE(q.try_push(1));
  ASSERT_TRUE(q.try_push(2));
  ASSERT_FALSE(q.try_push(3));  // 满 → 快速失败（Err::Busy 语义）
  q.pop(-1);
  ASSERT_TRUE(q.try_push(3));
}

TEST(queue, close_drains_then_rejects) {
  BoundedQueue<int> q(8);
  q.push(1);
  q.push(2);
  q.close();
  ASSERT_FALSE(q.push(3));  // 关闭拒新
  ASSERT_EQ(*q.pop(-1), 1);  // 已入队可排空
  ASSERT_EQ(*q.pop(-1), 2);
  ASSERT_TRUE(q.drained());
  ASSERT_FALSE(q.pop(10).has_value());
}

TEST(queue, pop_timeout_returns_nullopt) {
  BoundedQueue<int> q(8);
  auto t0 = RealClock::instance().now_ms();
  auto v = q.pop(30);
  auto dt = RealClock::instance().now_ms() - t0;
  ASSERT_FALSE(v.has_value());
  ASSERT_GE(dt, 25);
}

TEST(clock, virtual_clock_advances) {
  VirtualClock c(1000);
  ASSERT_EQ(c.now_ms(), 1000);
  c.advance_ms(500);
  ASSERT_EQ(c.now_ms(), 1500);
}

