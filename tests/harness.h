// 极简测试框架：零依赖、注册式 TEST 宏、ASSERT 失败即抛出。
// 生产仓库会用 GoogleTest；此处为保证任何环境零网络可构建而内联一个等价子集。
#pragma once

#include <chrono>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace immd_test {

struct Case {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct Failure {
  std::string msg;
};

template <typename A, typename B>
std::string fmt_cmp(const char* file, int line, const char* expr, const A& a, const B& b) {
  std::ostringstream os;
  os << file << ":" << line << "  " << expr << "  (lhs=" << a << ", rhs=" << b << ")";
  return os.str();
}

inline int run_all(int argc, char** argv) {
  std::string filter = argc > 1 ? argv[1] : "";
  int failed = 0, ran = 0;
  for (auto& c : registry()) {
    if (!filter.empty() && c.name.find(filter) == std::string::npos) continue;
    ++ran;
    auto t0 = std::chrono::steady_clock::now();
    auto ms = [&t0] {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
          .count();
    };
    try {
      c.fn();
      std::printf("[ PASS ] %s (%lldms)\n", c.name.c_str(), static_cast<long long>(ms()));
    } catch (const Failure& f) {
      ++failed;
      std::printf("[ FAIL ] %s (%lldms)\n         %s\n", c.name.c_str(), static_cast<long long>(ms()),
                  f.msg.c_str());
    } catch (const std::exception& e) {
      ++failed;
      std::printf("[ FAIL ] %s (%lldms)\n         unexpected exception: %s\n", c.name.c_str(),
                  static_cast<long long>(ms()), e.what());
    }
    std::fflush(stdout);
  }
  std::printf("---- %d ran, %d failed ----\n", ran, failed);
  return failed == 0 ? 0 : 1;
}

}  // namespace immd_test

#define TEST(suite, name)                                                                     \
  static void immd_test_##suite##_##name();                                                   \
  namespace {                                                                                 \
  const ::immd_test::Registrar immd_reg_##suite##_##name(#suite "." #name,                    \
                                                         &immd_test_##suite##_##name);        \
  }                                                                                           \
  static void immd_test_##suite##_##name()

#define ASSERT_TRUE(x)                                                                        \
  do {                                                                                        \
    if (!(x)) throw ::immd_test::Failure{::immd_test::fmt_cmp(__FILE__, __LINE__, #x, "false", "expected true")}; \
  } while (0)

#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))

#define ASSERT_EQ(a, b)                                                                       \
  do {                                                                                        \
    auto _va = (a);                                                                           \
    auto _vb = (b);                                                                           \
    if (!(_va == _vb))                                                                        \
      throw ::immd_test::Failure{::immd_test::fmt_cmp(__FILE__, __LINE__, #a " == " #b, _va, _vb)}; \
  } while (0)

#define ASSERT_NE(a, b)                                                                       \
  do {                                                                                        \
    auto _va = (a);                                                                           \
    auto _vb = (b);                                                                           \
    if (_va == _vb)                                                                           \
      throw ::immd_test::Failure{::immd_test::fmt_cmp(__FILE__, __LINE__, #a " != " #b, _va, _vb)}; \
  } while (0)

#define ASSERT_GE(a, b)                                                                       \
  do {                                                                                        \
    auto _va = (a);                                                                           \
    auto _vb = (b);                                                                           \
    if (!(_va >= _vb))                                                                        \
      throw ::immd_test::Failure{::immd_test::fmt_cmp(__FILE__, __LINE__, #a " >= " #b, _va, _vb)}; \
  } while (0)

#define ASSERT_LE(a, b)                                                                       \
  do {                                                                                        \
    auto _va = (a);                                                                           \
    auto _vb = (b);                                                                           \
    if (!(_va <= _vb))                                                                        \
      throw ::immd_test::Failure{::immd_test::fmt_cmp(__FILE__, __LINE__, #a " <= " #b, _va, _vb)}; \
  } while (0)

