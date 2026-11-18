#include "directory_lock.h"
#if defined(MADMEDIA_VALIDATED_LINUX_LOCK) && !defined(__ANDROID__) && !defined(__OHOS__) && !defined(__OpenHarmony__)
#define MADMEDIA_USE_LINUX_LOCK 1
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif
namespace immd {
DirectoryLock::~DirectoryLock(){
#if defined(MADMEDIA_USE_LINUX_LOCK)
 if(fd_>=0)close(fd_);
#endif
}
Expected<std::unique_ptr<DirectoryLock>> DirectoryLock::acquire(const std::string& directory){
#if !defined(MADMEDIA_USE_LINUX_LOCK)
 (void)directory;return make_err(Err::State,"directory ownership provider unavailable");
#else
 const auto path=directory+"/.madmedia.lock";
 int descriptor;
 do{descriptor=open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);}while(descriptor<0&&errno==EINTR);
 if(descriptor<0)return make_err(Err::Io,"directory lock open failed");
 struct Fd{int value;~Fd(){if(value>=0)close(value);}} fd{descriptor};
 struct stat info{};
 if(fstat(fd.value,&info)!=0||!S_ISREG(info.st_mode)||info.st_nlink!=1)return make_err(Err::Io,"invalid directory lock file");
 int result;
 do{result=flock(fd.value,LOCK_EX|LOCK_NB);}while(result<0&&errno==EINTR);
 if(result<0)return make_err(errno==EWOULDBLOCK||errno==EAGAIN?Err::Busy:Err::Io,"directory ownership unavailable");
 auto owned=std::unique_ptr<DirectoryLock>(new DirectoryLock(fd.value));fd.value=-1;return owned;
#endif
}
}
