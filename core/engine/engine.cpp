#include "core/engine/engine.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <variant>
#include <vector>

#include "core/util/json.h"

#include "core/cache/cache.h"
#include "core/compute/pool.h"
#include "core/crypto/crc32c.h"
#include "core/parser/imagemeta.h"
#include "core/parser/mp4box.h"
#include "core/crypto/sha256.h"
#include "core/protocol/api.h"
#include "core/protocol/service_port.h"
#include "core/scheduler/adaptive.h"
#include "core/scheduler/fsm.h"
#include "core/scheduler/retry.h"
#include "core/transport/http.h"
#include "core/util/clock.h"
#include "core/util/fs.h"
#include "core/util/id.h"
#include "core/util/log.h"
#include "core/util/queue.h"

namespace immd {

namespace {

// ---------------- 消息（命令 + 事件）----------------
enum class Op : int { Init, PartUrls, PutPart, Complete, Challenge, DlUrl, GetPart, MediaStatus };
bool control_operation(Op op,ControlOperation& out) {
 switch(op){
 case Op::Init:out=ControlOperation::Init;return true;
 case Op::PartUrls:out=ControlOperation::PartUrls;return true;
 case Op::Complete:out=ControlOperation::Complete;return true;
 case Op::Challenge:out=ControlOperation::Challenge;return true;
 case Op::DlUrl:out=ControlOperation::DownloadUrl;return true;
 case Op::MediaStatus:out=ControlOperation::MediaStatus;return true;
 default:return false;
 }
}


struct CmdUpload { TaskRecord rec; };
struct CmdDownload { TaskRecord rec; };
struct CmdPause { std::string id; };
struct CmdResume { std::string id; };
struct CmdCancel { std::string id; };
struct CmdRecover {};
struct CmdSetRate {
  int64_t bps = 0;
};
struct CmdQuery {
  std::string id;
  std::shared_ptr<std::promise<Expected<TaskRecord>>> pr;
};
struct EvPrepared {
  std::string id;
  Error err;
  int64_t size = 0;
  std::string sha, quick;
  std::string sniffed_mime;  // 魔数嗅探结果（非空即纠偏声明 mime，架构 §14 防改后缀）
  std::vector<uint32_t> crcs;
  int64_t planned_part = 0;
};
struct EvProof {
  std::string id;
  Error err;
  std::string proof;
};
struct EvVerify {
  std::string id;
  Error err;
};
struct EvCacheHit {
  std::string id;
  Error err;        // NotFound = 未命中 → 走正常下载
  int64_t size = 0;
};
struct CmdStreamAppend {
  std::string id;
  std::vector<uint8_t> bytes;
};
struct CmdStreamFinish {
  std::string id;
};
struct EvStreamFlushed {
  std::string id;
  Error err;
  uint32_t part = 0;
  int64_t len = 0;
  uint32_t crc = 0;
};
struct EvStreamHashed {
  std::string id;
  Error err;
  std::string sha;
};
struct EvHttp {
  std::string id;
  Op op = Op::Init;
  uint32_t part = 0;
  HttpResponse resp;
};

using Msg = std::variant<CmdUpload, CmdDownload, CmdPause, CmdResume, CmdCancel, CmdRecover, CmdSetRate,
                         CmdStreamAppend, CmdStreamFinish, CmdQuery, EvPrepared, EvProof, EvVerify,
                         EvCacheHit, EvStreamFlushed, EvStreamHashed, EvHttp>;

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// ---------------- 回调线程（三内核约定：内核回调线程发出）----------------
class CallbackThread {
 public:
  CallbackThread() : th_([this] { run(); }) {}
  ~CallbackThread() {
    q_.close();
    th_.join();
  }
  bool is_current() const { return th_.get_id() == std::this_thread::get_id(); }
  void post(std::function<void()> f) { q_.push(std::move(f)); }

 private:
  void run() {
    while (true) {
      auto f = q_.pop(-1);
      if (!f.has_value()) {
        if (q_.drained()) return;
        continue;
      }
      try { (*f)(); } catch (...) { /* callback faults must not kill worker */ }
    }
  }
  BoundedQueue<std::function<void()>> q_{4096};
  std::thread th_;
};

// 任务运行时上下文（仅调度线程访问）
struct TaskCtx {
  TaskRecord rec;
  std::map<uint32_t, std::string> urls;
  std::set<uint32_t> inflight;
  std::map<uint32_t, uint64_t> http_ids;
  std::map<uint32_t, int> attempts;
  std::map<uint32_t, int64_t> retry_at;
  int ctrl_attempts = 0;
  int64_t ctrl_retry_at = 0;
  bool ctrl_inflight = false;
  bool url_fetch_inflight = false;
  uint64_t compute_job = 0;
  std::vector<uint32_t> part_crcs;  // planned_part 口径下的分片 CRC（prepare 单遍产出）
  int64_t planned_part = 0;
  std::string challenge_id, nonce;
  std::vector<std::pair<int64_t, int64_t>> ranges;
  std::string dl_url;
  bool awaiting_status = false;  // complete 已过、等 AVAILABLE（同步审两档的严档，架构 §6）
  Err last_code = Err::Ok;       // Failed 时的真实分类（Rejected/Integrity/... 不丢失）
  // 流式上传（语音）：缓冲 → 按分片粒度顺序 flush 到 staging → 边录边传
  std::vector<uint8_t> stream_buf;
  bool stream_finishing = false;
  bool stream_flush_inflight = false;
  bool stream_hash_inflight = false;
  int64_t started_ms = 0;
  int64_t last_notify_ms = 0;
};

int64_t part_len(const TaskRecord& r, uint32_t p) {
  int64_t off = int64_t(p) * r.part_size;
  return std::min<int64_t>(r.part_size, r.size - off);
}

int64_t bytes_from_bitmap(const TaskRecord& r) {
  int64_t sum = 0;
  for (uint32_t p = 0; p < r.nparts; ++p)
    if (r.bitmap.get(p)) sum += part_len(r, p);
  return sum;
}

}  // namespace

// ================= Impl =================
struct Engine::Impl {
  Config cfg;
  ProgressCallback user_cb;
  std::unique_ptr<Journal> journal;  // 仅调度线程
  std::unique_ptr<HttpClient> http;
  std::unique_ptr<ComputePool> compute;
  std::unique_ptr<MediaCache> cache;  // 可选（cfg.cache_capacity > 0）；自身线程安全
  std::unique_ptr<CallbackThread> cbt;
  BoundedQueue<Msg> q{4096};
  std::thread sched;
  std::atomic<bool> stopping{false};  // 硬停：排空期间不再派发任何新工作
  std::map<std::string, TaskCtx> tasks;
  ThroughputEwma ewma;
  Backoff backoff;
  std::mt19937 rng{0xC0FFEE};
  int64_t rate_tokens = 0;
  int64_t rate_last_ms = 0;

  int64_t now() const { return RealClock::instance().now_ms(); }

  // ---------- 通知 ----------
  void notify(const TaskCtx& c, const char* msg = "") {
    if (!user_cb) return;
    ProgressEvent ev;
    ev.task_id = c.rec.task_id;
    ev.kind = c.rec.kind;
    ev.state = c.rec.state;
    ev.bytes_done = c.rec.bytes_done;
    ev.bytes_total = c.rec.size;
    ev.media_id = c.rec.media_id;
    ev.message = msg;
    if (c.rec.state == TaskState::Failed) {
      ev.error = c.last_code;  // 真实分类：Rejected/Integrity/Timeout... 业务按此决定 UI 与重试策略
      ev.message = c.rec.last_error;
    }
    auto cb = user_cb;
    cbt->post([cb, ev] { cb(ev); });
  }

  // ---------- 状态变更（唯一入口：fsm 校验 + journal 落账 + 通知）----------
  bool to_state(TaskCtx& c, TaskState to, const char* why = "") {
    if (c.rec.state == to) return true;
    if (!fsm_can(c.rec.state, to)) {
      IMMD_ELOG("illegal transition %s -> %s (task=%s) REFUSED", state_name(c.rec.state), state_name(to),
                c.rec.task_id.c_str());
      return false;  // 非法转移拒绝并上报——它本身就是 bug 信号
    }
    c.rec.state = to;
    c.rec.updated_at = now();
    if (Error e = journal->save(c.rec)) IMMD_ELOG("journal save failed: %s", e.msg.c_str());
    notify(c, why);
    return true;
  }

  void cancel_runtime(TaskCtx& c) {
    for (auto& kv : c.http_ids) http->cancel(kv.second);
    c.http_ids.clear();
    c.inflight.clear();
    c.urls.clear();
    c.retry_at.clear();
    c.ctrl_inflight = false;
    c.url_fetch_inflight = false;
    c.ctrl_retry_at = 0;
    if (c.compute_job) {
      compute->cancel(c.compute_job);
      c.compute_job = 0;
    }
  }

