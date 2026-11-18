#include "core/transport/http.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <stdexcept>

#include "core/util/clock.h"
#include "core/util/log.h"

namespace immd {

namespace {

struct Job {
  uint64_t id = 0;
  HttpRequest req;
  HttpDone done;
  CURL* easy = nullptr;
  curl_slist* hdrs = nullptr;
  int fd_up = -1;
  int64_t up_pos = 0;
  int64_t up_remaining = 0;
  int fd_down = -1;
  int64_t down_written = 0;
  std::string resp;
  char errbuf[CURL_ERROR_SIZE] = {0};
  int64_t t0 = 0;
};

size_t read_cb(char* buffer, size_t size, size_t nitems, void* ud) {
  Job* j = static_cast<Job*>(ud);
  if (j->up_remaining <= 0) return 0;
  size_t want = size * nitems;
  if (static_cast<int64_t>(want) > j->up_remaining) want = static_cast<size_t>(j->up_remaining);
  ssize_t n = pread(j->fd_up, buffer, want, j->up_pos);
  if (n <= 0) return CURL_READFUNC_ABORT;
  j->up_pos += n;
  j->up_remaining -= n;
  return static_cast<size_t>(n);
}

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
  Job* j = static_cast<Job*>(ud);
  size_t n = size * nmemb;
  if (j->fd_down >= 0) {
    ssize_t w = pwrite(j->fd_down, ptr, n, j->req.download_offset + j->down_written);
    if (w != static_cast<ssize_t>(n)) return 0;  // → CURLE_WRITE_ERROR
    j->down_written += w;
  } else {
    if (j->resp.size() + n > 16u * 1024 * 1024) return 0;  // 控制面响应上限：防协议异常撑爆内存
    j->resp.append(ptr, n);
    j->down_written += static_cast<int64_t>(n);
  }
  return n;
}

void global_curl_init() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

}  // namespace

struct HttpClient::Impl {
  explicit Impl(int streams) : max_streams(streams) {}

  int max_streams;
  CURLM* multi = nullptr;
  std::thread worker;
  std::atomic<bool> stopping{false};
  std::atomic<uint64_t> next_id{1};

  std::mutex mu;
  std::deque<Job*> incoming;   // submit() 投递（任意线程）
  std::set<uint64_t> cancels;  // cancel() 投递

  // 以下仅传输线程访问
  std::map<CURL*, Job*> active;
  std::deque<Job*> waiting;

  void finish(Job* j, Err e, const char* msg, CURLcode res) {
    HttpResponse r;
    r.elapsed_ms = RealClock::instance().now_ms() - j->t0;
    r.bytes_down = j->down_written;
    if (e == Err::Ok && res == CURLE_OK) {
      long code = 0;
      curl_easy_getinfo(j->easy, CURLINFO_RESPONSE_CODE, &code);
      r.status = code;
      r.body = std::move(j->resp);
    } else if (e != Err::Ok) {
      r.err = e;
      r.err_msg = msg ? msg : "";
    } else {
      switch (res) {
        case CURLE_OPERATION_TIMEDOUT: r.err = Err::Timeout; break;
        case CURLE_WRITE_ERROR:
        case CURLE_READ_ERROR:
        case CURLE_ABORTED_BY_CALLBACK: r.err = Err::Io; break;
        default: r.err = Err::Network; break;
      }
      r.err_msg = j->errbuf[0] ? j->errbuf : curl_easy_strerror(res);
    }
    if (j->easy) {
      curl_multi_remove_handle(multi, j->easy);
      curl_easy_cleanup(j->easy);
    }
    if (j->hdrs) curl_slist_free_all(j->hdrs);
    if (j->fd_up >= 0) close(j->fd_up);
    if (j->fd_down >= 0) close(j->fd_down);
    auto done = std::move(j->done);
    uint64_t id = j->id;
    delete j;
    if (done) done(id, std::move(r));
  }

