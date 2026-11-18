#include "check.h"
#include "ffi/platform/entropy.h"
#include <algorithm>
#include <cstring>
#include <cerrno>
static int mode=0,calls=0;
#if defined(TEST_APPLE)
#include <Security/SecRandom.h>
const SecRandomRef kSecRandomDefault=nullptr;
int SecRandomCopyBytes(SecRandomRef,size_t n,void* p){++calls;std::memset(p,0x5a,n);return mode?-1:0;}
#elif defined(TEST_ANDROID)
#include <sys/random.h>
extern "C" ssize_t __wrap_getrandom(void* p,size_t n,unsigned int flags){
 CHECK(flags==0);++calls;
 if(calls==1){errno=EINTR;return -1;}
 if(mode==1){errno=EIO;return -1;}
 if(mode==2 && calls>2)return 0;
 size_t got=std::min<size_t>(n,3);std::memset(p,0x5a,got);return static_cast<ssize_t>(got);
}
#elif defined(TEST_OHOS)
#include <CryptoArchitectureKit/crypto_rand.h>
static int live=0,blobs=0;
OH_Crypto_ErrCode OH_CryptoRand_Create(OH_CryptoRand** p){
 if(mode==1)return -1;
 *p=new OH_CryptoRand;++live;return mode==4?-1:0;
}
OH_Crypto_ErrCode OH_CryptoRand_GenerateRandom(OH_CryptoRand*,int n,Crypto_DataBlob* out){
 CHECK(n==16);++calls;out->data=new uint8_t[16];++blobs;out->len=mode==3?15:16;
 std::memset(out->data,0x5a,16);return mode==2?-1:0;
}
void OH_Crypto_FreeDataBlob(Crypto_DataBlob* p){delete[] p->data;p->data=nullptr;--blobs;}
void OH_CryptoRand_Destroy(OH_CryptoRand* p){delete p;--live;}
#endif
int main(){
 for(mode=0;mode<5;++mode){
  calls=0;std::array<unsigned char,16> out;out.fill(0xff);
  const bool ok=madmedia_platform::entropy128(out);
#if defined(TEST_APPLE)
  CHECK(ok==(mode==0));CHECK(calls==1);
#elif defined(TEST_ANDROID)
  CHECK(ok==(mode!=1 && mode!=2));if(ok)CHECK(calls==7);
#elif defined(TEST_OHOS)
  CHECK(ok==(mode==0));CHECK(live==0);CHECK(blobs==0);
#endif
  for(auto b:out)CHECK(b==(ok?0x5a:0));
 }
}
