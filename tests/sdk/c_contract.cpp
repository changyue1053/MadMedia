#include <madmedia/madmedia_c.h>
#include "check.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <unistd.h>
#include <sqlite3.h>
int main(){
 std::string dir="/tmp/madmedia-c-"+std::to_string(getpid());
 std::filesystem::create_directories(dir);std::string file=dir+"/input";std::ofstream(file)<<"fixture";
 madmedia_config_v1 c{sizeof(c),1,"http://127.0.0.1:1",dir.c_str(),1,0};
 madmedia_handle h=0;CHECK(madmedia_create(&c,&h)==MADMEDIA_OK && h);
 size_t required=999;CHECK(madmedia_poll(h,nullptr,0,&required)==MADMEDIA_EMPTY && required==0);
 char id[128],second[128],tiny[2];
 CHECK(madmedia_upload(h,file.c_str(),"application/octet-stream",tiny,sizeof(tiny))==MADMEDIA_BUFFER_TOO_SMALL);
 CHECK(madmedia_upload(h,file.c_str(),"application/octet-stream",id,sizeof(id))==MADMEDIA_OK);
 CHECK(madmedia_upload(h,file.c_str(),"application/octet-stream",second,sizeof(second))==MADMEDIA_BUSY);
 CHECK(madmedia_cancel(h,id)==MADMEDIA_OK);
 bool terminal=false;std::string body;
 for(int n=0;n<500 && !terminal;++n){
  auto status=madmedia_poll(h,nullptr,0,&required);
  if(status==MADMEDIA_EMPTY){std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;}
  CHECK(status==MADMEDIA_BUFFER_TOO_SMALL && required>1);
  CHECK(madmedia_poll(h,tiny,sizeof(tiny),&required)==MADMEDIA_BUFFER_TOO_SMALL);
  CHECK(madmedia_upload(h,file.c_str(),"application/octet-stream",second,sizeof(second))==MADMEDIA_BUSY);
  std::vector<char> data(required);
  status=madmedia_poll(h,data.data(),data.size(),&required);
  if(status==MADMEDIA_BUFFER_TOO_SMALL)continue;
  CHECK(status==MADMEDIA_OK);body=data.data();
  terminal=body.find("\"terminal\":true")!=std::string::npos;
 }
 CHECK(terminal);CHECK(body.find(id)!=std::string::npos);CHECK(body.find("fixture")==std::string::npos);
 CHECK(madmedia_upload(h,file.c_str(),"application/octet-stream",second,sizeof(second))==MADMEDIA_OK);
 // Concurrent poll/close: calls linearize as OK/EMPTY/BUFFER_TOO_SMALL or CLOSED.
 std::atomic<bool> stop{false};
 std::thread poller([&]{while(!stop){size_t size=0;auto r=madmedia_poll(h,nullptr,0,&size);CHECK(r==MADMEDIA_EMPTY||r==MADMEDIA_BUFFER_TOO_SMALL||r==MADMEDIA_CLOSED);}});
 CHECK(madmedia_destroy(h)==MADMEDIA_OK);stop=true;poller.join();
 CHECK(madmedia_destroy(h)==MADMEDIA_CLOSED);
 CHECK(madmedia_poll(h,nullptr,0,&required)==MADMEDIA_CLOSED);
 CHECK(madmedia_cancel(h,id)==MADMEDIA_CLOSED);
 // Accepted command whose first journal write fails must produce a terminal,
 // not occupy admission capacity forever without any event.
 CHECK(madmedia_create(&c,&h)==MADMEDIA_OK);
 sqlite3* db=nullptr;CHECK(sqlite3_open((dir+"/journal.db").c_str(),&db)==SQLITE_OK);
 CHECK(sqlite3_exec(db,"CREATE TRIGGER fail_enqueue BEFORE INSERT ON transfer_task BEGIN SELECT RAISE(ABORT, 'injected'); END",nullptr,nullptr,nullptr)==SQLITE_OK);
 CHECK(madmedia_upload(h,file.c_str(),"application/octet-stream",id,sizeof(id))==MADMEDIA_OK);
 terminal=false;
 for(int n=0;n<500&&!terminal;++n){
  auto r=madmedia_poll(h,nullptr,0,&required);
  if(r==MADMEDIA_EMPTY){std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;}
  CHECK(r==MADMEDIA_BUFFER_TOO_SMALL);std::vector<char> data(required);
  r=madmedia_poll(h,data.data(),data.size(),&required);
  if(r==MADMEDIA_BUFFER_TOO_SMALL)continue;
  CHECK(r==MADMEDIA_OK);body=data.data();terminal=body.find("\"terminal\":true")!=std::string::npos;
 }
 CHECK(terminal && body.find("\"state\":5")!=std::string::npos);
 CHECK(sqlite3_close(db)==SQLITE_OK);CHECK(madmedia_destroy(h)==MADMEDIA_OK);
 std::filesystem::remove_all(dir);
}
