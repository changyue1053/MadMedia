// 内核日志：等级过滤 + 单行输出。规则：日志永不打印文件内容与完整 URL（签名即凭证）。
#pragma once

#include <cstdarg>

namespace immd {

enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

void set_log_level(LogLevel lv);
LogLevel log_level();
void log_at(LogLevel lv, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace immd

#define IMMD_DLOG(...) ::immd::log_at(::immd::LogLevel::Debug, __VA_ARGS__)
#define IMMD_ILOG(...) ::immd::log_at(::immd::LogLevel::Info, __VA_ARGS__)
#define IMMD_WLOG(...) ::immd::log_at(::immd::LogLevel::Warn, __VA_ARGS__)
#define IMMD_ELOG(...) ::immd::log_at(::immd::LogLevel::Error, __VA_ARGS__)

