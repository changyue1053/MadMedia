#include <random>
#include <string>

#include "core/crypto/crc32c.h"
#include "core/crypto/sha256.h"
#include "tests/harness.h"

using namespace immd;

TEST(sha256, nist_vectors) {
  ASSERT_EQ(to_hex(Sha256::digest("", 0)),
            std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  ASSERT_EQ(to_hex(Sha256::digest("abc", 3)),
            std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  std::string abcdbl = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  ASSERT_EQ(to_hex(Sha256::digest(abcdbl.data(), abcdbl.size())),
            std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  std::string mil(1000000, 'a');
  ASSERT_EQ(to_hex(Sha256::digest(mil.data(), mil.size())),
            std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(sha256, streaming_equals_oneshot) {
  std::string data(300000, 0);
  std::mt19937 rng(42);
  for (auto& c : data) c = char(rng() & 0xff);
  auto oneshot = Sha256::digest(data.data(), data.size());
  std::mt19937 cut(7);
  Sha256 s;
  size_t pos = 0;
  while (pos < data.size()) {
    size_t take = 1 + cut() % 70000;
    if (pos + take > data.size()) take = data.size() - pos;
    s.update(data.data() + pos, take);
    pos += take;
  }
  ASSERT_EQ(to_hex(s.finish()), to_hex(oneshot));
}

TEST(hmac, rfc4231_vectors) {
  std::string key1(20, char(0x0b));
  ASSERT_EQ(to_hex(hmac_sha256(key1.data(), key1.size(), "Hi There", 8)),
            std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
  std::string key2 = "Jefe", msg2 = "what do ya want for nothing?";
  ASSERT_EQ(to_hex(hmac_sha256(key2.data(), key2.size(), msg2.data(), msg2.size())),
            std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
  // key > block size 分支
  std::string key3(131, char(0xaa));
  std::string msg3 = "Test Using Larger Than Block-Size Key - Hash Key First";
  ASSERT_EQ(to_hex(hmac_sha256(key3.data(), key3.size(), msg3.data(), msg3.size())),
            std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

TEST(crc32c, check_value_and_hw_sw_agree) {
  ASSERT_EQ(crc32c(0, "123456789", 9), 0xE3069283u);
  ASSERT_EQ(crc32c(0, "", 0), 0u);
  std::mt19937 rng(9);
  for (int round = 0; round < 20; ++round) {
    std::string buf(1 + rng() % 5000, 0);
    for (auto& c : buf) c = char(rng() & 0xff);
    ASSERT_EQ(crc32c(0, buf.data(), buf.size()), crc32c_sw(0, buf.data(), buf.size()));
  }
  // 增量 == 一次性
  std::string a = "hello ", b = "world";
  uint32_t inc = crc32c(crc32c(0, a.data(), a.size()), b.data(), b.size());
  std::string ab = a + b;
  ASSERT_EQ(inc, crc32c(0, ab.data(), ab.size()));
}

