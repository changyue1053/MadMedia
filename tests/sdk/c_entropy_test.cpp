#include <madmedia/madmedia_c.h>
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
int main(int argc,char** argv){
 CHECK(argc==2);mode=std::atoi(argv[1]);
 std::string dir="/tmp/madmedia-entropy-"+std::to_string(getpid());std::filesystem::create_directories(dir);
 std::string file=dir+"/input";std::ofstream(file)<<"fixture";
 madmedia_config_v1 c{sizeof(c),1,"http://127.0.0.1:1",dir.c_str(),1,0};madmedia_handle h=0;
 CHECK(madmedia_create(&c,&h)==MADMEDIA_OK);char id[128];
 const auto result=madmedia_upload(h,file.c_str(),"application/octet-stream",id,sizeof(id));
 if(mode==1){CHECK(result==MADMEDIA_OK);CHECK(std::string(id)=="madmedia_a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5");CHECK(reads==7);}
 else{CHECK(reads==(mode==0?0:3));CHECK(result==MADMEDIA_INTERNAL);CHECK(id[0]==0);size_t required=0;CHECK(madmedia_poll(h,nullptr,0,&required)==MADMEDIA_EMPTY);}
 CHECK(madmedia_destroy(h)==MADMEDIA_OK);std::filesystem::remove_all(dir);
}
