// immd-cli：排障手术刀（实施计划 P4.5 / M1 演示载体）。
//   immd-cli up   <file> --base <url> --data <dir> [--prio 0..5] [--part <bytes>]
//   immd-cli down <media_id> <dest> --base <url> --data <dir>
//   immd-cli recover --base <url> --data <dir>          # 恢复账本内未完成任务并等待收敛
//   immd-cli journal --data <dir>                       # 账本现场 dump
//   immd-cli mp4 <file> [--remux <out>]                 # moov 检视 / faststart 重排
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fstream>

#include "core/engine/engine.h"
#include "core/journal/journal.h"
#include "core/parser/imagemeta.h"
#include "core/parser/mp4box.h"
#include "core/pipeline/image/image_pipeline.h"
#include "core/scheduler/fsm.h"

using namespace immd;

namespace {

struct Args {
  std::string base, data, remux_out;
  int prio = 4;
  int64_t part = 0;
  std::vector<std::string> pos;
};

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 2; i < argc; ++i) {
    std::string s = argv[i];
    if (s == "--base" && i + 1 < argc) a.base = argv[++i];
    else if (s == "--data" && i + 1 < argc) a.data = argv[++i];
    else if (s == "--prio" && i + 1 < argc) a.prio = atoi(argv[++i]);
    else if (s == "--part" && i + 1 < argc) a.part = atoll(argv[++i]);
    else if (s == "--remux" && i + 1 < argc) a.remux_out = argv[++i];
    else a.pos.push_back(s);
  }
  return a;
}

struct Waiter {
  std::mutex mu;
  std::condition_variable cv;
  std::map<std::string, ProgressEvent> last;
  ProgressCallback cb() {
    return [this](const ProgressEvent& ev) {
      {
        std::lock_guard<std::mutex> lk(mu);
        last[ev.task_id] = ev;
        double pct = ev.bytes_total > 0 ? 100.0 * double(ev.bytes_done) / double(ev.bytes_total) : 0;
        std::fprintf(stderr, "\r[%s] %-12s %6.1f%% (%lld/%lld)", ev.task_id.c_str(), state_name(ev.state),
                     pct, (long long)ev.bytes_done, (long long)ev.bytes_total);
        if (fsm_terminal(ev.state) || ev.state == TaskState::Failed) std::fprintf(stderr, "\n");
      }
      cv.notify_all();
    };
  }
  ProgressEvent wait_terminal(const std::string& id) {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&] {
      auto it = last.find(id);
      return it != last.end() &&
             (fsm_terminal(it->second.state) || it->second.state == TaskState::Failed);
    });
    return last[id];
  }
};

Config make_cfg(const Args& a) {
  Config cfg;
  cfg.base_url = a.base;
  cfg.data_dir = a.data;
  if (a.part > 0) cfg.force_part_size = a.part;
  return cfg;
}

int cmd_up(const Args& a) {
  if (a.pos.empty() || a.base.empty() || a.data.empty()) {
    std::fprintf(stderr, "usage: immd-cli up <file> --base <url> --data <dir>\n");
    return 2;
  }
  Waiter w;
  auto eng = Engine::start(make_cfg(a), w.cb());
  if (!eng.ok()) {
    std::fprintf(stderr, "engine start failed: %s\n", eng.error().msg.c_str());
    return 1;
  }
  UploadRequest r;
  r.file_path = a.pos[0];
  r.priority = Priority(a.prio);
  auto id = eng.value()->upload(r);
  if (!id.ok()) {
    std::fprintf(stderr, "enqueue failed: %s\n", id.error().msg.c_str());
    return 1;
  }
  auto ev = w.wait_terminal(id.value());
  if (ev.state == TaskState::Ready) {
    std::printf("media_id=%s\n", ev.media_id.c_str());
    return 0;
  }
  std::fprintf(stderr, "upload failed: %s\n", ev.message.c_str());
  return 1;
}

int cmd_down(const Args& a) {
  if (a.pos.size() < 2 || a.base.empty() || a.data.empty()) {
    std::fprintf(stderr, "usage: immd-cli down <media_id> <dest> --base <url> --data <dir>\n");
    return 2;
  }
  Waiter w;
  auto eng = Engine::start(make_cfg(a), w.cb());
  if (!eng.ok()) return 1;
  DownloadRequest r;
  r.media_id = a.pos[0];
  r.dest_path = a.pos[1];
  r.priority = Priority(a.prio);
  auto id = eng.value()->download(r);
  if (!id.ok()) return 1;
  auto ev = w.wait_terminal(id.value());
  if (ev.state == TaskState::Ready) {
    std::printf("saved to %s\n", a.pos[1].c_str());
    return 0;
  }
  std::fprintf(stderr, "download failed: %s\n", ev.message.c_str());
  return 1;
}

int cmd_recover(const Args& a) {
  if (a.base.empty() || a.data.empty()) return 2;
  Waiter w;
  auto eng = Engine::start(make_cfg(a), w.cb());
  if (!eng.ok()) return 1;
  eng.value()->recover();
  // 简化：等 3 秒观察进度（生产由业务驱动生命周期）
  std::this_thread::sleep_for(std::chrono::seconds(3));
  std::fprintf(stderr, "\n");
  return 0;
}

