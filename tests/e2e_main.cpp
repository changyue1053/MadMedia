// e2e：真进程 mock 控制面 + 回环网络。
// 本文件当前覆盖 P2 冒烟（裸协议三段式）；引擎级剧本随 P3/P4 落地追加。
#include <memory>

#include "core/crypto/crc32c.h"
#include "core/crypto/sha256.h"
#include "core/parser/mp4box.h"
#include "core/transport/http.h"
#include "core/util/clock.h"
#include "core/util/fs.h"
#include "core/util/json.h"
#include "tests/harness.h"
#include "tests/httputil.h"
#include "tests/mediautil.h"
#include "tests/mockproc.h"
#include "tests/testutil.h"

using namespace immd;
using namespace immd_test;

static MockCtl* g_mock = nullptr;
static HttpClient* g_http = nullptr;

static std::string file_sha256_hex(const std::string& path) {
  auto data = read_file(path);
  return to_hex(Sha256::digest(data.data(), data.size()));
}

TEST(p2_smoke, mock_is_up) {
  auto st = g_mock->stats(*g_http);
  ASSERT_TRUE(st.is_obj());
  ASSERT_TRUE(st.has("stats"));
}

TEST(p2_smoke, raw_three_phase_upload_dedup_and_range_download) {
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("challenge_on_dedup", Json::boolean(false)));
  // 设置必须读回验证（测试自身的 setup 也要有断言）
  auto st0 = g_mock->stats(*g_http);
  ASSERT_TRUE(st0.find("faults") != nullptr);
  ASSERT_FALSE(st0.find("faults")->get_bool("challenge_on_dedup", true));
  const std::string base = g_mock->base();
  auto dir = make_temp_dir();
  std::string src = dir + "/src.bin";
  const int64_t kSize = 1500 * 1024;  // 1.5MB
  write_random_file(src, kSize, 11);
  std::string sha = file_sha256_hex(src);

  // ---- init（建议 256KB 分片 → 6 片）----
  Json init = Json::object();
  init.set("client_task_id", Json::str("t_raw_1"))
      .set("kind", Json::str("file"))
      .set("mime", Json::str("application/octet-stream"))
      .set("size", Json::number(double(kSize)))
      .set("sha256", Json::str(sha))
      .set("suggest_part_size", Json::number(256 * 1024));
  auto r1 = post_json(*g_http, base + "/v1/media/init", init);
  ASSERT_EQ(r1.status, 200);
  auto j1 = Json::parse(r1.body);
  ASSERT_TRUE(j1.ok());
  ASSERT_EQ(j1.value().get_str("result"), std::string("upload"));
  std::string uid = j1.value().get_str("upload_id");
  int64_t part_size = j1.value().get_i64("part_size");
  int64_t nparts = j1.value().get_i64("nparts");
  ASSERT_EQ(part_size, 256 * 1024);
  ASSERT_EQ(nparts, 6);

  // ---- 领 URL + 流式 PUT 每片（带 CRC32C 头）----
  Json want = Json::object().set("upload_id", Json::str(uid));
  Json parts = Json::array();
  for (int p = 0; p < nparts; ++p) parts.push(Json::number(p));
  want.set("parts", std::move(parts));
  auto r2 = post_json(*g_http, base + "/v1/media/part_urls", want);
  ASSERT_EQ(r2.status, 200);
  auto j2 = Json::parse(r2.body);
  ASSERT_TRUE(j2.ok());
  auto data = read_file(src);
  for (int p = 0; p < nparts; ++p) {
    int64_t off = p * part_size;
    int64_t len = std::min<int64_t>(part_size, kSize - off);
    uint32_t crc = crc32c(0, data.data() + off, size_t(len));
    char crchex[16];
    snprintf(crchex, sizeof(crchex), "%08x", crc);
    HttpRequest pr;
    pr.method = "PUT";
    pr.url = j2.value().find("urls")->get_str(std::to_string(p));
    ASSERT_TRUE(!pr.url.empty());
    pr.upload_path = src;
    pr.upload_offset = off;
    pr.upload_len = len;
    pr.headers.push_back(std::string("x-part-crc32c: ") + crchex);
    auto resp = http_sync(*g_http, pr);
    ASSERT_EQ(int(resp.err), int(Err::Ok));
    ASSERT_EQ(resp.status, 200);
  }

  // ---- complete：全量 SHA 校验，拿 media_id ----
  Json comp = Json::object().set("upload_id", Json::str(uid)).set("sha256", Json::str(sha));
  auto r3 = post_json(*g_http, base + "/v1/media/complete", comp);
  ASSERT_EQ(r3.status, 200);
  auto j3 = Json::parse(r3.body);
  ASSERT_TRUE(j3.ok());
  std::string media_id = j3.value().get_str("media_id");
  ASSERT_TRUE(!media_id.empty());

  // ---- 秒传：同内容不同任务 → instant ----
  init.set("client_task_id", Json::str("t_raw_2"));
  auto r4 = post_json(*g_http, base + "/v1/media/init", init);
  ASSERT_EQ(r4.status, 200);
  auto j4 = Json::parse(r4.body);
  ASSERT_TRUE(j4.ok());
  ASSERT_EQ(j4.value().get_str("result"), std::string("instant"));
  ASSERT_EQ(j4.value().get_str("media_id"), media_id);

  // ---- 下载：凭证 + Range 两段写偏移，比对原始字节 ----
  auto r5 = post_json(*g_http, base + "/v1/media/dl_url",
                      Json::object().set("media_id", Json::str(media_id)));
  ASSERT_EQ(r5.status, 200);
  auto j5 = Json::parse(r5.body);
  ASSERT_TRUE(j5.ok());
  std::string url = j5.value().get_str("url");
  std::string dst = dir + "/dst.bin";
  int64_t half = kSize / 2;
  {
    HttpRequest g1;
    g1.url = url;
    g1.download_path = dst;
    g1.download_offset = 0;
    g1.headers.push_back("Range: bytes=0-" + std::to_string(half - 1));
    auto resp = http_sync(*g_http, g1);
    ASSERT_EQ(resp.status, 206);
    ASSERT_EQ(resp.bytes_down, half);
  }
  {
    HttpRequest g2;
    g2.url = url;
    g2.download_path = dst;
    g2.download_offset = half;
    g2.headers.push_back("Range: bytes=" + std::to_string(half) + "-" + std::to_string(kSize - 1));
    auto resp = http_sync(*g_http, g2);
    ASSERT_EQ(resp.status, 206);
    ASSERT_EQ(resp.bytes_down, kSize - half);
  }
  ASSERT_EQ(file_sha256_hex(dst), sha);
}

