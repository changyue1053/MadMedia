// Candidate provider; not platform-qualified. Link Security.framework.
#include "entropy.h"
#include <Security/SecRandom.h>
namespace madmedia_platform {
bool entropy128(std::array<unsigned char,16>& out) noexcept {
 out.fill(0);std::array<unsigned char,16> scratch{};
 if(SecRandomCopyBytes(kSecRandomDefault,scratch.size(),scratch.data())!=0)return false;
 out=scratch;return true;
}
}
