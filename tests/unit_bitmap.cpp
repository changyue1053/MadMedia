#include "core/util/bitmap.h"
#include "tests/harness.h"

using namespace immd;

TEST(bitmap, set_get_count_missing) {
  PartBitmap b(10);
  ASSERT_EQ(b.count(), 0u);
  b.set(0);
  b.set(3);
  b.set(9);
  ASSERT_TRUE(b.get(0));
  ASSERT_FALSE(b.get(1));
  ASSERT_TRUE(b.get(9));
  ASSERT_EQ(b.count(), 3u);
  auto m = b.missing();
  ASSERT_EQ(m.size(), size_t(7));
  ASSERT_EQ(m[0], 1u);
  ASSERT_FALSE(b.complete());
  for (uint32_t i = 0; i < 10; ++i) b.set(i);
  ASSERT_TRUE(b.complete());
}

TEST(bitmap, blob_roundtrip_and_union) {
  PartBitmap b(19);
  b.set(2);
  b.set(17);
  auto restored = PartBitmap::from_blob(b.blob(), 19);
  ASSERT_TRUE(restored.get(2));
  ASSERT_TRUE(restored.get(17));
  ASSERT_EQ(restored.count(), 2u);

  PartBitmap server(19);
  server.set(5);
  server.set(2);
  restored.union_with(server);  // 以服务端为准的收敛：并集
  ASSERT_EQ(restored.count(), 3u);
  ASSERT_TRUE(restored.get(5));
}

TEST(bitmap, out_of_range_is_safe) {
  PartBitmap b(8);
  b.set(100);  // 越界写忽略
  ASSERT_FALSE(b.get(100));
  ASSERT_EQ(b.count(), 0u);
}