TEST(p2_smoke, transport_error_classification) {
  // 关闭端口 → Network（快速失败）
  HttpRequest r;
  r.url = "http://127.0.0.1:1/nope";
  r.connect_timeout_ms = 800;
  r.timeout_ms = 1500;
  auto resp = http_sync(*g_http, r);
  ASSERT_EQ(int(resp.err), int(Err::Network));
  // 过期签名 → HTTP 403（凭证换新路径的触发条件）
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("expire_urls", Json::boolean(true)));
  auto dir = make_temp_dir();
  std::string src = dir + "/s.bin";
  write_random_file(src, 4096, 3);
  Json init = Json::object();
  init.set("client_task_id", Json::str("t_exp"))
      .set("kind", Json::str("file"))
      .set("mime", Json::str("application/octet-stream"))
      .set("size", Json::number(4096))
      .set("sha256", Json::str(file_sha256_hex(src)))
      .set("suggest_part_size", Json::number(64 * 1024));
  auto r1 = post_json(*g_http, g_mock->base() + "/v1/media/init", init);
  ASSERT_EQ(r1.status, 200);
  std::string uid = Json::parse(r1.body).value().get_str("upload_id");
  auto r2 = post_json(*g_http, g_mock->base() + "/v1/media/part_urls",
                      Json::object().set("upload_id", Json::str(uid)).set("parts", Json::array().push(Json::number(0))));
  HttpRequest pr;
  pr.method = "PUT";
  pr.url = Json::parse(r2.body).value().find("urls")->get_str("0");
  pr.upload_path = src;
  pr.upload_offset = 0;
  pr.upload_len = 4096;
  auto resp2 = http_sync(*g_http, pr);
  ASSERT_EQ(resp2.status, 403);
  g_mock->set_faults(*g_http, Json::object().set("expire_urls", Json::boolean(false)));
}

// ==================== 引擎级剧本（P3/P4 验收）====================
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "core/engine/engine.h"

namespace {

struct Observer {
  std::mutex mu;
  std::condition_variable cv;
  std::map<std::string, ProgressEvent> last;

  ProgressCallback callback() {
    return [this](const ProgressEvent& ev) {
      std::lock_guard<std::mutex> lk(mu);
      last[ev.task_id] = ev;
      cv.notify_all();
    };
  }
  // 等待任务达到指定状态；返回该事件
  bool wait_state(const std::string& id, TaskState st, int64_t timeout_ms, ProgressEvent* out = nullptr) {
    std::unique_lock<std::mutex> lk(mu);
    bool ok = cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
      auto it = last.find(id);
      return it != last.end() && it->second.state == st;
    });
    if (ok && out) *out = last[id];
    return ok;
  }
};

immd::Config test_cfg(const std::string& dir) {
  immd::Config cfg;
  cfg.base_url = g_mock->base();
  cfg.data_dir = dir;
  cfg.force_part_size = 128 * 1024;
  cfg.max_parts_per_task = 3;
  cfg.max_total_streams = 4;
  cfg.retry_base_ms = 40;
  cfg.retry_cap_ms = 300;
  cfg.part_retry_limit = 6;
  cfg.http_timeout_ms = 8000;
  cfg.connect_timeout_ms = 2000;
  cfg.cache_capacity = 64 * 1024 * 1024;
  return cfg;
}

// mock 统计口径：某 upload 会话每个分片的成功 PUT 次数
std::map<std::string, int64_t> part_put_counts(Json stats) {
  std::map<std::string, int64_t> out;
  const Json* puts = stats.find("stats") ? stats.find("stats")->find("part_put_ok") : nullptr;
  if (puts && puts->is_obj())
    for (auto& kv : puts->obj()) out[kv.first] = kv.second.i64();
  return out;
}

int64_t stat_i64(Json stats, const char* key) {
  const Json* s = stats.find("stats");
  return s ? s->get_i64(key) : -1;
}

}  // namespace

TEST(engine, upload_small_then_dedup_via_challenge) {
  g_mock->reset(*g_http);  // challenge_on_dedup 默认开
  auto dir = make_temp_dir();
  std::string f1 = dir + "/a.bin", f2 = dir + "/b.bin";
  write_random_file(f1, 100 * 1024, 21);
  write_random_file(f2, 100 * 1024, 21);  // 同内容不同文件

  Observer ob;
  auto eng = Engine::start(test_cfg(dir + "/e1"), ob.callback());
  ASSERT_TRUE(eng.ok());
  auto id1 = eng.value()->upload({f1, "application/octet-stream", Priority::P2_Image, ""});
  ASSERT_TRUE(id1.ok());
  ProgressEvent ev1;
  ASSERT_TRUE(ob.wait_state(id1.value(), TaskState::Ready, 10000, &ev1));
  ASSERT_TRUE(!ev1.media_id.empty());
  int64_t bytes_after_first = stat_i64(g_mock->stats(*g_http), "bytes_received");

  auto id2 = eng.value()->upload({f2, "application/octet-stream", Priority::P2_Image, ""});
  ASSERT_TRUE(id2.ok());
  ProgressEvent ev2;
  ASSERT_TRUE(ob.wait_state(id2.value(), TaskState::Ready, 10000, &ev2));
  ASSERT_EQ(ev2.media_id, ev1.media_id);  // 秒传：同一对象
  auto st = g_mock->stats(*g_http);
  ASSERT_EQ(stat_i64(st, "bytes_received"), bytes_after_first);  // 第二次零字节上传
  ASSERT_GE(stat_i64(st, "challenge_count"), 1);                 // 持有证明真的发生过
}

TEST(engine, multipart_upload_survives_injected_5xx) {
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("fail_part_puts", Json::number(3)));
  auto dir = make_temp_dir();
  std::string f = dir + "/c.bin";
  write_random_file(f, 1024 * 1024, 31);  // 8 × 128KB

  Observer ob;
  auto eng = Engine::start(test_cfg(dir + "/e"), ob.callback());
  ASSERT_TRUE(eng.ok());
  auto id = eng.value()->upload({f, "application/octet-stream", Priority::P4_File, ""});
  ASSERT_TRUE(id.ok());
  ASSERT_TRUE(ob.wait_state(id.value(), TaskState::Ready, 15000));
  // 全部分片最终恰好成功一次（失败的注入 PUT 不计入 part_put_ok）
  auto counts = part_put_counts(g_mock->stats(*g_http));
  ASSERT_EQ(counts.size(), size_t(8));
  for (auto& kv : counts) ASSERT_EQ(kv.second, 1);
}

