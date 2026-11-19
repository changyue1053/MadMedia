#include "lease_fixture.h"

int main(int argc, char** argv) {
  umask(0077);
  if (argc == 2 && apple_preview::exercise(argv[1])) {
    std::puts("PASS: local advisory lease contention, reacquisition and path checks");
    return EXIT_SUCCESS;
  }
  std::puts("FAIL: local advisory lease");
  return EXIT_FAILURE;
}
