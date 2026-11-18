#include <madmedia/madmedia.h>
#include <pthread.h>
#include <dlfcn.h>
#include <atomic>
#include <algorithm>
#include <thread>
#include "check.h"
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>

static std::atomic<int> calls{0};
static int fail_at=0;
extern "C" int pthread_create(pthread_t* t,const pthread_attr_t* a,void*(*fn)(void*),void* p) {
 if (++calls==fail_at) return EAGAIN;
 using Create = int(*)(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
 auto real = reinterpret_cast<Create>(dlsym(RTLD_NEXT,"pthread_create"));
 CHECK(real); return real(t,a,fn,p);
}
int main(int argc,char** argv) {
 CHECK(argc==2); fail_at=std::atoi(argv[1]);
 const int workers=static_cast<int>(std::min(4u,std::max(1u,std::thread::hardware_concurrency()-1)));
 // Small CI machines create fewer workers; cap at scheduler creation rather than skipping.
 fail_at=std::min(fail_at,workers+3);
 madmedia::Config c; c.base_url="http://127.0.0.1";
 c.data_dir="/tmp/madmedia-thread-failure-"+std::to_string(getpid());
 auto e=madmedia::start(c);
 CHECK(!e.ok()); CHECK(e.error().code==madmedia::Err::Busy);
 fail_at=0;auto reopened=madmedia::start(c);CHECK(reopened.ok());reopened.value().reset();
 std::filesystem::remove_all(c.data_dir);
}