TEST(engine, complete_409_missing_parts_backfill) {
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("forget_parts_on_complete", Json::number(2)));
  auto dir = make_temp_dir();
  std::string f = dir + "/d.bin";
  write_random_file(f, 640 * 1024, 41);  // 5 片

  Observer ob;
  auto eng = Engine::start(test_cfg(dir + "/e"), ob.callback());
  ASSERT_TRUE(eng.ok());
  auto id = eng.value()->upload({f, "application/octet-stream", Priority::P4_File, ""});
  ASSERT_TRUE(id.ok());
  ASSERT_TRUE(ob.wait_state(id.value(), TaskState::Ready, 15000));
  auto counts = part_put_counts(g_mock->stats(*g_http));
  int64_t twice = 0;
  for (auto& kv : counts) {
    ASSERT_GE(kv.second, 1);
    if (kv.second == 2) ++twice;
    ASSERT_LE(kv.second, 2);
  }
  ASSERT_EQ(twice, 2);  // 被"忘掉"的两片回补重传，其余不重传
}

TEST(engine, url_expiry_triggers_refresh) {
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("expire_urls", Json::boolean(true)));
  auto dir = make_temp_dir();
  std::string f = dir + "/e.bin";
  write_random_file(f, 384 * 1024, 51);  // 3 片

  Observer ob;
  auto eng = Engine::start(test_cfg(dir + "/e"), ob.callback());
  ASSERT_TRUE(eng.ok());
  auto id = eng.value()->upload({f, "application/octet-stream", Priority::P4_File, ""});
  ASSERT_TRUE(id.ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(400));  // 让 403 → 换新循环转几轮
  g_mock->set_faults(*g_http, Json::object().set("expire_urls", Json::boolean(false)));
  ASSERT_TRUE(ob.wait_state(id.value(), TaskState::Ready, 15000));
  auto counts = part_put_counts(g_mock->stats(*g_http));
  for (auto& kv : counts) ASSERT_EQ(kv.second, 1);  // 403 不落字节，成功各一次
}

TEST(engine, crash_recovery_zero_reupload) {
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("slow_ms", Json::number(60)));
  auto dir = make_temp_dir();
  std::string f = dir + "/big.bin";
  write_random_file(f, 1536 * 1024, 61);  // 12 × 128KB
  std::string edir = dir + "/e";
  std::string task_id;

  {  // 第一段生命周期：传到一半"崩溃"（析构即硬停，journal 留现场）
    Observer ob;
    auto eng = Engine::start(test_cfg(edir), ob.callback());
    ASSERT_TRUE(eng.ok());
    auto id = eng.value()->upload({f, "application/octet-stream", Priority::P4_File, ""});
    ASSERT_TRUE(id.ok());
    task_id = id.value();
    // 等到至少 4 片落地
    for (int i = 0; i < 300; ++i) {
      auto q = eng.value()->query(task_id);
      if (q.ok() && q.value().bytes_done >= 4 * 128 * 1024) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    auto q = eng.value()->query(task_id);
    ASSERT_TRUE(q.ok());
    ASSERT_GE(q.value().bytes_done, 4 * 128 * 1024);
    // eng 析构 = 硬停（在途分片被取消，不清账）
  }

  g_mock->set_faults(*g_http, Json::object().set("slow_ms", Json::number(0)));
  {  // 第二段生命周期：recover → init 幂等对账 → 续传
    Observer ob;
    auto eng = Engine::start(test_cfg(edir), ob.callback());
    ASSERT_TRUE(eng.ok());
    ASSERT_FALSE(eng.value()->recover());
    ASSERT_TRUE(ob.wait_state(task_id, TaskState::Ready, 20000));
  }
  // 零重传的诚实语义边界（实施计划 P3.2 DoD）：
  //   非在途分片绝不重传（init 对账以服务端为准）；
  //   崩溃瞬间在途的分片存在固有竞态——字节已进 socket，服务端可能在客户端死后才记账、
  //   晚于新引擎的对账快照（S3 ListParts 同语义）——允许 ≤ 分片并发数（3）的重传。
  auto counts = part_put_counts(g_mock->stats(*g_http));
  ASSERT_EQ(counts.size(), size_t(12));
  int64_t dup = 0;
  for (auto& kv : counts) {
    ASSERT_GE(kv.second, 1);
    ASSERT_LE(kv.second, 2);  // 任何分片最多两次（一次原始 + 一次在途竞态重传）
    if (kv.second == 2) ++dup;
  }
  ASSERT_LE(dup, 3);  // 重传数 ≤ max_parts_per_task（在途窗口）
}

TEST(engine, p0_voice_preempts_big_upload) {
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("slow_ms", Json::number(80)));
  auto dir = make_temp_dir();
  std::string big = dir + "/big.bin", voice = dir + "/voice.bin";
  write_random_file(big, 3072 * 1024, 91);  // 24 片 P4：保证语音就绪时大文件仍在途
  write_random_file(voice, 60 * 1024, 92);  // 1 片 P0

  Observer ob;
  auto eng = Engine::start(test_cfg(dir + "/e"), ob.callback());
  ASSERT_TRUE(eng.ok());
  auto bid = eng.value()->upload({big, "application/octet-stream", Priority::P4_File, ""});
  ASSERT_TRUE(bid.ok());
  for (int i = 0; i < 300; ++i) {  // 等大文件进入传输期
    auto q = eng.value()->query(bid.value());
    if (q.ok() && q.value().bytes_done >= 2 * 128 * 1024) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  auto t0 = RealClock::instance().now_ms();
  auto vid = eng.value()->upload({voice, "audio/opus", Priority::P0_Voice, ""});
  ASSERT_TRUE(vid.ok());
  ASSERT_TRUE(ob.wait_state(vid.value(), TaskState::Ready, 8000));
  int64_t voice_ms = RealClock::instance().now_ms() - t0;
  // 语音在大文件仍在途（Transferring/Finalizing）时就绪，不等大文件排空
  //（架构 §8：分片边界抢占 + 单任务并发上限给高优留出流额度）
  auto q = eng.value()->query(bid.value());
  ASSERT_TRUE(q.ok());
  ASSERT_NE(int(q.value().state), int(TaskState::Ready));
  ASSERT_LE(voice_ms, 3000);
  ASSERT_TRUE(ob.wait_state(bid.value(), TaskState::Ready, 20000));
}