  void task_fail(TaskCtx& c, Err e, const std::string& msg) {
    cancel_runtime(c);
    c.last_code = e;
    // Parser diagnostics may contain decoded wire values. Redact centrally so
    // every protocol parse failure is safe across event, log and journal sinks.
    const std::string diagnostic = cfg.control_plane && e == Err::Protocol
        ? "control protocol rejected" : msg;
    c.rec.last_error = std::string(err_name(e)) + ": " + diagnostic;
    IMMD_WLOG("task %s failed: %s", c.rec.task_id.c_str(), c.rec.last_error.c_str());
    to_state(c, TaskState::Failed, diagnostic.c_str());
    // Failed 保留断点与 staging：手动/条件恢复从账本原地续传
  }

  void finish_ready(TaskCtx& c) {
    cancel_runtime(c);
    if (c.rec.kind == TaskKind::Upload && !c.rec.staging_path.empty()) remove_file(c.rec.staging_path);
    c.rec.bytes_done = c.rec.size;
    to_state(c, TaskState::Ready);
    tasks.erase(c.rec.task_id);  // 终态出运行时；journal 保留行
  }

  // ---------- 控制面请求 ----------
  void post_http(const std::string& task_id, Op op, uint32_t part, HttpRequest req) {
    if (stopping.load(std::memory_order_relaxed)) return;  // 硬停期间不发新请求
    ControlOperation operation{};
    if(cfg.control_plane && control_operation(op,operation)) {
      ControlRequest input{req.method,req.url,req.body,req.headers};
      auto mapped=encode_control(*cfg.control_plane,operation,input);
      if(!mapped.ok()) {
        auto task=tasks.find(task_id);
        if(task!=tasks.end())task_fail(task->second,Err::Protocol,"control adapter rejected");
        return;
      }
      auto value=std::move(mapped.value());req.method=std::move(value.method);
      req.url=std::move(value.url);req.body=std::move(value.body);req.headers=std::move(value.headers);
    }
    uint64_t hid = http->submit(std::move(req), [this, task_id, op, part](uint64_t, HttpResponse&& r) {
      q.push(EvHttp{task_id, op, part, std::move(r)});  // 传输线程只投递
    });
    auto it = tasks.find(task_id);
    if (it != tasks.end() && (op == Op::PutPart || op == Op::GetPart)) it->second.http_ids[part] = hid;
  }

  HttpRequest ctrl_req(const std::string& path, std::string body) {
    HttpRequest r;
    r.method = "POST";
    r.url = cfg.base_url + path;
    r.headers.push_back("Content-Type: application/json");
    r.body = std::move(body);
    r.timeout_ms = cfg.http_timeout_ms;
    r.connect_timeout_ms = cfg.connect_timeout_ms;
    return r;
  }

  void send_init(TaskCtx& c) {
    c.ctrl_inflight = true;
    int64_t suggest = cfg.force_part_size > 0
                          ? cfg.force_part_size
                          : (c.planned_part > 0
                                 ? c.planned_part
                                 : suggest_part_size(ewma.bytes_per_sec(), cfg.target_part_seconds,
                                                     cfg.part_min, cfg.part_max, cfg.part_first));
    post_http(c.rec.task_id, Op::Init, 0, ctrl_req("/v1/media/init", build_init(c.rec, cfg, suggest)));
  }

  void send_complete(TaskCtx& c) {
    c.ctrl_inflight = true;
    std::string body = c.rec.streaming
                           ? build_complete_streaming(c.rec.upload_id, c.rec.sha256_hex, c.rec.size,
                                                      c.rec.nparts)
                           : build_complete(c.rec.upload_id, c.rec.sha256_hex);
    post_http(c.rec.task_id, Op::Complete, 0, ctrl_req("/v1/media/complete", std::move(body)));
  }

  void send_dl_url(TaskCtx& c) {
    c.ctrl_inflight = true;
    post_http(c.rec.task_id, Op::DlUrl, 0, ctrl_req("/v1/media/dl_url", build_dl(c.rec.media_id)));
  }

  void send_media_status(TaskCtx& c) {
    c.ctrl_inflight = true;
    HttpRequest r;
    r.url = cfg.base_url + "/v1/media/" + c.rec.media_id;
    r.timeout_ms = cfg.http_timeout_ms;
    r.connect_timeout_ms = cfg.connect_timeout_ms;
    post_http(c.rec.task_id, Op::MediaStatus, 0, std::move(r));
  }

  // 控制面失败的统一重试：退避后由 drive_control 按状态决定重发什么
  void ctrl_retry(TaskCtx& c, const std::string& why) {
    c.ctrl_attempts++;
    if (c.ctrl_attempts > cfg.part_retry_limit) return task_fail(c, Err::Network, "control retries exhausted: " + why);
    if (now() - c.started_ms > cfg.task_retry_budget_ms)
      return task_fail(c, Err::Timeout, "retry budget exhausted: " + why);
    c.ctrl_retry_at = now() + backoff.delay_ms(c.ctrl_attempts, rng);
  }

  void drive_control(TaskCtx& c) {
    if (c.ctrl_inflight) return;
    if (c.rec.kind == TaskKind::Upload) {
      if (c.rec.state == TaskState::Preparing) return send_init(c);
      if (c.rec.state == TaskState::Finalizing)
        return c.awaiting_status ? send_media_status(c) : send_complete(c);  // 同步审：轮询不重发 complete
      if (c.rec.state == TaskState::Transferring && c.rec.upload_id.empty()) return send_init(c);
    } else {
      if (c.rec.state == TaskState::Preparing) return send_dl_url(c);
      if (c.rec.state == TaskState::Transferring && c.dl_url.empty()) return send_dl_url(c);
    }
  }

