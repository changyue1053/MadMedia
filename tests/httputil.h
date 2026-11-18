// 测试用同步 HTTP 包装（生产代码全部走异步事件，不用此工具）。
#pragma once

#include <future>
#include <string>

#include "core/transport/http.h"
#include "core/util/json.h"

namespace immd_test {

inline immd::HttpResponse http_sync(immd::HttpClient& c, immd::HttpRequest req) {
  auto pr = std::make_shared<std::promise<immd::HttpResponse>>();
  auto fut = pr->get_future();
  c.submit(std::move(req), [pr](uint64_t, immd::HttpResponse&& r) { pr->set_value(std::move(r)); });
  return fut.get();
}

inline immd::HttpResponse post_json(immd::HttpClient& c, const std::string& url, const immd::Json& j) {
  immd::HttpRequest r;
  r.method = "POST";
  r.url = url;
  r.headers.push_back("Content-Type: application/json");
  r.body = j.dump();
  return http_sync(c, r);
}

}  // namespace immd_test

