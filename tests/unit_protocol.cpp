#include "core/protocol/api.h"
#include "tests/harness.h"

using namespace immd;

TEST(protocol, parse_init_three_answers) {
  auto a = parse_init(R"({"result":"instant","media_id":"m_1"})");
  ASSERT_TRUE(a.ok());
  ASSERT_EQ(int(a.value().kind), int(InitAnswer::Kind::Instant));
  ASSERT_EQ(a.value().media_id, std::string("m_1"));

  auto b = parse_init(
      R"({"result":"challenge","challenge_id":"c_1","nonce":"ab","ranges":[[100,4096],[9000,4096]]})");
  ASSERT_TRUE(b.ok());
  ASSERT_EQ(int(b.value().kind), int(InitAnswer::Kind::Challenge));
  ASSERT_EQ(b.value().ranges.size(), size_t(2));
  ASSERT_EQ(b.value().ranges[1].first, 9000);

  auto c = parse_init(
      R"({"result":"upload","upload_id":"u_1","part_size":262144,"nparts":6,"received_parts":[0,3]})");
  ASSERT_TRUE(c.ok());
  ASSERT_EQ(c.value().part_size, 262144);
  ASSERT_EQ(c.value().nparts, 6u);
  ASSERT_EQ(c.value().received.size(), size_t(2));
}

TEST(protocol, parse_init_rejects_malformed) {
  const char* bad[] = {
      R"({"result":"instant"})",                                    // 缺 media_id
      R"({"result":"upload","upload_id":"u","part_size":0,"nparts":1,"received_parts":[]})",
      R"({"result":"upload","part_size":1024,"nparts":1,"received_parts":[]})",  // 缺 upload_id
      R"({"result":"challenge","challenge_id":"c"})",               // 缺 nonce/ranges
      R"({"result":"wat"})",
      R"(not json)",
      R"({"result":"upload","upload_id":"u","part_size":1024,"nparts":2,"received_parts":["x"]})",
  };
  // nparts == 0 是合法的流式开单应答（总片数 complete 补报）
  auto streaming = parse_init(R"({"result":"upload","upload_id":"u","part_size":16384,"nparts":0,"received_parts":[]})");
  ASSERT_TRUE(streaming.ok());
  ASSERT_EQ(streaming.value().nparts, 0u);
  for (const char* t : bad) {
    auto p = parse_init(t);
    ASSERT_FALSE(p.ok());
    ASSERT_EQ(int(p.error().code), int(Err::Protocol));
  }
}

TEST(protocol, part_urls_and_complete_roundtrip) {
  auto u = parse_part_urls(R"({"urls":{"0":"http://a/0?sig=x","5":"http://a/5?sig=y"}})");
  ASSERT_TRUE(u.ok());
  ASSERT_EQ(u.value().size(), size_t(2));
  ASSERT_EQ(u.value().at(5), std::string("http://a/5?sig=y"));
  ASSERT_FALSE(parse_part_urls(R"({"urls":{"0":42}})").ok());

  auto ok = parse_complete_ok(R"({"media_id":"m_2","status":"PROCESSING"})");
  ASSERT_TRUE(ok.ok());
  ASSERT_EQ(ok.value().media_id, std::string("m_2"));
  auto miss = parse_complete_missing(R"({"error":"missing parts","missing":[2,7]})");
  ASSERT_TRUE(miss.ok());
  ASSERT_EQ(miss.value().missing.size(), size_t(2));
  ASSERT_EQ(miss.value().missing[1], 7u);

  auto dl = parse_dl(R"({"url":"http://a/obj?sig=z","size":1024,"sha256":"ab"})");
  ASSERT_TRUE(dl.ok());
  ASSERT_EQ(dl.value().size, 1024);
  ASSERT_FALSE(parse_dl(R"({"size":1024})").ok());
}

TEST(protocol, build_init_contains_contract_fields) {
  TaskRecord r;
  r.task_id = "t_x";
  r.kind = TaskKind::Upload;
  r.mime = "image/webp";
  r.size = 300;
  r.sha256_hex = "ab";
  r.quick_hash_hex = "cd";
  Config cfg;
  cfg.tenant_id = "t_9";
  std::string body = build_init(r, cfg, 262144);
  ASSERT_TRUE(body.find("\"client_task_id\":\"t_x\"") != std::string::npos);
  ASSERT_TRUE(body.find("\"tenant_id\":\"t_9\"") != std::string::npos);
  ASSERT_TRUE(body.find("\"suggest_part_size\":262144") != std::string::npos);
}

