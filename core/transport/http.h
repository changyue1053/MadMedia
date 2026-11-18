// 传输执行器（架构 §9）：单传输线程 + curl multi 事件驱动。
// 铁律：done 回调在传输线程触发，只准投递事件（引擎入队），不准做业务。
// 实现说明：采用 curl_multi_poll + curl_multi_wakeup（而非 socket-action 自建 reactor）——
// 单线程事件驱动语义等价、代码量减半；需要更细粒度定时/限速时按架构 §9 预留升级为自有 reactor。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/util/error.h"

namespace immd {

struct HttpRequest {
  std::string method = "GET";  // GET / PUT / POST
  std::string url;
  std::vector<std::string> headers;  // "K: V"
  std::string body;                  // POST 控制面小 JSON

  // 上传：从文件区间流式读（pread，零整块拷贝）——upload_len > 0 时启用（PUT）
  std::string upload_path;
  int64_t upload_offset = 0;
  int64_t upload_len = 0;

  // 下载：写入文件指定偏移（pwrite）——download_path 非空时启用
  std::string download_path;
  int64_t download_offset = 0;

  int64_t timeout_ms = 30000;
  int64_t connect_timeout_ms = 5000;
};

struct HttpResponse {
  Err err = Err::Ok;  // 仅传输层错误；HTTP 层语义由 status 表达，调用方分类
  std::string err_msg;
  long status = 0;
  std::string body;  // 控制面响应体（文件下载模式为空）
  int64_t bytes_down = 0;
  int64_t elapsed_ms = 0;
};

using HttpDone = std::function<void(uint64_t id, HttpResponse&&)>;

class HttpClient {
 public:
  explicit HttpClient(int max_streams = 4);
  ~HttpClient();  // 停止并取消全部在途（done 以 Canceled 回调）

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // 线程安全；返回请求 id。析构后不得再调用。
  uint64_t submit(HttpRequest req, HttpDone done);
  // 尽力取消：已完成的忽略；命中则以 Canceled 回调。
  void cancel(uint64_t id);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace immd

