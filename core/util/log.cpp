#include "core/util/log.h"

#include <atomic>
#include <cstdio>
#include <ctime>

#include "core/util/error.h"

namespace immd {

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::Warn)};
const char* level_tag(LogLevel lv) {
  switch (lv) {
    case LogLevel::Debug: return "D";
    case LogLevel::Info: return "I";
    case LogLevel::Warn: return "W";
    case LogLevel::Error: return "E";
    default: return "?";
  }
}
}  // namespace

void set_log_level(LogLevel lv) { g_level.store(static_cast<int>(lv)); }
LogLevel log_level() { return static_cast<LogLevel>(g_level.load()); }

void log_at(LogLevel lv, const char* fmt, ...) {
  if (static_cast<int>(lv) < g_level.load(std::memory_order_relaxed)) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "[immd:%s] %s\n", level_tag(lv), buf);
}

const char* err_name(Err e) {
  switch (e) {
    case Err::Ok: return "Ok";
    case Err::Canceled: return "Canceled";
    case Err::Timeout: return "Timeout";
    case Err::Network: return "Network";
    case Err::HttpClient: return "HttpClient";
    case Err::HttpServer: return "HttpServer";
    case Err::Protocol: return "Protocol";
    case Err::Io: return "Io";
    case Err::Integrity: return "Integrity";
    case Err::Busy: return "Busy";
    case Err::State: return "State";
    case Err::NotFound: return "NotFound";
    case Err::Expired: return "Expired";
    case Err::Rejected: return "Rejected";
  }
  return "Unknown";
}

}  // namespace immd

