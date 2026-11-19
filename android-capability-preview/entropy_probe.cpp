#include "entropy.h"
#ifndef __ANDROID__
#error Target Android required
#endif
#if !defined(__aarch64__) || !defined(EXPECTED_ANDROID_API) || __ANDROID_API__ != EXPECTED_ANDROID_API
#error Selected Android arm64 API required
#endif

int main() {
    // Link the original provider; the fixture is not executed by the checker.
    std::array<unsigned char,16> output{};
    return madmedia_platform::entropy128(output) ? 0 : 1;
}
