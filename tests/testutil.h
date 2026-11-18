// 测试工具：临时目录 fixture、随机文件生成。
#pragma once

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>

namespace immd_test {

inline std::string make_temp_dir() {
  char tmpl[] = "/tmp/immd_test_XXXXXX";
  char* d = mkdtemp(tmpl);
  return d ? std::string(d) : std::string("/tmp");
}

inline void write_random_file(const std::string& path, size_t bytes, uint32_t seed) {
  std::mt19937 rng(seed);
  std::ofstream f(path, std::ios::binary);
  std::string chunk(64 * 1024, 0);
  size_t left = bytes;
  while (left > 0) {
    size_t n = left < chunk.size() ? left : chunk.size();
    for (size_t i = 0; i < n; ++i) chunk[i] = char(rng() & 0xff);
    f.write(chunk.data(), static_cast<std::streamsize>(n));
    left -= n;
  }
}

inline std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace immd_test

