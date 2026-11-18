#include <madmedia/madmedia_c.h>
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
  madmedia_config_v1 c{sizeof(c),1,"http://127.0.0.1:1",dir.c_str(),1,0};madmedia_handle h=0;
  CHECK(madmedia_create(&c,&h)==MADMEDIA_OK);char id[128];
  CHECK(madmedia_upload(h,file.c_str(),"application/octet-stream",id,sizeof(id))==MADMEDIA_OK);
  CHECK(madmedia_cancel(h,id)==MADMEDIA_OK);
  bool done=false;
  for(int n=0;n<500&&!done;++n){size_t size=0;auto r=madmedia_poll(h,nullptr,0,&size);
   if(r==MADMEDIA_EMPTY){std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;}
   CHECK(r==MADMEDIA_BUFFER_TOO_SMALL);std::vector<char> data(size);r=madmedia_poll(h,data.data(),data.size(),&size);
   if(r==MADMEDIA_BUFFER_TOO_SMALL)continue;CHECK(r==MADMEDIA_OK);
   done=std::string(data.data()).find("\"terminal\":true")!=std::string::npos;
  }
  CHECK(done);CHECK(madmedia_destroy(h)==MADMEDIA_OK);std::ofstream(argv[3])<<id;return 0;
 }
 CHECK(argc==1);std::string dir="/tmp/madmedia-restart-"+std::to_string(getpid());std::filesystem::create_directories(dir);
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
