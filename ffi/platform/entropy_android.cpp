// Candidate provider for Android API 28+ (bionic getrandom); not qualified.
#include "entropy.h"
#include <sys/random.h>
#include <cerrno>
namespace madmedia_platform {
bool entropy128(std::array<unsigned char,16>& out) noexcept {
 out.fill(0);std::array<unsigned char,16> scratch{};
 size_t have=0;
 while(have<scratch.size()) {
  auto n=getrandom(scratch.data()+have,scratch.size()-have,0);
  if(n<0 && errno==EINTR)continue;
  if(n<=0 || static_cast<size_t>(n)>scratch.size()-have)return false;
  have+=static_cast<size_t>(n);
 }
 out=scratch;return true;
}
}