TEST(engine, chaos_random_kill_faults_converge) {
  // 随机化收敛（实施计划 P3.6 缩量版）：随机故障 × 随机时点硬停 × recover，
  // 终态必须收敛（Ready），分片计数在 {1,2} 且服务端对象完整。种子固定可复现。
  std::mt19937 rng(20260830);
  for (int round = 0; round < 5; ++round) {
    g_mock->reset(*g_http);
    Json faults = Json::object();
    faults.set("slow_ms", Json::number(double(20 + rng() % 50)));
    if (rng() % 2) faults.set("fail_part_puts", Json::number(double(rng() % 3)));
    if (rng() % 3 == 0) faults.set("forget_parts_on_complete", Json::number(1));
    g_mock->set_faults(*g_http, faults);

    auto dir = make_temp_dir();
    std::string f = dir + "/chaos.bin";
    int64_t sz = int64_t(512 * 1024 + (rng() % 8) * 128 * 1024);
    write_random_file(f, size_t(sz), rng());
    std::string sha = file_sha256_hex(f);
    std::string edir = dir + "/e";
    std::string task;
    {
      Observer ob;
      auto eng = Engine::start(test_cfg(edir), ob.callback());
      ASSERT_TRUE(eng.ok());
      auto id = eng.value()->upload({f, "application/octet-stream", Priority::P3_Video, ""});
      ASSERT_TRUE(id.ok());
      task = id.value();
      std::this_thread::sleep_for(std::chrono::milliseconds(80 + rng() % 300));
      // 随机时点硬停（析构）
    }
    g_mock->set_faults(*g_http, Json::object().set("slow_ms", Json::number(0)));
    {
      Observer ob;
      auto eng = Engine::start(test_cfg(edir), ob.callback());
      ASSERT_TRUE(eng.ok());
      ASSERT_FALSE(eng.value()->recover());
      ProgressEvent ev;
      // 先查账本：硬停可能发生在 Ready 之后（终态任务 recover 不再产生事件）
      auto q0 = eng.value()->query(task);
      ASSERT_TRUE(q0.ok());
      if (q0.value().state == TaskState::Ready) {
        ev.media_id = q0.value().media_id;
      } else {
        ASSERT_TRUE(ob.wait_state(task, TaskState::Ready, 25000, &ev));
      }
      ASSERT_TRUE(!ev.media_id.empty());
      // 服务端对象完整可下载（内容一致性收口）
      auto r = post_json(*g_http, g_mock->base() + "/v1/media/dl_url",
                         Json::object().set("media_id", Json::str(ev.media_id)));
      ASSERT_EQ(r.status, 200);
      ASSERT_EQ(Json::parse(r.body).value().get_str("sha256"), sha);
    }
    auto counts = part_put_counts(g_mock->stats(*g_http));
    for (auto& kv : counts) {
      ASSERT_GE(kv.second, 1);
      ASSERT_LE(kv.second, 2);
    }
  }
}

TEST(engine, download_roundtrip_and_integrity_check) {
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("challenge_on_dedup", Json::boolean(false)));
  auto dir = make_temp_dir();
  std::string f = dir + "/src.bin";
  write_random_file(f, 700 * 1024, 71);
  std::string sha = file_sha256_hex(f);

  Observer ob;
  auto eng = Engine::start(test_cfg(dir + "/e"), ob.callback());
  ASSERT_TRUE(eng.ok());
  auto up = eng.value()->upload({f, "application/octet-stream", Priority::P2_Image, ""});
  ASSERT_TRUE(up.ok());
  ProgressEvent ev;
  ASSERT_TRUE(ob.wait_state(up.value(), TaskState::Ready, 10000, &ev));

  // 正常下载 + 校验 + 字节一致
  std::string dst = dir + "/dl.bin";
  auto dl = eng.value()->download({ev.media_id, dst, Priority::P2_Image, sha, ""});
  ASSERT_TRUE(dl.ok());
  ASSERT_TRUE(ob.wait_state(dl.value(), TaskState::Ready, 10000));
  ASSERT_EQ(file_sha256_hex(dst), sha);

  // 错误指纹 → Integrity 失败（消息引用里的 sha 是最后一道门）
  std::string dst2 = dir + "/dl2.bin";
  auto dl2 = eng.value()->download({ev.media_id, dst2, Priority::P2_Image, std::string(64, 'a'), ""});
  ASSERT_TRUE(dl2.ok());
  ASSERT_TRUE(ob.wait_state(dl2.value(), TaskState::Failed, 10000));
}

TEST(engine, sync_review_waits_for_available) {
  // 合规窗口严档（架构 §6）：ready_on_uploaded=false → 等流水线 AVAILABLE 才 READY
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("processing_delay_ms", Json::number(700)));
  auto dir = make_temp_dir();
  std::string f = dir + "/s.bin";
  write_random_file(f, 100 * 1024, 121);
  Observer ob;
  auto cfg = test_cfg(dir + "/e");
  cfg.ready_on_uploaded = false;  // 同步审租户
  auto eng = Engine::start(cfg, ob.callback());
  ASSERT_TRUE(eng.ok());
  auto t0 = RealClock::instance().now_ms();
  auto id = eng.value()->upload({f, "application/octet-stream", Priority::P2_Image, ""});
  ASSERT_TRUE(id.ok());
  ProgressEvent ev;
  ASSERT_TRUE(ob.wait_state(id.value(), TaskState::Ready, 15000, &ev));
  int64_t elapsed = RealClock::instance().now_ms() - t0;
  ASSERT_GE(elapsed, 600);  // PROCESSING 窗口真实生效（轮询等到 AVAILABLE）
  ASSERT_TRUE(!ev.media_id.empty());
}

