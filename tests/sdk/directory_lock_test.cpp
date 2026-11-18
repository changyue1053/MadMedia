#include <madmedia/madmedia.h>
#include <madmedia/madmedia_c.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include "check.h"
#include "tests/testutil.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
using namespace immd;
Expected<std::unique_ptr<Engine>> start(const std::string& dir){Config c;c.base_url="http://127.0.0.1:1";c.data_dir=dir;return Engine::start(c,{});}
int main(int argc,char** argv){
 if(argc==3){
  auto engine=start(argv[2]);if(!engine.ok())return engine.error().code==Err::Busy?20:21;
  if(std::string(argv[1])=="hold"){std::ofstream(std::string(argv[2])+"/ready")<<"ready";for(;;)pause();}
  return 0;
 }
 CHECK(argc==1);auto dir=immd_test::make_temp_dir();auto first=start(dir);CHECK(first.ok());
 auto duplicate=start(dir);CHECK(!duplicate.ok()&&duplicate.error().code==Err::Busy);
 auto alias=start(dir+"/.");CHECK(!alias.ok()&&alias.error().code==Err::Busy);
 auto links=immd_test::make_temp_dir();std::filesystem::create_directory_symlink(dir,links+"/alias");auto symlink=start(links+"/alias");CHECK(!symlink.ok()&&symlink.error().code==Err::Busy);
 auto independent=start(immd_test::make_temp_dir());CHECK(independent.ok());independent.value().reset();
 auto child=[&](const char* operation){pid_t pid=fork();CHECK(pid>=0);if(pid==0){execl(argv[0],argv[0],operation,dir.c_str(),static_cast<char*>(nullptr));_exit(99);}return pid;};
 int status=0;auto blocked=child("try");CHECK(waitpid(blocked,&status,0)==blocked);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==20);
 first.value().reset();CHECK(std::filesystem::exists(dir+"/.madmedia.lock"));auto allowed=child("try");CHECK(waitpid(allowed,&status,0)==allowed);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
 auto owner=child("hold");bool ready=false;for(int n=0;n<500;++n){if(std::filesystem::exists(dir+"/ready")){ready=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(5));}CHECK(ready);
 CHECK(!start(dir).ok());CHECK(kill(owner,SIGKILL)==0);CHECK(waitpid(owner,&status,0)==owner);CHECK(WIFSIGNALED(status));auto after_crash=start(dir);CHECK(after_crash.ok());after_crash.value().reset();
 // Refuse nonregular/symlink lock files without touching their targets.
 auto bad=immd_test::make_temp_dir();std::ofstream(bad+"/sentinel")<<"unchanged";std::filesystem::create_symlink(bad+"/sentinel",bad+"/.madmedia.lock");CHECK(!start(bad).ok());CHECK(immd_test::read_file(bad+"/sentinel")=="unchanged");
 auto fifo=immd_test::make_temp_dir();CHECK(mkfifo((fifo+"/.madmedia.lock").c_str(),0600)==0);CHECK(!start(fifo).ok());
 // Journal init failure must not leak directory ownership.
 auto broken=immd_test::make_temp_dir();std::filesystem::create_directory(broken+"/journal.db");CHECK(!start(broken).ok());std::filesystem::remove(broken+"/journal.db");CHECK(start(broken).ok());
 // Callback draining during close retains ownership, including reentrant open.
 auto draining=immd_test::make_temp_dir();auto input=draining+"/input";std::ofstream(input)<<"fixture";
 Config config;config.base_url="http://127.0.0.1:1";config.data_dir=draining;
 std::mutex mutex;std::condition_variable cv;bool entered=false,released=false;std::atomic<bool> once{false},reentry_rejected{false};
 auto closing=Engine::start(config,[&](const auto&){
  if(once.exchange(true))return;
  {std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_all();cv.wait(lock,[&]{return released;});}
  auto nested=start(draining);reentry_rejected=!nested.ok()&&nested.error().code==Err::Busy;
 });CHECK(closing.ok());CHECK(closing.value()->upload({input,"application/octet-stream",Priority::P4_File,""}).ok());
 {std::unique_lock<std::mutex> lock(mutex);CHECK(cv.wait_for(lock,std::chrono::seconds(5),[&]{return entered;}));}
 std::thread closer([owned=std::move(closing.value())]()mutable{owned.reset();});
 auto during_close=start(draining);CHECK(!during_close.ok()&&during_close.error().code==Err::Busy);
 {std::lock_guard<std::mutex> lock(mutex);released=true;}cv.notify_all();closer.join();CHECK(reentry_rejected);CHECK(start(draining).ok());
#if defined(MADMEDIA_TEST_C_ABI)
 auto cdir=immd_test::make_temp_dir();madmedia_config_v1 c{sizeof(c),1,"http://127.0.0.1:1",cdir.c_str(),1,0};madmedia_handle a=0,b=0;
 CHECK(madmedia_create(&c,&a)==MADMEDIA_OK);CHECK(madmedia_create(&c,&b)==MADMEDIA_NATIVE_ERROR);CHECK(b==0);
 CHECK(madmedia_destroy(a)==MADMEDIA_OK);CHECK(madmedia_create(&c,&b)==MADMEDIA_OK);CHECK(madmedia_destroy(b)==MADMEDIA_OK);
#endif

}