  // ---------- 计算任务 ----------
  void dispatch_prepare(TaskCtx& c) {
    int64_t planned = cfg.force_part_size > 0
                          ? cfg.force_part_size
                          : suggest_part_size(ewma.bytes_per_sec(), cfg.target_part_seconds, cfg.part_min,
                                              cfg.part_max, cfg.part_first);
    c.planned_part = planned;
    std::string id = c.rec.task_id, src = c.rec.orig_path, dst = c.rec.staging_path;
    const int64_t remux_max = cfg.remux_max_bytes;
    auto* self = this;
    c.compute_job = compute->submit([self, id, src, dst, planned,
                                     remux_max](const std::atomic<bool>& canceled) {
      EvPrepared ev;
      ev.id = id;
      ev.planned_part = planned;

      // 对指定文件跑一遍 SHA-256 + 分片 CRC（remux 改写字节后重算指纹用）
      auto hash_pass = [&canceled, planned](const std::string& path, std::string* sha_hex,
                                            std::vector<uint32_t>* crcs, int64_t* total) -> Error {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return make_err(Err::Io, "open for hash failed");
        Sha256 sha;
        crcs->clear();
        uint32_t cur = 0;
        int64_t in_part = 0, sum = 0;
        std::vector<char> buf(256 * 1024);
        while (true) {
          if (canceled.load(std::memory_order_relaxed)) {
            close(fd);
            return make_err(Err::Canceled, "prepare canceled");
          }
          ssize_t n = read(fd, buf.data(), buf.size());
          if (n < 0) {
            close(fd);
            return make_err(Err::Io, "read for hash failed");
          }
          if (n == 0) break;
          sha.update(buf.data(), size_t(n));
          int64_t off = 0;
          while (off < n) {
            int64_t take = std::min<int64_t>(planned - in_part, n - off);
            cur = crc32c(cur, buf.data() + off, size_t(take));
            in_part += take;
            off += take;
            if (in_part == planned) {
              crcs->push_back(cur);
              cur = 0;
              in_part = 0;
            }
          }
          sum += n;
        }
        close(fd);
        if (in_part > 0) crcs->push_back(cur);
        *sha_hex = to_hex(sha.finish());
        *total = sum;
        return {};
      };

      int in = open(src.c_str(), O_RDONLY);
      if (in < 0) {
        ev.err = make_err(Err::Io, "open source failed: " + src);
        self->q.push(std::move(ev));
        return;
      }
      int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (out < 0) {
        close(in);
        ev.err = make_err(Err::Io, "open staging failed: " + dst);
        self->q.push(std::move(ev));
        return;
      }
      // pass A：快照拷贝 + 单遍 SHA-256/分片 CRC（架构 §7），顺路捕获头部 64KB 供嗅探
      Sha256 sha;
      std::vector<uint32_t> crcs;
      std::vector<uint8_t> head;
      head.reserve(64 * 1024);
      uint32_t cur_crc = 0;
      int64_t in_part = 0, total = 0;
      std::vector<char> buf(256 * 1024);
      bool ok = true;
      while (true) {
        if (canceled.load(std::memory_order_relaxed)) {  // 协作取消检查点
          ok = false;
          break;
        }
        ssize_t n = read(in, buf.data(), buf.size());
        if (n < 0) {
          ev.err = make_err(Err::Io, "read source failed");
          ok = false;
          break;
        }
        if (n == 0) break;
        if (write(out, buf.data(), size_t(n)) != n) {
          ev.err = make_err(Err::Io, "write staging failed");
          ok = false;
          break;
        }
        if (head.size() < 64 * 1024) {
          size_t take = std::min<size_t>(size_t(n), 64 * 1024 - head.size());
          head.insert(head.end(), buf.data(), buf.data() + take);
        }
        sha.update(buf.data(), size_t(n));
        // 分片 CRC：跨 buffer 累积，按 planned 切段
        int64_t off = 0;
        while (off < n) {
          int64_t room = planned - in_part;
          int64_t take = std::min<int64_t>(room, n - off);
          cur_crc = crc32c(cur_crc, buf.data() + off, size_t(take));
          in_part += take;
          off += take;
          if (in_part == planned) {
            crcs.push_back(cur_crc);
            cur_crc = 0;
            in_part = 0;
          }
        }
        total += n;
      }
      if (ok && in_part > 0) crcs.push_back(cur_crc);
      close(in);
      close(out);
      if (!ok) {
        if (!ev.err) ev.err = make_err(Err::Canceled, "prepare canceled");
        remove_file(dst);
        self->q.push(std::move(ev));
        return;
      }
      ev.size = total;
      ev.sha = to_hex(sha.finish());
      ev.crcs = std::move(crcs);

      // ---- 安全门与变换：parser 接线（架构 §12/§13/§14 的承诺装车处）----
      std::string sniffed = sniff_mime(head.data(), head.size());
      if (sniffed != "application/octet-stream") ev.sniffed_mime = sniffed;  // 防改后缀：以魔数为准
      if (sniffed.rfind("image/", 0) == 0 && sniffed != "image/svg+xml") {
        auto ih = parse_image_head(head.data(), head.size());
        if (ih.ok() && ih.value().bomb) {  // 解码炸弹：解码前拒绝（1KB 文件不许换 GB 内存）
          remove_file(dst);
          ev.err = make_err(Err::Rejected, "decode bomb rejected before upload");
          self->q.push(std::move(ev));
          return;
        }
        // 头窗解析失败不拒绝：预检尽力而为，服务端流水线兜底（架构 §6）
      }
      if (sniffed == "video/mp4" && remux_max > 0 && total > 0 && total <= remux_max) {
        // 尾置 moov → 就地 faststart（点开即播，架构 §13）；失败不致命，交服务端 remux 兜底
        std::vector<uint8_t> whole(static_cast<size_t>(total));
        int fd = open(dst.c_str(), O_RDONLY);
        bool loaded = fd >= 0;
        if (loaded) {
          int64_t got = 0;
          while (got < total) {
            if (canceled.load(std::memory_order_relaxed)) {
              loaded = false;
              break;
            }
            ssize_t n = pread(fd, whole.data() + got, size_t(total - got), got);
            if (n <= 0) {
              loaded = false;
              break;
            }
            got += n;
          }
          close(fd);
        }
        if (loaded) {
          auto info = mp4_inspect(whole.data(), whole.size());
          if (info.ok() && info.value().has_moov && info.value().has_mdat && !info.value().faststart) {
            std::vector<uint8_t> fixed;
            if (!mp4_faststart(whole, &fixed)) {
              int wf = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
              bool wrote = wf >= 0 && write(wf, fixed.data(), fixed.size()) == ssize_t(fixed.size());
              if (wf >= 0) close(wf);
              if (wrote) {  // 字节已变：指纹重算（仅此路径二遍 IO，常规路径保持单遍）
                Error e = hash_pass(dst, &ev.sha, &ev.crcs, &ev.size);
                if (e) {
                  remove_file(dst);
                  ev.err = e;
                  self->q.push(std::move(ev));
                  return;
                }
                IMMD_ILOG("faststart remux applied in prepare (task=%s)", id.c_str());
              }
            } else {
              IMMD_WLOG("faststart remux skipped (task=%s): server-side fallback", id.c_str());
            }
          }
        }
      }
      // quick_hash = sha256(首/中/尾 256KB || size)：两段式探测的低成本指纹（架构 §16）。
      // 注意用 ev.size（remux 可能已改写字节与长度），不是拷贝时的 total。
      {
        Sha256 qh;
        int fd = open(dst.c_str(), O_RDONLY);
        if (fd >= 0) {
          const int64_t k = 256 * 1024;
          int64_t sz = ev.size;
          int64_t offs[3] = {0, std::max<int64_t>(0, sz / 2 - k / 2), std::max<int64_t>(0, sz - k)};
          std::vector<char> qb(static_cast<size_t>(k));
          for (int64_t o : offs) {
            ssize_t n = pread(fd, qb.data(), size_t(std::min<int64_t>(k, sz - o)), o);
            if (n > 0) qh.update(qb.data(), size_t(n));
          }
          close(fd);
        }
        qh.update(&ev.size, sizeof(ev.size));
        ev.quick = to_hex(qh.finish());
      }
      self->q.push(std::move(ev));
    });
  }

  void dispatch_proof(TaskCtx& c) {
    std::string id = c.rec.task_id, path = c.rec.staging_path.empty() ? c.rec.orig_path : c.rec.staging_path;
    std::string nonce = c.nonce;
    auto ranges = c.ranges;
    auto* self = this;
    c.compute_job = compute->submit([self, id, path, nonce, ranges](const std::atomic<bool>& canceled) {
      EvProof ev;
      ev.id = id;
      if (canceled.load()) {
        ev.err = make_err(Err::Canceled, "canceled");
        self->q.push(std::move(ev));
        return;
      }
      int fd = open(path.c_str(), O_RDONLY);
      if (fd < 0) {
        ev.err = make_err(Err::Io, "open for proof failed");
        self->q.push(std::move(ev));
        return;
      }
      std::string blob;
      for (auto& r : ranges) {
        std::string piece(size_t(r.second), 0);
        ssize_t n = pread(fd, piece.data(), size_t(r.second), r.first);
        if (n < 0) {
          ev.err = make_err(Err::Io, "pread for proof failed");
          close(fd);
          self->q.push(std::move(ev));
          return;
        }
        piece.resize(size_t(n));
        blob += piece;
      }
      close(fd);
      ev.proof = to_hex(hmac_sha256(nonce.data(), nonce.size(), blob.data(), blob.size()));
      self->q.push(std::move(ev));
    });
  }

  void dispatch_verify(TaskCtx& c) {
    std::string id = c.rec.task_id, path = c.rec.staging_path;  // 下载 tmp
    std::string want = c.rec.sha256_hex;
    auto* self = this;
    c.compute_job = compute->submit([self, id, path, want](const std::atomic<bool>& canceled) {
      EvVerify ev;
      ev.id = id;
      Sha256 sha;
      int fd = open(path.c_str(), O_RDONLY);
      if (fd < 0) {
        ev.err = make_err(Err::Io, "open for verify failed");
        self->q.push(std::move(ev));
        return;
      }
      std::vector<char> buf(256 * 1024);
      while (true) {
        if (canceled.load(std::memory_order_relaxed)) {
          ev.err = make_err(Err::Canceled, "canceled");
          close(fd);
          self->q.push(std::move(ev));
          return;
        }
        ssize_t n = read(fd, buf.data(), buf.size());
        if (n < 0) {
          ev.err = make_err(Err::Io, "read for verify failed");
          break;
        }
        if (n == 0) break;
        sha.update(buf.data(), size_t(n));
      }
      close(fd);
      if (!ev.err && !want.empty() && to_hex(sha.finish()) != want)
        ev.err = make_err(Err::Integrity, "sha256 mismatch after download");
      self->q.push(std::move(ev));
    });
  }

  // ---------- 分片派发 ----------
  bool take_rate(int64_t bytes) {
    if (cfg.rate_limit_bps <= 0) return true;
    int64_t t = now();
    rate_tokens = std::min<int64_t>(cfg.rate_limit_bps,
                                    rate_tokens + (t - rate_last_ms) * cfg.rate_limit_bps / 1000);
    rate_last_ms = t;
    if (rate_tokens >= bytes) {
      rate_tokens -= bytes;
      return true;
    }
    return false;
  }

  int total_streams() const {
    int n = 0;
    for (auto& kv : tasks) n += int(kv.second.inflight.size());
    return n;
  }

