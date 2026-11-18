// immd-bench（实施计划 P2.5）：热路径吞吐基线。数字入 README，劣化 >10% 视为回归。
// 端到端吞吐对本地 python mock 测量——受 mock 单线程实现约束，是"回归基线"不是"性能上限"；
// 真实上限基线（对 MinIO/千兆内网）随 P9 环境落地。
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "core/crypto/crc32c.h"
#include "core/crypto/sha256.h"
#include "core/engine/engine.h"
#include "core/journal/journal.h"
#include "core/util/id.h"
#include "core/util/json.h"
#include "tests/httputil.h"
#include "tests/mockproc.h"
#include "tests/testutil.h"

using namespace immd;
using namespace immd_test;
using Clk = std::chrono::steady_clock;

namespace {

double ms_since(Clk::time_point t0) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clk::now() - t0).count() / 1000.0;
}

void bench_hash() {
  const size_t kBuf = 8 * 1024 * 1024, kTotal = 256 * 1024 * 1024;
  std::vector<uint8_t> buf(kBuf);
  std::mt19937 rng(1);
  for (auto& b : buf) b = uint8_t(rng());
  {
    Sha256 s;
    auto t0 = Clk::now();
    for (size_t done = 0; done < kTotal; done += kBuf) s.update(buf.data(), kBuf);
    s.finish();
    double sec = ms_since(t0) / 1000.0;
    std::printf("%-34s %8.0f MB/s\n", "sha256 (streaming, sw)", kTotal / 1e6 / sec);
  }
  {
    auto t0 = Clk::now();
    uint32_t c = 0;
    for (size_t done = 0; done < kTotal; done += kBuf) c = crc32c(c, buf.data(), kBuf);
    double sec = ms_since(t0) / 1000.0;
    std::printf("%-34s %8.0f MB/s%s (guard=%08x)\n", "crc32c", kTotal / 1e6 / sec,
                crc32c_hw_available() ? " [hw]" : " [sw]", c);
  }
}

void bench_json() {
  const std::string doc =
      R"({"result":"upload","upload_id":"u_5e1","part_size":4194304,"nparts":12,)"
      R"("received_parts":[0,1,2,3,4,5,6,7]})";
  const int kN = 200000;
  auto t0 = Clk::now();
  int64_t guard = 0;
  for (int i = 0; i < kN; ++i) {
    auto p = Json::parse(doc);
    guard += p.value().get_i64("part_size");
  }
  double sec = ms_since(t0) / 1000.0;
  std::printf("%-34s %8.0f kops/s (guard=%lld)\n", "json parse (init answer)", kN / 1000.0 / sec,
              (long long)(guard & 1));
}

void bench_journal() {
  auto dir = make_temp_dir();
  auto j = Journal::open(dir + "/j.db");
  TaskRecord r;
  r.task_id = gen_id("t");
  r.kind = TaskKind::Upload;
  r.state = TaskState::Transferring;
  r.priority = Priority::P2_Image;
  r.size = 10 << 20;
  r.nparts = 12;
  r.bitmap = PartBitmap(12);
  const int kN = 5000;
  auto t0 = Clk::now();
  for (int i = 0; i < kN; ++i) {
    r.bitmap.set(uint32_t(i % 12));
    r.bytes_done = i;
    j.value()->save(r);
  }
  double sec = ms_since(t0) / 1000.0;
  std::printf("%-34s %8.0f ops/s (WAL upsert)\n", "journal save", kN / sec);
}

void bench_e2e_upload() {
  MockCtl mock;
  HttpClient http(4);
  if (!mock.wait_ready(http)) {
    std::printf("%-34s %8s (python3 unavailable)\n", "e2e upload throughput", "SKIP");
    return;
  }
  // 双口径（实施计划 P2.5）：verify=on 是回归基线（含 mock 的 python CRC 成本），
  // verify=off 更接近传输路径本身的极限（数据面无校验计算）。
  for (int pass = 0; pass < 2; ++pass) {
    const bool skip_crc = pass == 1;
    mock.reset(http);
    mock.set_faults(http, Json::object()
                              .set("challenge_on_dedup", Json::boolean(false))
                              .set("skip_crc", Json::boolean(skip_crc)));
    auto dir = make_temp_dir();
    std::string f = dir + "/pay.bin";
    const int64_t kSize = 48 * 1024 * 1024;
    write_random_file(f, size_t(kSize), 7 + uint32_t(pass));

    Config cfg;
    cfg.base_url = mock.base();
    cfg.data_dir = dir + "/e";
    cfg.force_part_size = 4 * 1024 * 1024;
    cfg.max_parts_per_task = 3;
    cfg.max_total_streams = 4;

    std::mutex mu;
    std::condition_variable cv;
    TaskState final_state = TaskState::Queued;
    auto eng = Engine::start(cfg, [&](const ProgressEvent& ev) {
      if (ev.state == TaskState::Ready || ev.state == TaskState::Failed) {
        std::lock_guard<std::mutex> lk(mu);
        final_state = ev.state;
        cv.notify_all();
      }
    });
    auto t0 = Clk::now();
    auto id = eng.value()->upload({f, "application/octet-stream", Priority::P4_File, ""});
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait_for(lk, std::chrono::seconds(120), [&] { return final_state != TaskState::Queued; });
    }
    double sec = ms_since(t0) / 1000.0;
    (void)id;
    std::printf("%-34s %8.0f MB/s (48MB, 4MB×3%s)\n",
                skip_crc ? "e2e upload (mock verify off)" : "e2e upload (mock verify on)",
                final_state == TaskState::Ready ? kSize / 1e6 / sec : 0.0,
                skip_crc ? "; 接近传输路径极限" : "; 回归基线，python CRC 是瓶颈");
  }
}

}  // namespace

int main() {
  std::printf("== immd-bench (RelWithDebInfo, single run) ==\n");
  bench_hash();
  bench_json();
  bench_journal();
  bench_e2e_upload();
  return 0;
}

