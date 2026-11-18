#include <madmedia/madmedia.h>
#include "core/journal/journal.h"
#include "tests/testutil.h"
#include <thread>
#include <sqlite3.h>
#include "check.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
extern "C" int __real_open(const char*,int,...);
extern "C" ssize_t __real_read(int,void*,size_t);
static std::atomic<int> entropy_fd{-1},reads{0};
static int mode=0;
extern "C" int __wrap_open(const char* p,int flags,...){
 if(std::strcmp(p,"/dev/urandom")==0){
  if(mode==0){errno=EIO;return -1;}
  int fd=__real_open("/dev/null",flags);entropy_fd=fd;return fd;
 }
 if(flags&O_CREAT){va_list args;va_start(args,flags);int permissions=va_arg(args,int);va_end(args);return __real_open(p,flags,permissions);}
 return __real_open(p,flags);
}
extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd){
 if(fd==entropy_fd.load())entropy_fd=-1;
 return __real_close(fd);
}
extern "C" ssize_t __wrap_read(int fd,void* data,size_t len){
 if(fd!=entropy_fd)return __real_read(fd,data,len);
 int n=++reads;
 if(n==1){errno=EINTR;return -1;}
 if(mode==2&&n>=3)return 0;
 if(mode==3&&n>=3){errno=EIO;return -1;}
 size_t size=std::min<size_t>(len,3);std::memset(data,0xa5,size);return static_cast<ssize_t>(size);
}
// glibc fortification may rewrite read() to __read_chk at -O2.
// Preserve the real bounds check for invalid calls, then use the same injector.
extern "C" ssize_t __real___read_chk(int,void*,size_t,size_t);
extern "C" ssize_t __wrap___read_chk(int fd,void* data,size_t len,size_t available){
 if(len>available)return __real___read_chk(fd,data,len,available);
 return __wrap_read(fd,data,len);
}
extern "C" int __real_sqlite3_step(sqlite3_stmt*);
extern "C" int __wrap_sqlite3_step(sqlite3_stmt* statement){
 const char* sql=sqlite3_sql(statement);
 if(mode==4 && sql && std::strcmp(sql,"SELECT task_id FROM transfer_task")==0)return SQLITE_IOERR;
 return __real_sqlite3_step(statement);
}
int main(int argc,char** argv){
 CHECK(argc==2);mode=std::atoi(argv[1]);
 std::string dir=immd_test::make_temp_dir(),file=dir+"/input";std::ofstream(file)<<"fixture";
 immd::Config cfg;cfg.base_url="http://127.0.0.1:1";cfg.data_dir=dir;
 std::atomic<int> events{0};auto engine=immd::Engine::start(cfg,[&](const auto&){++events;});
 if(mode==4){CHECK(!engine.ok());CHECK(engine.error().code==immd::Err::Io);CHECK(events==0);return 0;}
 CHECK(engine.ok());
 auto uploaded=engine.value()->upload({file,"application/octet-stream",immd::Priority::P4_File,""});
 if(mode==1){
  CHECK(uploaded.ok());CHECK(uploaded.value()=="t_a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5");
  auto duplicate=engine.value()->upload({file,"application/octet-stream",immd::Priority::P4_File,""});CHECK(!duplicate.ok());CHECK(duplicate.error().code==immd::Err::State);
  CHECK(!engine.value()->upload({file,"application/octet-stream",immd::Priority::P4_File,"../escape"}).ok());
  std::atomic<int> admitted{0};std::vector<std::thread> workers;
  for(int n=0;n<12;++n)workers.emplace_back([&]{if(engine.value()->upload({file,"application/octet-stream",immd::Priority::P4_File,"shared_id"}).ok())++admitted;});
  for(auto& worker:workers)worker.join();CHECK(admitted==1);
  CHECK(engine.value()->query(uploaded.value()).ok());engine.value().reset();
  auto reopened=immd::Engine::start(cfg,{});CHECK(reopened.ok());
  auto existing=reopened.value()->upload({file,"application/octet-stream",immd::Priority::P4_File,uploaded.value()});CHECK(!existing.ok());CHECK(existing.error().code==immd::Err::State);
  auto collision=reopened.value()->upload({file,"application/octet-stream",immd::Priority::P4_File,""});CHECK(!collision.ok());CHECK(collision.error().code==immd::Err::State);reopened.value().reset();
  auto journal=immd::Journal::open(dir+"/journal.db");CHECK(journal.ok());auto rows=journal.value()->load_all();CHECK(rows.ok());CHECK(rows.value().size()==2);
 }else{
  CHECK(!uploaded.ok());CHECK(uploaded.error().code==immd::Err::Busy);
  auto dl=engine.value()->download({"media",dir+"/out",immd::Priority::P4_File,""});CHECK(!dl.ok());CHECK(dl.error().code==immd::Err::Busy);
  auto stream=engine.value()->stream_begin({});CHECK(!stream.ok());CHECK(stream.error().code==immd::Err::Busy);engine.value().reset();CHECK(events==0);
  auto journal=immd::Journal::open(dir+"/journal.db");CHECK(journal.ok());auto rows=journal.value()->load_all();CHECK(rows.ok());CHECK(rows.value().empty());
 }
}
