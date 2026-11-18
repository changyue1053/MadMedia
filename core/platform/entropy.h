#pragma once
#include <array>
namespace madmedia_platform {
// Internal, fixed-size operation-ID entropy. Output is zero on failure.
// Exactly one target provider is linked; no fallback and no public injection.
bool entropy128(std::array<unsigned char,16>& out) noexcept;
}
