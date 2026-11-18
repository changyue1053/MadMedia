// 文件系统小工具（POSIX）。
#pragma once

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "core/util/error.h"

namespace immd {

inline Error mkdirs(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur.push_back(path[i]);
    if (path[i] == '/' || i + 1 == path.size()) {
      if (cur == "/" || cur.empty()) continue;
      if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
        return make_err(Err::Io, "mkdir failed: " + cur);
      }
    }
  }
  return {};
}

inline Expected<int64_t> file_size(const std::string& path) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0) return make_err(Err::Io, "stat failed: " + path);
  return static_cast<int64_t>(st.st_size);
}

inline bool file_exists(const std::string& path) {
  struct stat st{};
  return stat(path.c_str(), &st) == 0;
}

inline void remove_file(const std::string& path) { ::unlink(path.c_str()); }

inline Error rename_file(const std::string& from, const std::string& to) {
  if (::rename(from.c_str(), to.c_str()) != 0) return make_err(Err::Io, "rename failed");
  return {};
}

}  // namespace immd

