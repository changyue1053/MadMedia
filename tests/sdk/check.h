#pragma once
#include <cstdio>
#include <cstdlib>
// Always evaluates expression, including in Release/NDEBUG builds.
#define CHECK(expression) do { \
  if (!(expression)) { \
    std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expression, __FILE__, __LINE__); \
    std::abort(); \
  } \
} while (false)
