#include "entropy.h"

#include <cstdio>
#include <cstdlib>

#ifndef __APPLE__
#error This fixture requires the actual Apple SDK provider.
#endif

int main() {
  std::array<unsigned char,16> first{}, second{}, zero{};
  if (!madmedia_platform::entropy128(first) || !madmedia_platform::entropy128(second) ||
      first == zero || second == zero || first == second) {
    std::puts("FAIL: Apple Security framework entropy");
    return EXIT_FAILURE;
  }
  std::puts("PASS: public Apple entropy provider with Security framework");
  return EXIT_SUCCESS;
}