  void dispatch_put(TaskCtx& c, uint32_t p) {
    int64_t off = int64_t(p) * c.rec.part_size;
    int64_t len = part_len(c.rec, p);
    HttpRequest r;
    r.method = "PUT";
    r.url = c.urls[p];
    r.upload_path = c.rec.staging_path;
    r.upload_offset = off;
    r.upload_len = len;
    r.timeout_ms = cfg.http_timeout_ms;
    r.connect_timeout_ms = cfg.connect_timeout_ms;
    if (!c.part_crcs.empty() && c.planned_part == c.rec.part_size && p < c.part_crcs.size()) {
      char hex[16];
      snprintf(hex, sizeof(hex), "%08x", c.part_crcs[p]);
      r.headers.push_back(std::string("x-part-crc32c: ") + hex);
    }
    c.inflight.insert(p);
    post_http(c.rec.task_id, Op::PutPart, p, std::move(r));
  }

  void dispatch_get(TaskCtx& c, uint32_t p) {
    int64_t off = int64_t(p) * c.rec.part_size;
    int64_t len = part_len(c.rec, p);
    HttpRequest r;
    r.url = c.dl_url;
    r.download_path = c.rec.staging_path;  // dest.part
    r.download_offset = off;
    r.headers.push_back("Range: bytes=" + std::to_string(off) + "-" + std::to_string(off + len - 1));
    r.timeout_ms = cfg.http_timeout_ms;
    r.connect_timeout_ms = cfg.connect_timeout_ms;
    c.inflight.insert(p);
    post_http(c.rec.task_id, Op::GetPart, p, std::move(r));
  }

  std::vector<TaskCtx*> tasks_by_priority() {
    std::vector<TaskCtx*> v;
    for (auto& kv : tasks) v.push_back(&kv.second);
    std::sort(v.begin(), v.end(), [](const TaskCtx* a, const TaskCtx* b) {
      if (a->rec.priority != b->rec.priority) return int(a->rec.priority) < int(b->rec.priority);
      return a->rec.created_at < b->rec.created_at;
    });
    return v;
  }

  void pump() {
    int64_t t = now();
    auto ordered = tasks_by_priority();

    // 1) 控制面退避到期 → 重驱动
    for (auto* c : ordered) {
      if (!c->ctrl_inflight && c->ctrl_retry_at > 0 && c->ctrl_retry_at <= t) {
        c->ctrl_retry_at = 0;
        drive_control(*c);
      }
    }

    // 2) 任务级预算：启动排队任务
    int started = 0;
    for (auto* c : ordered)
      if (c->rec.state == TaskState::Preparing || c->rec.state == TaskState::Transferring ||
          c->rec.state == TaskState::Finalizing)
        ++started;
    for (auto* c : ordered) {
      if (started >= cfg.max_active_tasks) break;
      if (c->rec.state != TaskState::Queued) continue;
      start_task(*c);
      ++started;
    }

    // 3) 分片派发（优先级序：高优任务先吃预算 = 分片边界抢占，架构 §8）
    int streams = total_streams();
    for (auto* cp : ordered) {
      TaskCtx& c = *cp;
      if (c.rec.state != TaskState::Transferring) continue;
      // 对账优先于传输：init/complete 在途时本任务不派发分片——
      // 否则 recover 后会按过期位图重传服务端已有的分片（真实 bug，收敛测试抓获）。
      if (c.ctrl_inflight) continue;
      // 上传需要 URL：批量领取
      if (c.rec.kind == TaskKind::Upload) {
        std::vector<uint32_t> need_url;
        for (uint32_t p = 0; p < c.rec.nparts && need_url.size() < 32; ++p)
          if (!c.rec.bitmap.get(p) && !c.urls.count(p) && !c.inflight.count(p)) need_url.push_back(p);
        if (!need_url.empty() && !c.url_fetch_inflight && c.ctrl_retry_at == 0) {
          c.url_fetch_inflight = true;
          post_http(c.rec.task_id, Op::PartUrls, 0,
                    ctrl_req("/v1/media/part_urls", build_part_urls(c.rec.upload_id, need_url)));
        }
      }
      if (c.rec.state != TaskState::Transferring) continue; // encode failure may terminate this pump turn
      for (uint32_t p = 0; p < c.rec.nparts; ++p) {
        if (int(c.inflight.size()) >= cfg.max_parts_per_task) break;
        if (streams >= cfg.max_total_streams) return;
        if (c.rec.bitmap.get(p) || c.inflight.count(p)) continue;
        auto ra = c.retry_at.find(p);
        if (ra != c.retry_at.end() && ra->second > t) continue;
        if (c.rec.kind == TaskKind::Upload && !c.urls.count(p)) continue;  // 等 URL
        if (c.rec.kind == TaskKind::Download && c.dl_url.empty()) break;   // 等凭证
        if (!take_rate(part_len(c.rec, p))) return;  // 限速：本轮到此为止
        if (c.rec.kind == TaskKind::Upload) dispatch_put(c, p);
        else dispatch_get(c, p);
        ++streams;
      }
    }
  }

  void start_task(TaskCtx& c) {
    c.started_ms = now();
    if (c.rec.kind == TaskKind::Upload) {
      if (c.rec.streaming) {  // 流式：无 hash/size 直接开单，字节随 append 到达
        if (!to_state(c, TaskState::Preparing)) return;
        send_init(c);
        return;
      }
      if (c.rec.sha256_hex.empty()) {
        if (!to_state(c, TaskState::Preparing)) return;
        dispatch_prepare(c);
      } else {  // 恢复/重试：指纹已知，直接对账
        if (!to_state(c, TaskState::Preparing)) return;
        send_init(c);
      }
    } else {
      if (!to_state(c, TaskState::Preparing)) return;
      // 缓存优先（内容寻址：sha 即身份）：命中零网络，未命中回落正常下载
      if (cache && !c.rec.sha256_hex.empty()) {
        dispatch_cache_probe(c);
        return;
      }
      send_dl_url(c);
    }
  }

  void dispatch_cache_probe(TaskCtx& c) {
    std::string id = c.rec.task_id, sha = c.rec.sha256_hex, dest = c.rec.dest_path;
    MediaCache* mc = cache.get();
    auto* self = this;
    c.compute_job = compute->submit([self, mc, id, sha, dest](const std::atomic<bool>& canceled) {
      EvCacheHit ev;
      ev.id = id;
      auto hit = mc->lookup(sha);
      if (!hit.ok()) {
        ev.err = hit.error();
        self->q.push(std::move(ev));
        return;
      }
      // 缓存 → 目标路径流式拷贝（可取消）
      int in = open(hit.value().c_str(), O_RDONLY);
      int out = in >= 0 ? open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
      if (in < 0 || out < 0) {
        if (in >= 0) close(in);
        ev.err = make_err(Err::Io, "cache copy open failed");
        self->q.push(std::move(ev));
        return;
      }
      std::vector<char> buf(256 * 1024);
      int64_t total = 0;
      while (true) {
        if (canceled.load(std::memory_order_relaxed)) {
          ev.err = make_err(Err::Canceled, "canceled");
          break;
        }
        ssize_t n = read(in, buf.data(), buf.size());
        if (n < 0) {
          ev.err = make_err(Err::Io, "cache read failed");
          break;
        }
        if (n == 0) break;
        if (write(out, buf.data(), size_t(n)) != n) {
          ev.err = make_err(Err::Io, "cache copy write failed");
          break;
        }
        total += n;
      }
      close(in);
      close(out);
      if (ev.err) remove_file(dest);
      ev.size = total;
      self->q.push(std::move(ev));
    });
  }

  void on_cache_hit(EvCacheHit& ev) {
    auto it = tasks.find(ev.id);
    if (it == tasks.end()) return;
    TaskCtx& c = it->second;
    c.compute_job = 0;
    if (c.rec.state != TaskState::Preparing) return;
    if (ev.err) {           // 未命中/拷贝失败：回落正常下载（缓存只是优化，不是依赖）
      send_dl_url(c);
      return;
    }
    c.rec.size = ev.size;
    c.rec.bytes_done = ev.size;
    finish_ready(c);        // 零网络完成
  }

  // 下载校验通过后异步回填缓存（fire-and-forget：失败只损失下次命中，不影响本次任务）
  void dispatch_cache_store(const std::string& sha, const std::string& src) {
    if (!cache || sha.empty()) return;
    MediaCache* mc = cache.get();
    compute->submit([mc, sha, src](const std::atomic<bool>& canceled) {
      if (canceled.load()) return;
      std::string tmp = src + ".cachetmp";
      std::ifstream in(src, std::ios::binary);
      std::ofstream out(tmp, std::ios::binary);
      if (!in || !out) return;
      out << in.rdbuf();
      out.close();
      if (mc->put(sha, tmp)) remove_file(tmp);  // put 失败清理临时文件
    });
  }

