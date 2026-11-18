#include "entropy.h"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
namespace madmedia_platform {
bool entropy128(std::array<unsigned char,16>& out) noexcept {
 out.fill(0);std::array<unsigned char,16> scratch{};
 int fd;
 do {fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);} while(fd<0 && errno==EINTR);
 if(fd<0)return false;
 size_t have=0;
 while(have<scratch.size()) {
  auto n=read(fd,scratch.data()+have,scratch.size()-have);
  if(n<0 && errno==EINTR)continue;
  if(n<=0 || static_cast<size_t>(n)>scratch.size()-have){close(fd);return false;}
  have+=static_cast<size_t>(n);
 }
 close(fd);out=scratch;return true;
}
}