int cmd_journal(const Args& a) {
  if (a.data.empty()) return 2;
  auto j = Journal::open(a.data + "/journal.db");
  if (!j.ok()) {
    std::fprintf(stderr, "open journal failed: %s\n", j.error().msg.c_str());
    return 1;
  }
  auto rows = j.value()->load_all();
  if (!rows.ok()) return 1;
  std::printf("%-36s %-8s %-13s %-4s %12s/%-12s %-6s %s\n", "task_id", "kind", "state", "prio",
              "done", "total", "parts", "error");
  for (auto& r : rows.value()) {
    std::printf("%-36s %-8s %-13s P%-3d %12lld/%-12lld %3u/%-3u %s\n", r.task_id.c_str(),
                kind_name(r.kind), state_name(r.state), int(r.priority), (long long)r.bytes_done,
                (long long)r.size, r.bitmap.count(), r.nparts, r.last_error.c_str());
  }
  return 0;
}

int cmd_mp4(const Args& a) {
  if (a.pos.empty()) {
    std::fprintf(stderr, "usage: immd-cli mp4 <file> [--remux <out>]\n");
    return 2;
  }
  std::ifstream in(a.pos[0], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "open failed: %s\n", a.pos[0].c_str());
    return 1;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::printf("sniffed mime : %s\n", sniff_mime(bytes.data(), bytes.size()).c_str());
  auto info = mp4_inspect(bytes.data(), bytes.size());
  if (!info.ok()) {
    std::fprintf(stderr, "mp4 inspect failed: %s\n", info.error().msg.c_str());
    return 1;
  }
  auto& i = info.value();
  std::printf("brand        : %s\n", i.major_brand.c_str());
  std::printf("duration     : %lld ms\n", (long long)i.duration_ms);
  std::printf("dimensions   : %ux%u\n", i.width, i.height);
  std::printf("moov offset  : %lld (size %lld)   mdat offset: %lld\n", (long long)i.moov_off,
              (long long)i.moov_size, (long long)i.mdat_off);
  std::printf("faststart    : %s\n", i.faststart ? "YES（点开即播）" : "NO（尾置 moov，需 remux）");
  if (!a.remux_out.empty()) {
    std::vector<uint8_t> out;
    if (Error e = mp4_faststart(bytes, &out)) {
      std::fprintf(stderr, "remux failed: %s\n", e.msg.c_str());
      return 1;
    }
    std::ofstream of(a.remux_out, std::ios::binary);
    of.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    std::printf("remuxed      : %s (%zu bytes%s)\n", a.remux_out.c_str(), out.size(),
                out.size() != bytes.size() ? ", stco→co64 已升级" : "");
  }
  return 0;
}

int cmd_img(const Args& a) {
  if (a.pos.empty()) {
    std::fprintf(stderr, "usage: immd-cli img <file> [--out <prefix>]\n");
    return 2;
  }
  std::ifstream in(a.pos[0], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "open failed: %s\n", a.pos[0].c_str());
    return 1;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::printf("sniffed mime : %s\n", sniff_mime(bytes.data(), bytes.size()).c_str());
  auto head = parse_image_head(bytes.data(), bytes.size());
  if (head.ok()) {
    std::printf("dimensions   : %ux%u  orientation=%d  gps=%s  bomb=%s\n", head.value().width,
                head.value().height, head.value().orientation, head.value().has_gps ? "yes" : "no",
                head.value().bomb ? "YES(拒绝)" : "no");
  }
  auto v = make_variants(bytes.data(), bytes.size());
  if (!v.ok()) {
    std::fprintf(stderr, "make_variants failed: %s\n", v.error().msg.c_str());
    return 1;
  }
  std::printf("thumb        : %zu bytes\n", v.value().thumb_jpeg.size());
  std::printf("preview      : %zu bytes\n", v.value().preview_jpeg.size());
  std::printf("blurhash     : %s\n", v.value().blurhash.c_str());
  if (!a.remux_out.empty()) {
    auto dump = [](const std::string& p, const std::vector<uint8_t>& b) {
      std::ofstream f(p, std::ios::binary);
      f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    };
    dump(a.remux_out + ".thumb.jpg", v.value().thumb_jpeg);
    dump(a.remux_out + ".preview.jpg", v.value().preview_jpeg);
    std::printf("written      : %s.thumb.jpg / .preview.jpg\n", a.remux_out.c_str());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "immd-cli <up|down|recover|journal> ...\n");
    return 2;
  }
  Args a = parse(argc, argv);
  std::string cmd = argv[1];
  if (cmd == "up") return cmd_up(a);
  if (cmd == "down") return cmd_down(a);
  if (cmd == "recover") return cmd_recover(a);
  if (cmd == "journal") return cmd_journal(a);
  if (cmd == "mp4") return cmd_mp4(a);
  if (cmd == "img") return cmd_img(a);
  std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
  return 2;
}