  // ---------- 流式上传（语音）----------
  void on_stream_append(CmdStreamAppend& v) {
    auto it = tasks.find(v.id);
    if (it == tasks.end()) return;
    TaskCtx& c = it->second;
    if (!c.rec.streaming || c.stream_finishing) return;  // finish 后的迟到 append 丢弃
    if (c.rec.state == TaskState::Failed) return;
    if (c.stream_buf.size() + v.bytes.size() > 64u * 1024 * 1024)
      return task_fail(c, Err::State, "stream buffer overflow");
    c.stream_buf.insert(c.stream_buf.end(), v.bytes.begin(), v.bytes.end());
    maybe_stream_flush(c);
  }

  void on_stream_finish(const std::string& id) {
    auto it = tasks.find(id);
    if (it == tasks.end()) return;
    TaskCtx& c = it->second;
    if (!c.rec.streaming || c.stream_finishing) return;
    c.stream_finishing = true;
    maybe_stream_flush(c);
    maybe_stream_finalize(c);
  }

  // 缓冲达到分片粒度（或 finish 后有尾片）→ 顺序 flush 到 staging（计算池写盘 + CRC）
  void maybe_stream_flush(TaskCtx& c) {
    if (c.rec.state != TaskState::Transferring) return;  // init 未回：先缓冲
    if (c.stream_flush_inflight) return;                 // 单飞：保证分片号与偏移顺序
    size_t ps = size_t(c.rec.part_size);
    if (ps == 0) return;
    bool full = c.stream_buf.size() >= ps;
    bool tail = c.stream_finishing && !c.stream_buf.empty();
    if (!full && !tail) return;
    size_t n = full ? ps : c.stream_buf.size();
    auto chunk = std::make_shared<std::vector<uint8_t>>(c.stream_buf.begin(),
                                                        c.stream_buf.begin() + long(n));
    c.stream_buf.erase(c.stream_buf.begin(), c.stream_buf.begin() + long(n));
    uint32_t part = c.rec.nparts;
    int64_t off = int64_t(part) * c.rec.part_size;
    c.stream_flush_inflight = true;
    std::string id = c.rec.task_id, path = c.rec.staging_path;
    auto* self = this;
    c.compute_job = compute->submit([self, id, path, chunk, part, off](const std::atomic<bool>& canceled) {
      EvStreamFlushed ev;
      ev.id = id;
      ev.part = part;
      ev.len = int64_t(chunk->size());
      if (canceled.load()) {
        ev.err = make_err(Err::Canceled, "canceled");
        self->q.push(std::move(ev));
        return;
      }
      int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0600);
      if (fd < 0) {
        ev.err = make_err(Err::Io, "open staging failed");
      } else {
        if (pwrite(fd, chunk->data(), chunk->size(), off) != ssize_t(chunk->size()))
          ev.err = make_err(Err::Io, "stream flush write failed");
        close(fd);
      }
      ev.crc = crc32c(0, chunk->data(), chunk->size());
      self->q.push(std::move(ev));
    });
  }

  void on_stream_flushed(EvStreamFlushed& ev) {
    auto it = tasks.find(ev.id);
    if (it == tasks.end()) return;
    TaskCtx& c = it->second;
    c.stream_flush_inflight = false;
    c.compute_job = 0;
    if (c.rec.state != TaskState::Transferring) return;
    if (ev.err) return task_fail(c, ev.err.code, ev.err.msg);
    // 账本随分片生长：位图扩容保留已置位（from_blob 拷贝旧字节）
    c.rec.nparts = ev.part + 1;
    c.rec.bitmap = PartBitmap::from_blob(c.rec.bitmap.blob(), c.rec.nparts);
    c.part_crcs.push_back(ev.crc);  // flush 严格顺序 → 索引即分片号
    c.rec.size += ev.len;
    journal->save(c.rec);
    maybe_stream_flush(c);     // 缓冲里可能还有整片
    maybe_stream_finalize(c);  // finish 已到且这是尾片时推进
    // 新分片的 PUT 由 loop 的 pump 派发
  }