TEST(engine, rejected_by_policy_fails_with_rejected_code) {
  // REJECTED 联动（架构 §6 补偿链的端侧半边）：错误分类必须可区分，业务据此撤回消息
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("reject_next_complete", Json::boolean(true)));
  auto dir = make_temp_dir();
  std::string f = dir + "/r.bin";
  write_random_file(f, 80 * 1024, 131);
  Observer ob;
  auto cfg = test_cfg(dir + "/e");
  cfg.ready_on_uploaded = false;  // 严档下 REJECTED 在发出前拦截
  auto eng = Engine::start(cfg, ob.callback());
  ASSERT_TRUE(eng.ok());
  auto id = eng.value()->upload({f, "application/octet-stream", Priority::P2_Image, ""});
  ASSERT_TRUE(id.ok());
  ProgressEvent ev;
  ASSERT_TRUE(ob.wait_state(id.value(), TaskState::Failed, 15000, &ev));
  ASSERT_EQ(int(ev.error), int(Err::Rejected));  // 不是笼统的 Network——业务要按此走撤回+告警
}

TEST(engine, cancel_mid_upload_cleans_and_persists_terminal) {
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("slow_ms", Json::number(60)));
  auto dir = make_temp_dir();
  std::string f = dir + "/c.bin";
  write_random_file(f, 1536 * 1024, 141);  // 12 片
  Observer ob;
  auto eng = Engine::start(test_cfg(dir + "/e"), ob.callback());
  ASSERT_TRUE(eng.ok());
  auto id = eng.value()->upload({f, "application/octet-stream", Priority::P4_File, ""});
  ASSERT_TRUE(id.ok());
  std::string staging;
  for (int i = 0; i < 300; ++i) {  // 等进入传输期并拿到 staging 路径
    auto q = eng.value()->query(id.value());
    if (q.ok() && q.value().bytes_done >= 2 * 128 * 1024) {
      staging = q.value().staging_path;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  ASSERT_TRUE(!staging.empty());
  ASSERT_FALSE(eng.value()->cancel(id.value()));
  ASSERT_TRUE(ob.wait_state(id.value(), TaskState::Canceled, 5000));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ASSERT_FALSE(file_exists(staging));  // 快照即清（服务端碎片交 TTL 收割，架构 §17 L1）
  auto q = eng.value()->query(id.value());
  ASSERT_TRUE(q.ok());
  ASSERT_EQ(int(q.value().state), int(TaskState::Canceled));  // journal 保留终态供审计
}

TEST(engine, runtime_rate_limit_takes_effect) {
  // 让路信号落点（架构 §7）：token bucket 真的在限流，且可运行时换档
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("challenge_on_dedup", Json::boolean(false)));
  auto dir = make_temp_dir();
  std::string f1 = dir + "/rl1.bin", f2 = dir + "/rl2.bin";
  write_random_file(f1, 256 * 1024, 151);
  write_random_file(f2, 256 * 1024, 152);
  Observer ob;
  auto cfg = test_cfg(dir + "/e");
  cfg.force_part_size = 64 * 1024;
  auto eng = Engine::start(cfg, ob.callback());
  ASSERT_TRUE(eng.ok());

  ASSERT_FALSE(eng.value()->set_rate_limit(64 * 1024));  // 64KB/s：4 片 ≈ 满桶 1 片 + 3 秒
  auto t0 = RealClock::instance().now_ms();
  auto id1 = eng.value()->upload({f1, "application/octet-stream", Priority::P4_File, ""});
  ASSERT_TRUE(id1.ok());
  ASSERT_TRUE(ob.wait_state(id1.value(), TaskState::Ready, 20000));
  int64_t throttled = RealClock::instance().now_ms() - t0;
  ASSERT_GE(throttled, 2200);  // 限速真实生效（留调度余量）

  ASSERT_FALSE(eng.value()->set_rate_limit(0));  // 恢复不限速
  t0 = RealClock::instance().now_ms();
  auto id2 = eng.value()->upload({f2, "application/octet-stream", Priority::P4_File, ""});
  ASSERT_TRUE(id2.ok());
  ASSERT_TRUE(ob.wait_state(id2.value(), TaskState::Ready, 20000));
  ASSERT_LE(RealClock::instance().now_ms() - t0, 2000);  // 换档立即生效
}

TEST(engine, download_second_time_hits_cache_zero_network) {
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("challenge_on_dedup", Json::boolean(false)));
  auto dir = make_temp_dir();
  std::string f = dir + "/c.bin";
  write_random_file(f, 300 * 1024, 111);
  std::string sha = file_sha256_hex(f);

  Observer ob;
  auto eng = Engine::start(test_cfg(dir + "/e"), ob.callback());
  ASSERT_TRUE(eng.ok());
  auto up = eng.value()->upload({f, "application/octet-stream", Priority::P2_Image, ""});
  ASSERT_TRUE(up.ok());
  ProgressEvent ev;
  ASSERT_TRUE(ob.wait_state(up.value(), TaskState::Ready, 10000, &ev));

  // 首次下载：走网络，校验通过后异步回填缓存
  std::string d1 = dir + "/d1.bin";
  auto dl1 = eng.value()->download({ev.media_id, d1, Priority::P2_Image, sha, ""});
  ASSERT_TRUE(dl1.ok());
  ASSERT_TRUE(ob.wait_state(dl1.value(), TaskState::Ready, 10000));
  int64_t dl_count_1 = stat_i64(g_mock->stats(*g_http), "dl_url_count");
  ASSERT_GE(dl_count_1, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // 等回填任务落缓存

  // 二次下载（不同目标路径）：内容寻址命中，零网络（架构 §18 内容寻址第四次兑现）
  std::string d2 = dir + "/d2.bin";
  auto dl2 = eng.value()->download({ev.media_id, d2, Priority::P2_Image, sha, ""});
  ASSERT_TRUE(dl2.ok());
  ASSERT_TRUE(ob.wait_state(dl2.value(), TaskState::Ready, 10000));
  ASSERT_EQ(stat_i64(g_mock->stats(*g_http), "dl_url_count"), dl_count_1);  // 没有新凭证请求
  ASSERT_EQ(file_sha256_hex(d2), sha);
}

