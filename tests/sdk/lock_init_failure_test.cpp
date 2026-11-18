#include <madmedia/madmedia.h>
#include "core/platform/directory_lock.h"
#include "check.h"
#include "tests/testutil.h"
#include <atomic>
#include <cstdlib>
#include <new>
#include <sqlite3.h>
static std::atomic<size_t> fail_size{0};
static std::atomic<bool> injected{false},owned_during_close{false};
static std::atomic<int> observed_closes{0};
static std::string directory;
void* operator new(size_t n){
 if(n==fail_size.load()&&fail_size.exchange(0)!=0){injected=true;throw std::bad_alloc();}
 if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,size_t)noexcept{std::free(p);}
void operator delete[](void* p,size_t)noexcept{std::free(p);}
extern "C" int __real_sqlite3_close(sqlite3*);
extern "C" int __wrap_sqlite3_close(sqlite3* db){
 if(injected){auto attempt=immd::DirectoryLock::acquire(directory);++observed_closes;owned_during_close=!attempt.ok()&&attempt.error().code==immd::Err::Busy;}
 return __real_sqlite3_close(db);
}
int main(){
 directory=immd_test::make_temp_dir();madmedia::Config c;c.data_dir=directory;
 c.base_url=std::string(131071,'x');c.base_url.replace(0,7,"http://");
 fail_size=c.base_url.size()+1;
 // Moving the argument avoids failing the call-site copy. The next allocation
 // of exactly this string size is Impl::cfg's copy after journal open.
 auto result=madmedia::Engine::start(std::move(c),{});
 CHECK(injected);CHECK(!result.ok());CHECK(result.error().code==madmedia::Err::Busy);
 CHECK(observed_closes==1);CHECK(owned_during_close);
 injected=false;madmedia::Config retry;retry.base_url="http://127.0.0.1:1";retry.data_dir=directory;
 CHECK(madmedia::Engine::start(std::move(retry),{}).ok());
}