  // finish 后的收敛判定：缓冲清空 + 尾片落盘 + 全部 PUT 成功 + hash 就绪 → complete
  void maybe_stream_finalize(TaskCtx& c) {
    if (!c.stream_finishing || c.rec.state != TaskState::Transferring) return;
    if (!c.stream_buf.empty() || c.stream_flush_inflight) return;
    if (c.rec.nparts == 0) return task_fail(c, Err::State, "empty stream");
    if (c.rec.sha256_hex.empty()) {
      if (c.stream_hash_inflight) return;
      c.stream_hash_inflight = true;
      std::string id = c.rec.task_id, path = c.rec.staging_path;
      auto* self = this;
      c.compute_job = compute->submit([self, id, path](const std::atomic<bool>& canceled) {
        EvStreamHashed ev;
        ev.id = id;
        Sha256 sha;
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
          ev.err = make_err(Err::Io, "open staging for hash failed");
          self->q.push(std::move(ev));
          return;
        }
        std::vector<char> buf(256 * 1024);
        while (true) {
          if (canceled.load(std::memory_order_relaxed)) {
            ev.err = make_err(Err::Canceled, "canceled");
            break;
          }
          ssize_t n = read(fd, buf.data(), buf.size());
          if (n < 0) {
            ev.err = make_err(Err::Io, "read staging for hash failed");
            break;
          }
          if (n == 0) break;
          sha.update(buf.data(), size_t(n));
        }
        close(fd);
        if (!ev.err) ev.sha = to_hex(sha.finish());
        self->q.push(std::move(ev));
      });
      return;
    }
    if (c.rec.bitmap.complete()) {
      if (to_state(c, TaskState::Finalizing)) send_complete(c);
    }
  }

  void on_stream_hashed(EvStreamHashed& ev) {
    auto it = tasks.find(ev.id);
    if (it == tasks.end()) return;
    TaskCtx& c = it->second;
    c.stream_hash_inflight = false;
    c.compute_job = 0;
    if (c.rec.state != TaskState::Transferring) return;
    if (ev.err) return task_fail(c, ev.err.code, ev.err.msg);
    c.rec.sha256_hex = ev.sha;
    journal->save(c.rec);
    maybe_stream_finalize(c);
  }

  // ---------- 事件处理 ----------
  void on_prepared(EvPrepared& ev) {
    auto it = tasks.find(ev.id);
    if (it == tasks.end()) return;
    TaskCtx& c = it->second;
    c.compute_job = 0;
    if (c.rec.state != TaskState::Preparing) return;  // 已暂停/取消
    if (ev.err) return task_fail(c, ev.err.code, ev.err.msg);
    c.rec.size = ev.size;
    c.rec.sha256_hex = ev.sha;
    c.rec.quick_hash_hex = ev.quick;
    if (!ev.sniffed_mime.empty() && ev.sniffed_mime != c.rec.mime) {
      IMMD_ILOG("mime corrected by magic sniff: %s -> %s (task=%s)", c.rec.mime.c_str(),
                ev.sniffed_mime.c_str(), c.rec.task_id.c_str());
      c.rec.mime = ev.sniffed_mime;  // 以魔数为准（架构 §14：不信后缀与声明）
    }
    c.part_crcs = std::move(ev.crcs);
    c.planned_part = ev.planned_part;
    journal->save(c.rec);
    send_init(c);
  }

  void on_proof(EvProof& ev) {
    auto it = tasks.find(ev.id);
    if (it == tasks.end()) return;
    TaskCtx& c = it->second;
    c.compute_job = 0;
    if (c.rec.state != TaskState::Preparing) return;
    if (ev.err) return task_fail(c, ev.err.code, ev.err.msg);
    c.ctrl_inflight = true;
    post_http(c.rec.task_id, Op::Challenge, 0,
              ctrl_req("/v1/media/challenge", build_challenge(c.challenge_id, ev.proof)));
  }

  void on_verify(EvVerify& ev) {
    auto it = tasks.find(ev.id);
    if (it == tasks.end()) return;
    TaskCtx& c = it->second;
    c.compute_job = 0;
    if (c.rec.state != TaskState::Finalizing) return;
    if (ev.err) {
      remove_file(c.rec.staging_path);
      c.rec.bitmap = PartBitmap(c.rec.nparts);  // 校验失败重下
      c.rec.bytes_done = 0;
      return task_fail(c, ev.err.code, ev.err.msg);
    }
    if (Error e = rename_file(c.rec.staging_path, c.rec.dest_path))
      return task_fail(c, Err::Io, "rename to dest failed");
    c.rec.staging_path.clear();
    dispatch_cache_store(c.rec.sha256_hex, c.rec.dest_path);  // 回填：下次同内容零网络
    finish_ready(c);
  }

  void on_http(EvHttp& ev) {
    auto it = tasks.find(ev.id);
    if (it == tasks.end()) return;  // 任务已终态（取消晚到的事件）
    TaskCtx& c = it->second;
    // 暂停/失败态下晚到的事件：只清理在途标记，不推进任何状态（恢复时 init 幂等对账兜底）
    if (c.rec.state == TaskState::Paused || c.rec.state == TaskState::Failed) {
      if (ev.op == Op::PartUrls) c.url_fetch_inflight = false;
      else if (ev.op == Op::PutPart || ev.op == Op::GetPart) {
        c.inflight.erase(ev.part);
        c.http_ids.erase(ev.part);
      } else {
        c.ctrl_inflight = false;
      }
      return;
    }
    ControlOperation operation{};
    if(cfg.control_plane && ev.resp.err==Err::Ok && control_operation(ev.op,operation)) {
      auto mapped=decode_control(*cfg.control_plane,operation,{ev.resp.status,ev.resp.body});
      if(!mapped.ok())return task_fail(c,Err::Protocol,"control adapter rejected");
      ev.resp.status=mapped.value().status;ev.resp.body=std::move(mapped.value().body);
    }
    switch (ev.op) {
      case Op::Init: return on_init(c, ev.resp);
      case Op::PartUrls: return on_part_urls(c, ev.resp);
      case Op::PutPart: return on_put_part(c, ev.part, ev.resp);
      case Op::Complete: return on_complete(c, ev.resp);
      case Op::Challenge: return on_challenge(c, ev.resp);
      case Op::DlUrl: return on_dl_url(c, ev.resp);
      case Op::GetPart: return on_get_part(c, ev.part, ev.resp);
      case Op::MediaStatus: return on_media_status(c, ev.resp);
    }
  }

  bool ctrl_transport_ok(TaskCtx& c, const HttpResponse& r, const char* what) {
    c.ctrl_inflight = false;
    if (r.err == Err::Canceled) return false;
    if (r.err != Err::Ok || r.status >= 500 || r.status == 0) {
      ctrl_retry(c, std::string(what) + " " + (r.err != Err::Ok ? r.err_msg : std::to_string(r.status)));
      return false;
    }
    return true;
  }

  void on_init(TaskCtx& c, HttpResponse& r) {
    if (!ctrl_transport_ok(c, r, "init")) return;
    if (r.status != 200) return task_fail(c, Err::HttpClient, "init http " + std::to_string(r.status));
    auto a = parse_init(r.body);
    if (!a.ok()) return task_fail(c, Err::Protocol, a.error().msg);
    auto& ans = a.value();
    c.ctrl_attempts = 0;
    switch (ans.kind) {
      case InitAnswer::Kind::Instant:
        c.rec.media_id = ans.media_id;
        return finish_ready(c);  // 秒传：零字节完成
      case InitAnswer::Kind::Challenge:
        c.challenge_id = ans.challenge_id;
        c.nonce = ans.nonce;
        c.ranges = ans.ranges;
        return dispatch_proof(c);
      case InitAnswer::Kind::Upload: {
        c.rec.upload_id = ans.upload_id;
        c.rec.part_size = ans.part_size;
        c.rec.nparts = ans.nparts;
        if (ans.part_size != c.planned_part) c.part_crcs.clear();  // CRC 口径失配则不发头
        // 服务端为真相源：received_parts 直接重建位图（架构 §10 对账）
        PartBitmap bm(ans.nparts);
        for (auto p : ans.received) bm.set(p);
        c.rec.bitmap = bm;
        c.rec.bytes_done = bytes_from_bitmap(c.rec);
        c.urls.clear();
        if (c.rec.state == TaskState::Preparing) {
          if (!to_state(c, TaskState::Transferring)) return;
        } else {
          journal->save(c.rec);
          notify(c, "reconciled");
        }
        if (c.rec.streaming) {   // 流式：CRC 口径 = 服务端分片大小；开闸缓冲的 flush
          c.planned_part = c.rec.part_size;
          maybe_stream_flush(c);
          maybe_stream_finalize(c);  // begin 后立即 finish 的空流场景
          return;
        }
        if (c.rec.bitmap.complete()) {
          if (to_state(c, TaskState::Finalizing)) send_complete(c);
        }
        return;
      }
    }
  }

  void on_challenge(TaskCtx& c, HttpResponse& r) {
    if (!ctrl_transport_ok(c, r, "challenge")) return;
    if (r.status == 404) {  // 挑战过期：重新 init（服务端会再发新挑战）
      ctrl_retry(c, "challenge expired");
      return;
    }
    if (r.status == 403) return task_fail(c, Err::Integrity, "challenge proof rejected");
    if (r.status != 200) return task_fail(c, Err::HttpClient, "challenge http " + std::to_string(r.status));
    auto a = parse_init(r.body);
    if (!a.ok() || a.value().kind != InitAnswer::Kind::Instant)
      return task_fail(c, Err::Protocol, "bad challenge answer");
    c.rec.media_id = a.value().media_id;
    finish_ready(c);
  }

  void on_part_urls(TaskCtx& c, HttpResponse& r) {
    c.url_fetch_inflight = false;
    if (r.err == Err::Canceled) return;
    if (r.err != Err::Ok || r.status != 200) {
      ctrl_retry(c, "part_urls failed");
      return;
    }
    auto m = parse_part_urls(r.body);
    if (!m.ok()) return task_fail(c, Err::Protocol, m.error().msg);
    for (auto& kv : m.value()) c.urls[kv.first] = kv.second;
    c.ctrl_attempts = 0;
  }

  void on_put_part(TaskCtx& c, uint32_t p, HttpResponse& r) {
    c.inflight.erase(p);
    c.http_ids.erase(p);
    if (c.rec.state != TaskState::Transferring) return;  // 暂停/取消晚到的事件
    int64_t len = part_len(c.rec, p);
    if (r.err == Err::Ok && r.status == 200) {
      c.rec.bitmap.set(p);
      c.rec.bytes_done = bytes_from_bitmap(c.rec);
      c.attempts.erase(p);
      c.retry_at.erase(p);
      ewma.on_sample(len, std::max<int64_t>(r.elapsed_ms, 1));
      journal->save(c.rec);
      maybe_notify_progress(c);
      if (c.rec.streaming) {
        maybe_stream_finalize(c);  // 流式：nparts 还在生长，是否收敛由 finalize 统一判定
      } else if (c.rec.bitmap.complete()) {
        if (to_state(c, TaskState::Finalizing)) send_complete(c);
      }
      return;
    }
    if (r.err == Err::Canceled) return;
    if (r.status == 403) {  // 签名过期：全部 URL 作废，按固定节奏换新
      c.urls.clear();
      credential_pace(c, p);
      return;
    }
    if (r.err != Err::Ok) ewma.reset();  // 网络级失败：吞吐估计作废（切网语义近似）
    part_retry(c, p, r.err != Err::Ok ? r.err_msg : ("http " + std::to_string(r.status)));
  }

  // 凭证换新不是数据路径失败：不计入分片重试次数（否则毫秒级烧光预算），
  // 以固定节奏循环，由任务级时间预算兜住"服务端永远发过期 URL"的病态场景。
  void credential_pace(TaskCtx& c, uint32_t p) {
    if (now() - c.started_ms > cfg.task_retry_budget_ms)
      return task_fail(c, Err::Timeout, "credential refresh budget exhausted");
    c.retry_at[p] = now() + 150;
  }

  void part_retry(TaskCtx& c, uint32_t p, const std::string& why) {
    int n = ++c.attempts[p];
    if (n > cfg.part_retry_limit)
      return task_fail(c, Err::Network, "part " + std::to_string(p) + " retries exhausted: " + why);
    if (now() - c.started_ms > cfg.task_retry_budget_ms)
      return task_fail(c, Err::Timeout, "retry budget exhausted");
    c.retry_at[p] = now() + backoff.delay_ms(n, rng);
  }

  void on_complete(TaskCtx& c, HttpResponse& r) {
    if (!ctrl_transport_ok(c, r, "complete")) return;
    if (r.status == 200) {
      auto a = parse_complete_ok(r.body);
      if (!a.ok()) return task_fail(c, Err::Protocol, a.error().msg);
      c.rec.media_id = a.value().media_id;
      c.ctrl_attempts = 0;
      if (cfg.ready_on_uploaded) return finish_ready(c);
      c.awaiting_status = true;  // 严档：等流水线 AVAILABLE 才 READY（架构 §6 合规窗口）
      return send_media_status(c);
    }
    if (r.status == 409) {  // 服务端缺片：以服务端为准回补（架构 §5 幂等语义）
      auto a = parse_complete_missing(r.body);
      if (!a.ok()) return task_fail(c, Err::Protocol, a.error().msg);
      for (auto p : a.value().missing) c.rec.bitmap.clear(p);
      c.rec.bytes_done = bytes_from_bitmap(c.rec);
      if (!to_state(c, TaskState::Transferring)) return;
      return;
    }
    if (r.status == 400) return task_fail(c, Err::Integrity, "complete sha mismatch (source mutated?)");
    task_fail(c, Err::HttpClient, "complete http " + std::to_string(r.status));
  }

  void on_media_status(TaskCtx& c, HttpResponse& r) {
    if (!ctrl_transport_ok(c, r, "media_status")) return;
    if (r.status != 200) return task_fail(c, Err::HttpClient, "status http " + std::to_string(r.status));
    auto p = Json::parse(r.body);
    if (!p.ok()) return task_fail(c, Err::Protocol, "bad status body");
    std::string st = p.value().get_str("status");
    if (st == "AVAILABLE") return finish_ready(c);
    if (st == "REJECTED") return task_fail(c, Err::Rejected, "rejected by policy");
    c.ctrl_retry_at = now() + 300;  // PROCESSING：轮询（架构 §6 推送 + 轮询兜底的兜底半边）
  }

  void on_dl_url(TaskCtx& c, HttpResponse& r) {
    if (!ctrl_transport_ok(c, r, "dl_url")) return;
    if (r.status == 409) {  // PROCESSING：等可用
      c.ctrl_retry_at = now() + 300;
      return;
    }
    if (r.status == 403) return task_fail(c, Err::Rejected, "media rejected");
    if (r.status == 404) return task_fail(c, Err::NotFound, "media not found");
    if (r.status != 200) return task_fail(c, Err::HttpClient, "dl_url http " + std::to_string(r.status));
    auto a = parse_dl(r.body);
    if (!a.ok()) return task_fail(c, Err::Protocol, a.error().msg);
    c.dl_url = a.value().url;
    c.ctrl_attempts = 0;
    if (c.rec.state == TaskState::Preparing) {  // 首次：建位图与临时文件
      c.rec.size = a.value().size;
      if (c.rec.sha256_hex.empty()) c.rec.sha256_hex = a.value().sha256_hex;
      int64_t part = cfg.force_part_size > 0
                         ? cfg.force_part_size
                         : suggest_part_size(ewma.bytes_per_sec(), cfg.target_part_seconds, cfg.part_min,
                                             cfg.part_max, cfg.part_first);
      c.rec.part_size = std::min<int64_t>(part, std::max<int64_t>(c.rec.size, 1));
      c.rec.nparts = uint32_t((c.rec.size + c.rec.part_size - 1) / c.rec.part_size);
      if (c.rec.nparts == 0) c.rec.nparts = 1;
      c.rec.bitmap = PartBitmap(c.rec.nparts);
      c.rec.staging_path = c.rec.dest_path + ".part";
      if (!to_state(c, TaskState::Transferring)) return;
    } else {
      journal->save(c.rec);  // 刷新凭证（403 换新路径）
    }
  }

  void on_get_part(TaskCtx& c, uint32_t p, HttpResponse& r) {
    c.inflight.erase(p);
    c.http_ids.erase(p);
    if (c.rec.state != TaskState::Transferring) return;
    int64_t len = part_len(c.rec, p);
    if (r.err == Err::Ok && (r.status == 206 || r.status == 200) && r.bytes_down == len) {
      c.rec.bitmap.set(p);
      c.rec.bytes_done = bytes_from_bitmap(c.rec);
      c.attempts.erase(p);
      c.retry_at.erase(p);
      ewma.on_sample(len, std::max<int64_t>(r.elapsed_ms, 1));
      journal->save(c.rec);
      maybe_notify_progress(c);
      if (c.rec.bitmap.complete()) {
        if (to_state(c, TaskState::Finalizing)) {
          if (c.rec.sha256_hex.empty()) {  // 无指纹可校：直接落位
            EvVerify ok;
            ok.id = c.rec.task_id;
            on_verify(ok);
          } else {
            dispatch_verify(c);
          }
        }
      }
      return;
    }
    if (r.err == Err::Canceled) return;
    if (r.status == 403) {  // 下载凭证过期：换新
      c.dl_url.clear();
      credential_pace(c, p);
      return;
    }
    if (r.err != Err::Ok) ewma.reset();
    part_retry(c, p, r.err != Err::Ok ? r.err_msg : ("http " + std::to_string(r.status)));
  }

  void maybe_notify_progress(TaskCtx& c) {
    int64_t t = now();
    if (t - c.last_notify_ms < 100) return;  // 进度节流（架构 §9 铁律）
    c.last_notify_ms = t;
    notify(c);
  }

  // ---------- 命令处理 ----------
  void on_enqueue(TaskRecord rec) {
    rec.created_at = now();
    rec.updated_at = rec.created_at;
    if (Error e = journal->save(rec)) {
      IMMD_ELOG("enqueue journal save failed: %s", e.msg.c_str());
      // The request was already accepted by the command queue. Surface an
      // explicit terminal failure instead of leaving the caller waiting forever.
      TaskCtx failed;
      failed.rec = std::move(rec);
      failed.rec.state = TaskState::Failed;
      failed.rec.last_error = "journal enqueue failed";
      failed.last_code = e.code;
      notify(failed);
      return;
    }
    TaskCtx ctx;
    ctx.rec = std::move(rec);
    auto& c = tasks[ctx.rec.task_id];
    c = std::move(ctx);
    notify(c, "queued");
  }

  void on_pause(const std::string& id) {
    auto it = tasks.find(id);
    if (it == tasks.end()) return;
    TaskCtx& c = it->second;
    if (!fsm_can(c.rec.state, TaskState::Paused)) return;
    cancel_runtime(c);
    to_state(c, TaskState::Paused);
  }

  void on_resume(const std::string& id) {
    auto it = tasks.find(id);
    if (it == tasks.end()) return;
    TaskCtx& c = it->second;
    if (!fsm_resumable(c.rec.state)) return;
    c.attempts.clear();
    c.retry_at.clear();
    c.ctrl_attempts = 0;
    c.ctrl_retry_at = 0;
    to_state(c, TaskState::Queued);
  }

  void on_cancel(const std::string& id) {
    auto it = tasks.find(id);
    if (it == tasks.end()) return;
    TaskCtx& c = it->second;
    if (!fsm_can(c.rec.state, TaskState::Canceled)) return;
    cancel_runtime(c);
    if (c.rec.kind == TaskKind::Upload && !c.rec.staging_path.empty()) remove_file(c.rec.staging_path);
    if (c.rec.kind == TaskKind::Download && !c.rec.staging_path.empty()) remove_file(c.rec.staging_path);
    to_state(c, TaskState::Canceled);
    tasks.erase(id);  // journal 保留 Canceled 行供审计
  }

  void on_recover() {
    auto rows = journal->load_unfinished();
    if (!rows.ok()) {
      IMMD_ELOG("recover load failed: %s", rows.error().msg.c_str());
      return;
    }
    for (auto& rec : rows.value()) {
      if (tasks.count(rec.task_id)) continue;
      TaskCtx c;
      c.rec = rec;
      c.started_ms = now();
      auto& ctx = tasks[rec.task_id];
      ctx = std::move(c);
      TaskCtx& t = ctx;
      // 流式任务不跨进程恢复：录音源已随进程消亡，缓冲不落账——诚实标记失败（架构 §14）
      if (t.rec.streaming && !fsm_terminal(t.rec.state) && t.rec.state != TaskState::Failed) {
        t.rec.state = TaskState::Failed;
        t.last_code = Err::Canceled;
        t.rec.last_error = "stream interrupted by restart";
        journal->save(t.rec);
        notify(t, "recovered");
        continue;
      }
      switch (t.rec.state) {
        case TaskState::Queued:
        case TaskState::Paused:
        case TaskState::Failed:
          break;  // 等 pump / 用户动作
        case TaskState::Preparing:
          // 现场重建：指纹未完成 → 回 Queued 重来；已有指纹 → 直接 init 对账
          if (t.rec.kind == TaskKind::Upload && t.rec.sha256_hex.empty()) {
            t.rec.state = TaskState::Queued;  // 恢复期重建，不走 fsm（现场即事实）
            journal->save(t.rec);
          } else {
            drive_control(t);
          }
          break;
        case TaskState::Transferring:
          if (t.rec.kind == TaskKind::Upload) {
            t.rec.upload_id.empty() ? (void)(t.rec.state = TaskState::Queued, journal->save(t.rec))
                                    : send_init(t);  // init 幂等对账：服务端 received_parts 为准
          } else {
            send_dl_url(t);  // 凭证过期，换新后按位图续传
          }
          break;
        case TaskState::Finalizing:
          if (t.rec.kind == TaskKind::Upload) send_complete(t);  // complete 幂等
          else dispatch_verify(t);
          break;
        default:
          break;
      }
      notify(t, "recovered");
    }
  }

  void on_query(CmdQuery& cq) {
    auto it = tasks.find(cq.id);
    if (it != tasks.end()) {
      cq.pr->set_value(it->second.rec);
      return;
    }
    cq.pr->set_value(journal->load(cq.id));
  }

  // ---------- 主循环 ----------
  int64_t next_deadline() {
    int64_t d = std::numeric_limits<int64_t>::max();
    for (auto& kv : tasks) {
      auto& c = kv.second;
      if (c.ctrl_retry_at > 0) d = std::min(d, c.ctrl_retry_at);
      for (auto& ra : c.retry_at) d = std::min(d, ra.second);
    }
    return d;
  }

  void loop() {
    while (true) {
      int64_t timeout = 200;
      int64_t dl = next_deadline();
      if (dl != std::numeric_limits<int64_t>::max())
        timeout = std::max<int64_t>(1, std::min<int64_t>(dl - now(), 200));
      auto m = q.pop(timeout);
      if (!m.has_value()) {
        if (q.drained()) break;  // close() 后排空即退出
      } else {
        std::visit(overloaded{
                       [&](CmdUpload& v) { on_enqueue(std::move(v.rec)); },
                       [&](CmdDownload& v) { on_enqueue(std::move(v.rec)); },
                       [&](CmdPause& v) { on_pause(v.id); },
                       [&](CmdResume& v) { on_resume(v.id); },
                       [&](CmdCancel& v) { on_cancel(v.id); },
                       [&](CmdRecover&) { on_recover(); },
                       [&](CmdSetRate& v) {
                         cfg.rate_limit_bps = v.bps;
                         rate_tokens = v.bps;  // 换档即重置桶（让路要立即生效）
                         rate_last_ms = now();
                       },
                       [&](CmdQuery& v) { on_query(v); },
                       [&](CmdStreamAppend& v) { on_stream_append(v); },
                       [&](CmdStreamFinish& v) { on_stream_finish(v.id); },
                       [&](EvPrepared& v) { on_prepared(v); },
                       [&](EvProof& v) { on_proof(v); },
                       [&](EvVerify& v) { on_verify(v); },
                       [&](EvCacheHit& v) { on_cache_hit(v); },
                       [&](EvStreamFlushed& v) { on_stream_flushed(v); },
                       [&](EvStreamHashed& v) { on_stream_hashed(v); },
                       [&](EvHttp& v) { on_http(v); },
                   },
                   *m);
      }
      if (!stopping.load(std::memory_order_relaxed)) pump();
    }
  }
};

