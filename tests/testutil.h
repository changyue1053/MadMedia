// 测试工具：临时目录 fixture、随机文件生成。
#pragma once

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace immd_test {

// Track only directories created by this test process. Never sweep /tmp or
// remove a pre-existing path. Failure to allocate a fixture must not use /tmp.
struct TempFixtures {
  std::mutex mutex;
  std::vector<std::string> owned;
  ~TempFixtures() {
    for (const auto& p : owned) { std::error_code ec; std::filesystem::remove_all(p, ec); }
  }
};
inline std::string make_temp_dir() {
  static TempFixtures fixtures;
  const char* configured = std::getenv("TMPDIR");
  std::string root = configured && *configured ? configured : "/tmp";
  if (root.front() != '/') throw std::runtime_error("test TMPDIR must be absolute");
  std::string pattern = root + "/immd_test_XXXXXX";
  std::vector<char> tmpl(pattern.begin(), pattern.end()); tmpl.push_back(0);
  char* d = mkdtemp(tmpl.data());
  if (!d) throw std::runtime_error("cannot create isolated test fixture");
  try {
    std::string result(d);
    std::lock_guard<std::mutex> lock(fixtures.mutex);
    fixtures.owned.push_back(result);
    return result;  // NRVO or noexcept move; no failure after ownership registration
  } catch (...) {
    std::error_code ec; std::filesystem::remove_all(d, ec); throw;
  }
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

