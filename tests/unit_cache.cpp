#include <vector>

#include "core/cache/cache.h"
#include "tests/harness.h"
#include "tests/testutil.h"

using namespace immd;

static void make_file(const std::string& p, size_t n, uint32_t seed) { immd_test::write_random_file(p, n, seed); }

TEST(cache, lru_eviction_with_callback) {
  auto dir = immd_test::make_temp_dir();
  auto c = MediaCache::open(dir + "/cache", 250 * 1024);  // 容量 250KB
  ASSERT_TRUE(c.ok());
  std::vector<std::string> evicted;
  c.value()->set_evict_cb([&](const std::string& sha) { evicted.push_back(sha); });

  for (int i = 0; i < 3; ++i) {  // 3 × 100KB → 超容量，最老的被淘汰
    std::string src = dir + "/f" + std::to_string(i);
    make_file(src, 100 * 1024, uint32_t(i));
    ASSERT_FALSE(c.value()->put("sha_" + std::to_string(i), src));
  }
  ASSERT_EQ(evicted.size(), size_t(1));
  ASSERT_EQ(evicted[0], std::string("sha_0"));
  ASSERT_FALSE(c.value()->lookup("sha_0").ok());
  ASSERT_TRUE(c.value()->lookup("sha_1").ok());
  ASSERT_TRUE(c.value()->lookup("sha_2").ok());
  ASSERT_LE(c.value()->total_bytes(), 250 * 1024);
}

TEST(cache, touch_protects_from_eviction) {
  auto dir = immd_test::make_temp_dir();
  auto c = MediaCache::open(dir + "/cache", 250 * 1024);
  ASSERT_TRUE(c.ok());
  for (int i = 0; i < 2; ++i) {
    std::string src = dir + "/f" + std::to_string(i);
    make_file(src, 100 * 1024, uint32_t(i));
    ASSERT_FALSE(c.value()->put("sha_" + std::to_string(i), src));
  }
  ASSERT_TRUE(c.value()->lookup("sha_0").ok());  // touch：sha_0 变最新
  std::string src = dir + "/f2";
  make_file(src, 100 * 1024, 2);
  ASSERT_FALSE(c.value()->put("sha_2", src));
  ASSERT_TRUE(c.value()->lookup("sha_0").ok());   // 被 touch 保护
  ASSERT_FALSE(c.value()->lookup("sha_1").ok());  // 未被 touch 的被淘汰
}

TEST(cache, reopen_rebuilds_index) {
  auto dir = immd_test::make_temp_dir();
  std::string cdir = dir + "/cache";
  {
    auto c = MediaCache::open(cdir, 10 * 1024 * 1024);
    ASSERT_TRUE(c.ok());
    std::string src = dir + "/f";
    make_file(src, 64 * 1024, 9);
    ASSERT_FALSE(c.value()->put("sha_x", src));
  }
  auto c2 = MediaCache::open(cdir, 10 * 1024 * 1024);
  ASSERT_TRUE(c2.ok());
  ASSERT_EQ(c2.value()->total_bytes(), 64 * 1024);
  ASSERT_TRUE(c2.value()->lookup("sha_x").ok());
}