// ================= 公共 API =================
Expected<std::unique_ptr<Engine>> Engine::start(Config cfg, ProgressCallback cb) try {
  if (cfg.base_url.empty() || cfg.data_dir.empty())
    return make_err(Err::State, "base_url/data_dir required");
  if (Error e = mkdirs(cfg.data_dir + "/staging")) return e;
  auto j = Journal::open(cfg.data_dir + "/journal.db");
  if (!j.ok()) return j.error();

  auto eng = std::unique_ptr<Engine>(new Engine());
  eng->impl_ = std::make_unique<Impl>();
  auto& im = *eng->impl_;
  im.cfg = cfg;
  im.user_cb = std::move(cb);
  im.journal = std::move(j.value());
  im.backoff = Backoff{cfg.retry_base_ms, cfg.retry_cap_ms};
  im.cbt = std::make_unique<CallbackThread>();
  im.compute = std::make_unique<ComputePool>(int(std::min(4u, std::max(1u, std::thread::hardware_concurrency() - 1))));
  im.http = std::make_unique<HttpClient>(cfg.max_total_streams);
  if (cfg.cache_capacity > 0) {
    auto mc = MediaCache::open(cfg.data_dir + "/cache", cfg.cache_capacity);
    if (mc.ok()) im.cache = std::move(mc.value());
    else IMMD_WLOG("cache disabled: %s", mc.error().msg.c_str());  // 缓存是优化不是依赖
  }
  im.rate_last_ms = im.now();
  im.rate_tokens = cfg.rate_limit_bps;
  im.sched = std::thread([p = eng->impl_.get()] { p->loop(); });
  return eng;
} catch (const std::exception&) {
  return make_err(Err::Busy, "engine initialization failed");
}