TEST(engine, mp4_tail_moov_remuxed_in_prepare_pipeline) {
  // parser 装车验证（架构 §13 "秒传/直传路径必须检测 moov"）：
  // 上传尾置 moov 的 MP4 → prepare 就地 remux → 服务端存的对象已是 faststart。
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("challenge_on_dedup", Json::boolean(false)));
  auto dir = make_temp_dir();
  auto mp4 = make_tail_moov_mp4(60 * 1024, 191);
  {
    auto probe = mp4_inspect(mp4.data(), mp4.size());
    ASSERT_TRUE(probe.ok());
    ASSERT_FALSE(probe.value().faststart);  // 输入确实尾置
  }
  std::string src = dir + "/v.mp4";
  write_bytes(src, mp4);

  Observer ob;
  auto eng = Engine::start(test_cfg(dir + "/e"), ob.callback());
  ASSERT_TRUE(eng.ok());
  auto up = eng.value()->upload({src, "video/mp4", Priority::P3_Video, ""});
  ASSERT_TRUE(up.ok());
  ProgressEvent ev;
  ASSERT_TRUE(ob.wait_state(up.value(), TaskState::Ready, 10000, &ev));

  // 下载回来检视：服务端对象必须已前置 moov，且元信息（时长）保留
  std::string dst = dir + "/back.mp4";
  auto dl = eng.value()->download({ev.media_id, dst, Priority::P3_Video, "", ""});
  ASSERT_TRUE(dl.ok());
  ASSERT_TRUE(ob.wait_state(dl.value(), TaskState::Ready, 10000));
  auto back = read_file(dst);
  auto info = mp4_inspect(reinterpret_cast<const uint8_t*>(back.data()), back.size());
  ASSERT_TRUE(info.ok());
  ASSERT_TRUE(info.value().faststart);  // 点开即播的承诺落到服务端对象上
  ASSERT_EQ(info.value().duration_ms, 5000);
  ASSERT_EQ(back.size(), mp4.size());  // 纯重排不改长度
}

TEST(engine, prepare_gate_rejects_bomb_and_corrects_mime) {
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("challenge_on_dedup", Json::boolean(false)));
  auto dir = make_temp_dir();
  Observer ob;
  auto eng = Engine::start(test_cfg(dir + "/e"), ob.callback());
  ASSERT_TRUE(eng.ok());

  // ① 解码炸弹：上传前即拒（Rejected 分类），一个字节都不出门
  std::string bomb = dir + "/bomb.jpg";
  write_bytes(bomb, make_bomb_jpeg());
  auto b = eng.value()->upload({bomb, "image/jpeg", Priority::P2_Image, ""});
  ASSERT_TRUE(b.ok());
  ProgressEvent bev;
  ASSERT_TRUE(ob.wait_state(b.value(), TaskState::Failed, 8000, &bev));
  ASSERT_EQ(int(bev.error), int(Err::Rejected));
  ASSERT_EQ(stat_i64(g_mock->stats(*g_http), "bytes_received"), 0);  // 零字节出门

  // ② 改后缀伪装：MZ 可执行声明成 image/jpeg → 魔数纠偏，服务端拿到真实类型
  std::string fake = dir + "/fake.jpg";
  write_bytes(fake, make_fake_exe(32 * 1024, 192));
  auto f = eng.value()->upload({fake, "image/jpeg", Priority::P2_Image, ""});
  ASSERT_TRUE(f.ok());
  ASSERT_TRUE(ob.wait_state(f.value(), TaskState::Ready, 8000));
  auto q = eng.value()->query(f.value());
  ASSERT_TRUE(q.ok());
  ASSERT_EQ(q.value().mime, std::string("application/x-msdownload"));
}

TEST(engine, adaptive_suggestion_feeds_next_task) {
  // 弱网自适应闭环（架构 §7）：任务 1 的实测吞吐（EWMA）必须传导进任务 2 的开单建议。
  // 冷启动 = part_first 慢启动；本地回环实测 ~20MB/s（python mock 口径）→ 任务 2 撞 part_max 钳位。
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("challenge_on_dedup", Json::boolean(false)));
  auto dir = make_temp_dir();
  auto cfg = test_cfg(dir + "/e");
  cfg.force_part_size = 0;  // 关掉测试注入位，走真自适应（first=1MB / min=512KB / max=8MB）

  Observer ob;
  auto eng = Engine::start(cfg, ob.callback());
  ASSERT_TRUE(eng.ok());
  std::string f1 = dir + "/a.bin", f2 = dir + "/b.bin";
  write_random_file(f1, 2560 * 1024, 171);  // 3 片 @1MB：产生吞吐样本
  write_random_file(f2, 100 * 1024, 172);
  auto id1 = eng.value()->upload({f1, "application/octet-stream", Priority::P3_Video, ""});
  ASSERT_TRUE(id1.ok());
  ASSERT_TRUE(ob.wait_state(id1.value(), TaskState::Ready, 20000));
  auto id2 = eng.value()->upload({f2, "application/octet-stream", Priority::P3_Video, ""});
  ASSERT_TRUE(id2.ok());
  ASSERT_TRUE(ob.wait_state(id2.value(), TaskState::Ready, 20000));

  Json st = g_mock->stats(*g_http);
  const Json* suggests = st.find("stats") ? st.find("stats")->find("init_suggests") : nullptr;
  if (!suggests || suggests->arr().size() != 2) {
    std::printf("ADAPTIVE-DIAG stats: %s\n", st.dump().c_str());
  }
  ASSERT_TRUE(suggests != nullptr && suggests->arr().size() == 2);
  ASSERT_EQ(suggests->arr()[0].i64(), int64_t(1) << 20);  // 冷启动：慢启动首选 1MB
  ASSERT_EQ(suggests->arr()[1].i64(), int64_t(8) << 20);  // 闭环：EWMA×目标时长撞 8MB 钳位
}

TEST(engine, weaknet_mini_matrix) {
  // P9.1 弱网矩阵缩量版（toxiproxy 的 mock 近似）：带宽整形 / 慢响应 / 半途断连 × 文件尺寸。
  struct Cell {
    const char* name;
    int64_t bw_bps;
    int slow_ms;
    int drops;
    size_t size;
    int64_t timeout_ms;
  };
  const Cell cells[] = {
      {"fast-384k", 0, 0, 0, 384 * 1024, 10000},
      {"96kbps-96k", 96 * 1024, 20, 0, 96 * 1024, 15000},
      {"96kbps-384k", 96 * 1024, 20, 0, 384 * 1024, 25000},
      {"drop2-192k", 0, 20, 2, 192 * 1024, 15000},
  };
  int seed = 180;
  for (const auto& c : cells) {
    g_mock->reset(*g_http);
    Json f = Json::object();
    f.set("challenge_on_dedup", Json::boolean(false))
        .set("bandwidth_bps", Json::number(double(c.bw_bps)))
        .set("slow_ms", Json::number(c.slow_ms))
        .set("drop_connections", Json::number(c.drops));
    g_mock->set_faults(*g_http, f);

    auto dir = make_temp_dir();
    std::string file = dir + "/w.bin";
    write_random_file(file, c.size, uint32_t(seed++));
    Observer ob;
    auto eng = Engine::start(test_cfg(dir + "/e"), ob.callback());
    ASSERT_TRUE(eng.ok());
    auto id = eng.value()->upload({file, "application/octet-stream", Priority::P4_File, ""});
    ASSERT_TRUE(id.ok());
    if (!ob.wait_state(id.value(), TaskState::Ready, c.timeout_ms)) {
      std::printf("weaknet cell FAILED: %s\n", c.name);
      ASSERT_TRUE(false);
    }
    // 弱网下的账仍要对：每片 1..3 次（断连/整形只允许有限重传）
    for (auto& kv : part_put_counts(g_mock->stats(*g_http))) {
      ASSERT_GE(kv.second, 1);
      ASSERT_LE(kv.second, 3);
    }
  }
  g_mock->reset(*g_http);
}

