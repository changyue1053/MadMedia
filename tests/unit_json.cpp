#include <random>
#include <string>

#include "core/util/json.h"
#include "tests/harness.h"

using namespace immd;

TEST(json, roundtrip_object) {
  Json j = Json::object();
  j.set("result", Json::str("upload"))
      .set("upload_id", Json::str("u_5e"))
      .set("part_size", Json::number(4194304))
      .set("ok", Json::boolean(true))
      .set("received_parts", Json::array().push(Json::number(1)).push(Json::number(7)));
  auto p = Json::parse(j.dump());
  ASSERT_TRUE(p.ok());
  auto& v = p.value();
  ASSERT_EQ(v.get_str("result"), std::string("upload"));
  ASSERT_EQ(v.get_i64("part_size"), 4194304);
  ASSERT_TRUE(v.get_bool("ok"));
  ASSERT_EQ(v.find("received_parts")->arr().size(), size_t(2));
  ASSERT_EQ(v.find("received_parts")->arr()[1].i64(), 7);
}

TEST(json, big_integers_stay_integral) {
  // 8GB 文件 size 这类值不能被序列化成科学计数法
  Json j = Json::object();
  j.set("size", Json::number(8589934592.0));
  ASSERT_EQ(j.dump(), std::string("{\"size\":8589934592}"));
  auto p = Json::parse(j.dump());
  ASSERT_TRUE(p.ok());
  ASSERT_EQ(p.value().get_i64("size"), 8589934592LL);
}

TEST(json, escapes_and_unicode) {
  auto p = Json::parse(R"({"s":"a\"b\\c\n\u4e2d\ud83d\ude00"})");
  ASSERT_TRUE(p.ok());
  const std::string& s = p.value().find("s")->sval();
  ASSERT_EQ(s, std::string("a\"b\\c\n\xe4\xb8\xad\xf0\x9f\x98\x80"));
  // dump 后能再 parse 回同值
  auto p2 = Json::parse(p.value().dump());
  ASSERT_TRUE(p2.ok());
  ASSERT_EQ(p2.value().find("s")->sval(), s);
}

TEST(json, malformed_inputs_error_not_crash) {
  const char* bad[] = {
      "",           "{",           "[1,",        "{\"a\":}",   "{\"a\" 1}",  "tru",
      "01x",        "\"unterm",    "{\"a\":1}g", "[1 2]",      "{\"a\":\x01\"b\"}",
      "\"\\u12\"",  "\"\\ud800\"", "\"\\q\"",    "nul",        "[",          "]",
  };
  for (const char* t : bad) {
    auto p = Json::parse(t);
    ASSERT_FALSE(p.ok());
    ASSERT_EQ(int(p.error().code), int(Err::Protocol));
  }
}

TEST(json, depth_limit_enforced) {
  std::string deep(64, '[');
  deep += std::string(64, ']');
  ASSERT_FALSE(Json::parse(deep).ok());
  std::string ok(20, '[');
  ok += std::string(20, ']');
  ASSERT_TRUE(Json::parse(ok).ok());
}

TEST(json, fuzz_lite_mutations_never_crash) {
  std::string seedDoc =
      R"({"result":"upload","upload_id":"u_1","part_size":4194304,"received_parts":[1,2,3],)"
      R"("nested":{"a":[true,false,null,1.5e3],"s":"\u4e2d\n"}})";
  std::mt19937 rng(20260830);
  for (int i = 0; i < 3000; ++i) {
    std::string doc = seedDoc;
    int muts = 1 + rng() % 6;
    for (int m = 0; m < muts; ++m) {
      size_t pos = rng() % doc.size();
      switch (rng() % 3) {
        case 0: doc[pos] = char(rng() & 0xff); break;
        case 1: doc.insert(pos, 1, char(rng() & 0xff)); break;
        case 2: doc.erase(pos, 1); break;
      }
    }
    Json::parse(doc);  // 只要求不崩溃不越界（ASan 配置下跑同一套）
  }
  // 纯随机垃圾
  for (int i = 0; i < 1000; ++i) {
    std::string doc(rng() % 200, 0);
    for (auto& c : doc) c = char(rng() & 0xff);
    Json::parse(doc);
  }
}

