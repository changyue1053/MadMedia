#include "check.h"
#include "core/platform/entropy.h"
#include "core/platform/directory_lock.h"
int main(){std::array<unsigned char,16> out;out.fill(0xff);CHECK(!madmedia_platform::entropy128(out));for(auto b:out)CHECK(b==0);auto lock=immd::DirectoryLock::acquire("/tmp/madmedia-unqualified-provider-must-not-open");CHECK(!lock.ok());CHECK(lock.error().code==immd::Err::State);}
