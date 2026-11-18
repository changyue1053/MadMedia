#include "check.h"
#include "core/platform/entropy.h"
int main(){std::array<unsigned char,16> out;out.fill(0xff);CHECK(!madmedia_platform::entropy128(out));for(auto b:out)CHECK(b==0);}
