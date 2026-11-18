// mock 控制面进程管理：fork/exec python3 mockctl.py，就绪轮询，析构 SIGKILL。
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "core/transport/http.h"
#include "tests/httputil.h"
#include "tests/testutil.h"

namespace immd_test {

inline int free_port() {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  socklen_t len = sizeof(a);
  getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
  int port = ntohs(a.sin_port);
  close(s);
  return port;
}

class MockCtl {
 public:
  MockCtl() {
    root_ = make_temp_dir();
    port_ = free_port();
    const char* script = getenv("IMMD_MOCKCTL");
    script_ = script ? script : "testkit/mockctl.py";
    pid_ = fork();
    if (pid_ == 0) {
      std::string port_s = std::to_string(port_);
      execlp("python3", "python3", script_.c_str(), "--port", port_s.c_str(), "--root", root_.c_str(),
             static_cast<char*>(nullptr));
      _exit(127);
    }
  }

  ~MockCtl() {
    if (pid_ > 0) {
      kill(pid_, SIGKILL);
      int st = 0;
      waitpid(pid_, &st, 0);
    }
  }

  std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
  const std::string& root() const { return root_; }

  bool wait_ready(immd::HttpClient& c, int rounds = 100) {
    for (int i = 0; i < rounds; ++i) {
      immd::HttpRequest r;
      r.url = base() + "/admin/stats";
      r.connect_timeout_ms = 500;
      r.timeout_ms = 1000;
      auto resp = http_sync(c, r);
      if (resp.err == immd::Err::Ok && resp.status == 200) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
  }

  // 管理面调用统一带重试：前序测试取消在途请求会留下半死的 keep-alive 连接，
  // 偶发复用到坏连接 → 空响应/传输错——重试换新连接即恢复（消灭一类测试假信号）。
  void set_faults(immd::HttpClient& c, const immd::Json& j) {
    immd::HttpResponse r;
    for (int attempt = 0; attempt < 3; ++attempt) {
      r = post_json(c, base() + "/admin/faults", j);
      if (r.err == immd::Err::Ok && r.status == 200) return;
    }
    std::fprintf(stderr, "set_faults failed: err=%d status=%ld body=%s\n", int(r.err), r.status,
                 r.body.c_str());
    abort();
  }
  void reset(immd::HttpClient& c) {
    immd::HttpResponse r;
    for (int attempt = 0; attempt < 3; ++attempt) {
      r = post_json(c, base() + "/admin/reset", immd::Json::object());
      if (r.err == immd::Err::Ok && r.status == 200) return;
    }
    std::fprintf(stderr, "reset failed: err=%d status=%ld body=%s\n", int(r.err), r.status, r.body.c_str());
    abort();
  }
  immd::Json stats(immd::HttpClient& c) {
    for (int attempt = 0; attempt < 3; ++attempt) {
      immd::HttpRequest r;
      r.url = base() + "/admin/stats";
      auto resp = http_sync(c, r);
      if (resp.err != immd::Err::Ok || resp.status != 200) continue;
      auto p = immd::Json::parse(resp.body);
      if (p.ok()) return p.value();
    }
    std::fprintf(stderr, "stats failed after retries\n");
    return immd::Json::object();
  }

 private:
  std::string root_, script_;
  int port_ = 0;
  pid_t pid_ = -1;
};

}  // namespace immd_test