Engine::~Engine() {
  if (!impl_) return;
  impl_->stopping.store(true);  // 硬停：排空期间不再派发新工作（贴近 kill -9 语义）
  impl_->q.close();             // 拒新 + 排空后调度线程退出
  if (impl_->sched.joinable()) impl_->sched.join();
  impl_->http.reset();       // 取消在途；回调向已关闭队列投递 → 安全丢弃
  impl_->compute.reset();
  impl_->cbt.reset();        // 排空回调
}

Expected<std::string> Engine::upload(UploadRequest r) {
  if (!file_exists(r.file_path)) return make_err(Err::Io, "file not found: " + r.file_path);
  TaskRecord rec;
  rec.task_id = r.task_id.empty() ? gen_id("t") : r.task_id;
  rec.kind = TaskKind::Upload;
  rec.state = TaskState::Queued;
  rec.priority = r.priority;
  rec.orig_path = r.file_path;
  rec.staging_path = impl_->cfg.data_dir + "/staging/" + rec.task_id + ".bin";
  rec.mime = r.mime;
  std::string id = rec.task_id;
  if (!impl_->q.try_push(CmdUpload{std::move(rec)})) return make_err(Err::Busy, "engine queue full");
  return id;
}

Expected<std::string> Engine::download(DownloadRequest r) {
  if (r.media_id.empty() || r.dest_path.empty()) return make_err(Err::State, "media_id/dest required");
  TaskRecord rec;
  rec.task_id = r.task_id.empty() ? gen_id("d") : r.task_id;
  rec.kind = TaskKind::Download;
  rec.state = TaskState::Queued;
  rec.priority = r.priority;
  rec.media_id = r.media_id;
  rec.dest_path = r.dest_path;
  rec.sha256_hex = r.sha256_hex;
  std::string id = rec.task_id;
  if (!impl_->q.try_push(CmdDownload{std::move(rec)})) return make_err(Err::Busy, "engine queue full");
  return id;
}

Error Engine::pause(const std::string& id) {
  return impl_->q.try_push(CmdPause{id}) ? Error{} : make_err(Err::Busy, "queue full");
}
Error Engine::resume(const std::string& id) {
  return impl_->q.try_push(CmdResume{id}) ? Error{} : make_err(Err::Busy, "queue full");
}
Error Engine::cancel(const std::string& id) {
  return impl_->q.try_push(CmdCancel{id}) ? Error{} : make_err(Err::Busy, "queue full");
}
Error Engine::recover() {
  return impl_->q.try_push(CmdRecover{}) ? Error{} : make_err(Err::Busy, "queue full");
}
Error Engine::set_rate_limit(int64_t bytes_per_sec) {
  return impl_->q.try_push(CmdSetRate{bytes_per_sec}) ? Error{} : make_err(Err::Busy, "queue full");
}

Expected<std::string> Engine::stream_begin(StreamBeginRequest r) {
  TaskRecord rec;
  rec.task_id = gen_id("v");
  rec.kind = TaskKind::Upload;
  rec.streaming = true;
  rec.state = TaskState::Queued;
  rec.priority = r.priority;
  rec.mime = r.mime;
  rec.staging_path = impl_->cfg.data_dir + "/staging/" + rec.task_id + ".bin";
  std::string id = rec.task_id;
  if (!impl_->q.try_push(CmdUpload{std::move(rec)})) return make_err(Err::Busy, "engine queue full");
  return id;
}

Error Engine::stream_append(const std::string& task_id, const void* data, size_t len) {
  if (len == 0) return {};
  if (len > 8u * 1024 * 1024) return make_err(Err::State, "append chunk too large");
  CmdStreamAppend cmd;
  cmd.id = task_id;
  cmd.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
  return impl_->q.try_push(std::move(cmd)) ? Error{} : make_err(Err::Busy, "queue full");
}

Error Engine::stream_finish(const std::string& task_id) {
  return impl_->q.try_push(CmdStreamFinish{task_id}) ? Error{} : make_err(Err::Busy, "queue full");
}

Expected<TaskRecord> Engine::query(const std::string& id) {
  if (impl_->cbt && impl_->cbt->is_current())
    return make_err(Err::State, "synchronous query is forbidden on callback thread");
  CmdQuery cq;
  cq.id = id;
  cq.pr = std::make_shared<std::promise<Expected<TaskRecord>>>();
  auto fut = cq.pr->get_future();
  if (!impl_->q.try_push(std::move(cq))) return make_err(Err::Busy, "queue full");
  return fut.get();
}

}  // namespace immd