TEST(engine, voice_streaming_records_then_finishes_subsecond) {
  // P6.5 边录边传：录制期间分片已在途，松手（finish）只补尾片 + hash + complete
  g_mock->reset(*g_http);
  auto dir = make_temp_dir();
  auto cfg = test_cfg(dir + "/e");
  cfg.force_part_size = 16 * 1024;  // 16KB 片：60KB 语音 → 3 整片 + 12KB 尾片
  Observer ob;
  auto eng = Engine::start(cfg, ob.callback());
  ASSERT_TRUE(eng.ok());

  auto vb = eng.value()->stream_begin({});  // audio/opus, P0
  ASSERT_TRUE(vb.ok());
  std::string all;
  std::mt19937 rng(161);
  for (int i = 0; i < 10; ++i) {  // 模拟录音：10 × 6KB，每 40ms 一段
    std::string chunk(6 * 1024, 0);
    for (auto& ch : chunk) ch = char(rng() & 0xff);
    all += chunk;
    ASSERT_FALSE(eng.value()->stream_append(vb.value(), chunk.data(), chunk.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  // 边录边传的证据：松手之前已有分片落到服务端
  int64_t puts_before_finish = stat_i64(g_mock->stats(*g_http), "part_put_total");
  ASSERT_GE(puts_before_finish, 2);

  auto t0 = RealClock::instance().now_ms();
  ASSERT_FALSE(eng.value()->stream_finish(vb.value()));
  ProgressEvent ev;
  ASSERT_TRUE(ob.wait_state(vb.value(), TaskState::Ready, 8000, &ev));
  int64_t finish_ms = RealClock::instance().now_ms() - t0;
  ASSERT_LE(finish_ms, 1500);  // 松手即达（本地宽松界；mock 环境实测应在百毫秒级）
  ASSERT_TRUE(!ev.media_id.empty());

  // 服务端对象完整性：sha/size 与本地录音逐字节一致
  HttpRequest st;
  st.url = g_mock->base() + "/v1/media/" + ev.media_id;
  auto resp = http_sync(*g_http, st);
  ASSERT_EQ(resp.status, 200);
  auto j = Json::parse(resp.body);
  ASSERT_TRUE(j.ok());
  ASSERT_EQ(j.value().get_i64("size"), int64_t(all.size()));
  ASSERT_EQ(j.value().get_str("sha256"), to_hex(Sha256::digest(all.data(), all.size())));
  // 4 片各恰好一次
  auto counts = part_put_counts(g_mock->stats(*g_http));
  ASSERT_EQ(counts.size(), size_t(4));
  for (auto& kv : counts) ASSERT_EQ(kv.second, 1);
}

TEST(engine, voice_stream_cancel_discards_cleanly) {
  g_mock->reset(*g_http);
  auto dir = make_temp_dir();
  auto cfg = test_cfg(dir + "/e");
  cfg.force_part_size = 16 * 1024;
  Observer ob;
  auto eng = Engine::start(cfg, ob.callback());
  ASSERT_TRUE(eng.ok());
  auto vb = eng.value()->stream_begin({});
  ASSERT_TRUE(vb.ok());
  std::string chunk(20 * 1024, 'x');  // 超过一片 → 至少一片已在途
  ASSERT_FALSE(eng.value()->stream_append(vb.value(), chunk.data(), chunk.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ASSERT_FALSE(eng.value()->cancel(vb.value()));
  ASSERT_TRUE(ob.wait_state(vb.value(), TaskState::Canceled, 5000));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  auto q = eng.value()->query(vb.value());
  ASSERT_TRUE(q.ok());
  ASSERT_EQ(int(q.value().state), int(TaskState::Canceled));
  ASSERT_FALSE(file_exists(q.value().staging_path));  // 录音残片即弃（服务端碎片交 TTL）
}

TEST(engine, pause_resume_keeps_breakpoint) {
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("slow_ms", Json::number(60)));
  auto dir = make_temp_dir();
  std::string f = dir + "/p.bin";
  write_random_file(f, 1536 * 1024, 81);  // 12 片

  Observer ob;
  auto eng = Engine::start(test_cfg(dir + "/e"), ob.callback());
  ASSERT_TRUE(eng.ok());
  auto id = eng.value()->upload({f, "application/octet-stream", Priority::P4_File, ""});
  ASSERT_TRUE(id.ok());
  for (int i = 0; i < 300; ++i) {
    auto q = eng.value()->query(id.value());
    if (q.ok() && q.value().bytes_done >= 3 * 128 * 1024) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_FALSE(eng.value()->pause(id.value()));
  ASSERT_TRUE(ob.wait_state(id.value(), TaskState::Paused, 5000));
  int64_t puts_at_pause = stat_i64(g_mock->stats(*g_http), "part_put_total");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  // 暂停期间没有新分片落库（允许暂停瞬间在途的片完成）
  ASSERT_LE(stat_i64(g_mock->stats(*g_http), "part_put_total"), puts_at_pause + 3);

  g_mock->set_faults(*g_http, Json::object().set("slow_ms", Json::number(0)));
  ASSERT_FALSE(eng.value()->resume(id.value()));
  ASSERT_TRUE(ob.wait_state(id.value(), TaskState::Ready, 15000));
  auto counts = part_put_counts(g_mock->stats(*g_http));
  ASSERT_EQ(counts.size(), size_t(12));
  for (auto& kv : counts) ASSERT_LE(kv.second, 2);  // 在途片允许重传，其余一次
}

// ==================== soak 模式（P9 缩量长跑）====================
#ifdef __APPLE__
#include <mach/mach.h>
static int64_t current_rss_bytes() {
  mach_task_basic_info info;
  mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &cnt) !=
      KERN_SUCCESS)
    return -1;
  return int64_t(info.resident_size);
}
#else
#include <fstream>
static int64_t current_rss_bytes() {
  std::ifstream f("/proc/self/statm");
  long total = 0, rss = 0;
  f >> total >> rss;
  return rss * 4096;
}
#endif

// 多代引擎生命周期：随机上传/下载/暂停/取消 × 随机时点硬停 × recover 续跑。
// 终局判定：全部任务收敛到 Ready/Canceled（暂停的最后统一恢复）；RSS 增长有界。
static int run_soak(int seconds) {
  uint32_t seed = uint32_t(RealClock::instance().now_ms());
  std::mt19937 rng(seed);
  std::printf("soak: %ds, seed=%u\n", seconds, seed);
  g_mock->reset(*g_http);
  g_mock->set_faults(*g_http, Json::object().set("challenge_on_dedup", Json::boolean(false)));

  auto dir = make_temp_dir();
  std::string edir = dir + "/e";
  int64_t rss0 = current_rss_bytes();
  int64_t deadline = RealClock::instance().now_ms() + int64_t(seconds) * 1000;

  std::vector<std::string> all_tasks, paused;
  std::vector<std::pair<std::string, std::string>> media;  // (media_id, sha)
  std::map<std::string, std::string> upload_sha;           // task → sha（Ready 时登记 media）
  int gen = 0, ups = 0, downs = 0, pauses = 0, cancels = 0;

  while (RealClock::instance().now_ms() < deadline) {
    ++gen;
    Observer ob;
    auto eng = Engine::start(test_cfg(edir), ob.callback());
    if (!eng.ok()) return 1;
    eng.value()->recover();

    int n_up = 1 + int(rng() % 3);
    for (int i = 0; i < n_up; ++i) {
      std::string f = dir + "/s" + std::to_string(gen) + "_" + std::to_string(i) + ".bin";
      size_t sz = 64 * 1024 + (rng() % 8) * 96 * 1024;
      write_random_file(f, sz, rng());
      auto id = eng.value()->upload({f, "application/octet-stream",
                                     Priority(int(rng() % 6)), ""});
      if (id.ok()) {
        all_tasks.push_back(id.value());
        upload_sha[id.value()] = file_sha256_hex(f);
        ++ups;
      }
    }
    if (!media.empty() && (rng() % 2)) {
      auto& m = media[rng() % media.size()];
      auto id = eng.value()->download({m.first, dir + "/dl" + std::to_string(gen) + ".bin",
                                       Priority::P2_Image, m.second, ""});
      if (id.ok()) {
        all_tasks.push_back(id.value());
        ++downs;
      }
    }
    if (!all_tasks.empty() && rng() % 3 == 0) {
      auto& t = all_tasks[rng() % all_tasks.size()];
      if (rng() % 2) {
        eng.value()->pause(t);
        paused.push_back(t);
        ++pauses;
      } else {
        eng.value()->cancel(t);
        ++cancels;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200 + rng() % 700));
    // 收割本代已 Ready 的上传 → 登记 media 供后续下载
    for (auto& t : all_tasks) {
      auto q = eng.value()->query(t);
      if (q.ok() && q.value().state == TaskState::Ready && !q.value().media_id.empty() &&
          upload_sha.count(t)) {
        media.push_back({q.value().media_id, upload_sha[t]});
        upload_sha.erase(t);
      }
    }
    // 引擎析构 = 硬停
  }

  // 终局：恢复全部暂停任务，等待收敛
  {
    Observer ob;
    auto eng = Engine::start(test_cfg(edir), ob.callback());
    if (!eng.ok()) return 1;
    eng.value()->recover();
    for (auto& t : paused) eng.value()->resume(t);
    int64_t conv_deadline = RealClock::instance().now_ms() + 30000;
    size_t pending = 1;
    while (RealClock::instance().now_ms() < conv_deadline) {
      pending = 0;
      for (auto& t : all_tasks) {
        auto q = eng.value()->query(t);
        if (!q.ok()) continue;
        auto st = q.value().state;
        if (st != TaskState::Ready && st != TaskState::Canceled && st != TaskState::Failed) ++pending;
      }
      if (pending == 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    int ready = 0, canceled = 0, failed = 0;
    for (auto& t : all_tasks) {
      auto q = eng.value()->query(t);
      if (!q.ok()) continue;
      if (q.value().state == TaskState::Ready) ++ready;
      else if (q.value().state == TaskState::Canceled) ++canceled;
      else if (q.value().state == TaskState::Failed) {
        ++failed;
        std::printf("soak: FAILED task %s: %s\n", t.c_str(), q.value().last_error.c_str());
      }
    }
    int64_t rss1 = current_rss_bytes();
    std::printf("soak summary: gens=%d ups=%d downs=%d pauses=%d cancels=%d | "
                "ready=%d canceled=%d failed=%d pending=%zu | rss %+0.1fMB (%.1f→%.1f)\n",
                gen, ups, downs, pauses, cancels, ready, canceled, failed, pending,
                (rss1 - rss0) / 1e6, rss0 / 1e6, rss1 / 1e6);
    // 收敛判定：无 pending、无 failed（soak 不注入故障）；RSS 增长有界
    if (pending != 0 || failed != 0) return 1;
    if (rss1 - rss0 > 96 * 1024 * 1024) {
      std::printf("soak: RSS growth exceeds bound\n");
      return 1;
    }
  }
  std::printf("soak: PASS\n");
  return 0;
}

int main(int argc, char** argv) {
  auto t0 = RealClock::instance().now_ms();
  auto lap = [&](const char* what) {
    std::fprintf(stderr, "[e2e] %s at %lldms\n", what, (long long)(RealClock::instance().now_ms() - t0));
  };
  MockCtl mock;
  lap("mock spawned");
  HttpClient http(4);
  lap("http client up");
  if (!mock.wait_ready(http)) {
    std::printf("SKIP: python3/mockctl not available\n");
    return 0;  // 环境缺 python3 时跳过（CI 有 python3，本地兜底）
  }
  lap("mock ready");
  g_mock = &mock;
  g_http = &http;
  if (argc >= 2 && std::string(argv[1]) == "--soak") {
    return run_soak(argc >= 3 ? atoi(argv[2]) : 60);
  }
  int rc = immd_test::run_all(argc, argv);
  lap("tests done");
  return rc;
}
