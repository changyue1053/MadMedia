// 错误分类与 Expected：错误码跨 FFI 稳定，msg 仅用于日志（已脱敏口径）。
#pragma once

#include <string>
#include <utility>

namespace immd {

enum class Err : int {
  Ok = 0,
  Canceled = 1,     // 用户/上层主动取消
  Timeout = 2,      // 传输超时
  Network = 3,      // DNS/连接/TLS/中断
  HttpClient = 4,   // 4xx（不可盲重试；401/403 走凭证换新路径）
  HttpServer = 5,   // 5xx（可重试）
  Protocol = 6,     // 响应不符合契约（JSON 畸形/字段缺失）
  Io = 7,           // 本地文件读写失败
  Integrity = 8,    // 校验不一致（CRC/SHA）
  Busy = 9,         // 队列满 / 预算耗尽（快速失败）
  State = 10,       // 非法状态转移 / 句柄已关闭
  NotFound = 11,    // 任务或对象不存在
  Expired = 12,     // 凭证/URL 过期（自动换新，一般不出内核）
  Rejected = 13,    // 服务端安全策略拦截（终态，不重试）
};

const char* err_name(Err e);

struct Error {
  Err code = Err::Ok;
  std::string msg;
  explicit operator bool() const { return code != Err::Ok; }
};

inline Error make_err(Err c, std::string m) { return Error{c, std::move(m)}; }

// 轻量 Expected：要求 T 可默认构造（内核 DTO 均满足）。
template <typename T>
class Expected {
 public:
  Expected(T v) : ok_(true), val_(std::move(v)) {}          // NOLINT(implicit)
  Expected(Error e) : ok_(false), err_(std::move(e)) {}     // NOLINT(implicit)
  bool ok() const { return ok_; }
  T& value() { return val_; }
  const T& value() const { return val_; }
  const Error& error() const { return err_; }

 private:
  bool ok_;
  T val_{};
  Error err_{};
};

}  // namespace immd

