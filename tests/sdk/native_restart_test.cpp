#include <madmedia/madmedia.h>
#include <condition_variable>
#include <mutex>
#include "tests/testutil.h"
#include "check.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <sys/wait.h>
#include <unistd.h>
#include <sqlite3.h>
int main(int argc,char** argv){
 if(argc==4){
  std::string dir=argv[2],file=dir+"/input";
  immd::Config cfg;cfg.base_url="http://127.0.0.1:1";cfg.data_dir=dir;
  std::mutex mutex;std::condition_variable cv;bool done=false;
  auto engine=immd::Engine::start(cfg,[&](const auto& event){if(event.state==immd::TaskState::Canceled||event.state==immd::TaskState::Failed){std::lock_guard<std::mutex> lock(mutex);done=true;cv.notify_all();}});CHECK(engine.ok());
  std::string old;std::ifstream(dir+"/id0")>>old;
  if(!old.empty())CHECK(!engine.value()->upload({file,"application/octet-stream",immd::Priority::P4_File,old}).ok());
  auto id=engine.value()->upload({file,"application/octet-stream",immd::Priority::P4_File,""});CHECK(id.ok());CHECK(!engine.value()->cancel(id.value()));
  {std::unique_lock<std::mutex> lock(mutex);CHECK(cv.wait_for(lock,std::chrono::seconds(5),[&]{return done;}));}
  engine.value().reset();std::ofstream(argv[3])<<id.value();return 0;
 }
 CHECK(argc==1);std::string dir=immd_test::make_temp_dir();
 std::ofstream(dir+"/input")<<"fixture";
 for(int n=0;n<2;++n){std::string result=dir+"/id"+std::to_string(n);pid_t pid=fork();CHECK(pid>=0);
  if(pid==0){execl(argv[0],argv[0],"child",dir.c_str(),result.c_str(),static_cast<char*>(nullptr));_exit(99);}
  int status=0;CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
 }
 std::string a,b;std::ifstream(dir+"/id0")>>a;std::ifstream(dir+"/id1")>>b;CHECK(!a.empty()&&a!=b);
 sqlite3* db=nullptr;CHECK(sqlite3_open((dir+"/journal.db").c_str(),&db)==SQLITE_OK);
 sqlite3_stmt* statement=nullptr;CHECK(sqlite3_prepare_v2(db,"SELECT COUNT(*), COUNT(DISTINCT staging_path) FROM transfer_task",-1,&statement,nullptr)==SQLITE_OK);
 CHECK(sqlite3_step(statement)==SQLITE_ROW);CHECK(sqlite3_column_int(statement,0)==2&&sqlite3_column_int(statement,1)==2);
 sqlite3_finalize(statement);sqlite3_close(db);std::filesystem::remove_all(dir);
}