  void start_job(Job* j) {
    j->t0 = RealClock::instance().now_ms();
    if (j->req.upload_len > 0) {
      j->fd_up = open(j->req.upload_path.c_str(), O_RDONLY);
      if (j->fd_up < 0) return finish(j, Err::Io, "open upload file failed", CURLE_OK);
      j->up_pos = j->req.upload_offset;
      j->up_remaining = j->req.upload_len;
    }
    if (!j->req.download_path.empty()) {
      j->fd_down = open(j->req.download_path.c_str(), O_WRONLY | O_CREAT, 0644);
      if (j->fd_down < 0) return finish(j, Err::Io, "open download file failed", CURLE_OK);
    }
    CURL* e = curl_easy_init();
    if (!e) return finish(j, Err::Network, "curl_easy_init failed", CURLE_OK);
    j->easy = e;
    curl_easy_setopt(e, CURLOPT_URL, j->req.url.c_str());
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, j->errbuf);
    curl_easy_setopt(e, CURLOPT_PRIVATE, j);
    curl_easy_setopt(e, CURLOPT_TIMEOUT_MS, static_cast<long>(j->req.timeout_ms));
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(j->req.connect_timeout_ms));
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, j);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);

    // 禁 100-continue：省一轮往返，且简化对端实现（契约明确）。
    j->hdrs = curl_slist_append(j->hdrs, "Expect:");
    for (auto& h : j->req.headers) j->hdrs = curl_slist_append(j->hdrs, h.c_str());
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, j->hdrs);

    if (j->req.method == "PUT" && j->req.upload_len > 0) {
      curl_easy_setopt(e, CURLOPT_UPLOAD, 1L);
      curl_easy_setopt(e, CURLOPT_READFUNCTION, read_cb);
      curl_easy_setopt(e, CURLOPT_READDATA, j);
      curl_easy_setopt(e, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(j->req.upload_len));
    } else if (j->req.method == "POST") {
      curl_easy_setopt(e, CURLOPT_POST, 1L);
      curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, static_cast<long>(j->req.body.size()));
      curl_easy_setopt(e, CURLOPT_COPYPOSTFIELDS, j->req.body.c_str());
    }
    CURLMcode mc = curl_multi_add_handle(multi, e);
    if (mc != CURLM_OK) return finish(j, Err::Network, curl_multi_strerror(mc), CURLE_OK);
    active[e] = j;
  }

  void loop() {
    while (true) {
      // 1) 收新任务与取消
      std::deque<Job*> in;
      std::set<uint64_t> cxl;
      bool stop_now;
      {
        std::lock_guard<std::mutex> lk(mu);
        in.swap(incoming);
        cxl.swap(cancels);
        stop_now = stopping.load();
      }
      for (Job* j : in) waiting.push_back(j);
      if (!cxl.empty()) {
        for (auto it = waiting.begin(); it != waiting.end();) {
          if (cxl.count((*it)->id)) {
            Job* j = *it;
            it = waiting.erase(it);
            finish(j, Err::Canceled, "canceled", CURLE_OK);
          } else {
            ++it;
          }
        }
        for (auto it = active.begin(); it != active.end();) {
          if (cxl.count(it->second->id)) {
            Job* j = it->second;
            it = active.erase(it);
            finish(j, Err::Canceled, "canceled", CURLE_OK);
          } else {
            ++it;
          }
        }
      }
      if (stop_now) break;

      // 2) 补充并发额度
      while (!waiting.empty() && static_cast<int>(active.size()) < max_streams) {
        Job* j = waiting.front();
        waiting.pop_front();
        start_job(j);  // 失败会直接 finish，不占额度
      }

      // 3) 驱动传输
      int still = 0;
      curl_multi_perform(multi, &still);
      int nq = 0;
      while (CURLMsg* m = curl_multi_info_read(multi, &nq)) {
        if (m->msg != CURLMSG_DONE) continue;
        CURL* e = m->easy_handle;
        auto it = active.find(e);
        if (it == active.end()) continue;
        Job* j = it->second;
        active.erase(it);
        finish(j, Err::Ok, nullptr, m->data.result);
      }

      // 4) 等事件（新任务经 wakeup 唤醒）
      long tmo = -1;
      curl_multi_timeout(multi, &tmo);
      if (tmo < 0 || tmo > 100) tmo = 100;
      if (tmo < 1) tmo = 1;
      curl_multi_poll(multi, nullptr, 0, static_cast<int>(tmo), nullptr);
    }
    // 停机：全部在途与排队以 Canceled 收尾
    for (auto& kv : active) finish(kv.second, Err::Canceled, "client shutdown", CURLE_OK);
    active.clear();
    for (Job* j : waiting) finish(j, Err::Canceled, "client shutdown", CURLE_OK);
    waiting.clear();
    std::deque<Job*> rest;
    {
      std::lock_guard<std::mutex> lk(mu);
      rest.swap(incoming);
    }
    for (Job* j : rest) finish(j, Err::Canceled, "client shutdown", CURLE_OK);
  }
};

HttpClient::HttpClient(int max_streams) : impl_(new Impl(max_streams)) {
  global_curl_init();
  impl_->multi = curl_multi_init();
  if (!impl_->multi) throw std::runtime_error("curl multi initialization failed");
  try {
    impl_->worker = std::thread([this] { impl_->loop(); });
  } catch (...) {
    curl_multi_cleanup(impl_->multi);
    impl_->multi = nullptr;
    throw;
  }
}

HttpClient::~HttpClient() {
  impl_->stopping.store(true);
  curl_multi_wakeup(impl_->multi);
  impl_->worker.join();
  curl_multi_cleanup(impl_->multi);
}

uint64_t HttpClient::submit(HttpRequest req, HttpDone done) {
  Job* j = new Job();
  const uint64_t id = impl_->next_id.fetch_add(1);
  j->id = id;
  j->req = std::move(req);
  j->done = std::move(done);
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->incoming.push_back(j);
  }
  // 入队后 j 的所有权已移交传输线程，此处不得再触碰（防 use-after-free）。
  curl_multi_wakeup(impl_->multi);
  return id;
}

void HttpClient::cancel(uint64_t id) {
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->cancels.insert(id);
  }
  curl_multi_wakeup(impl_->multi);
}

}  // namespace immd

