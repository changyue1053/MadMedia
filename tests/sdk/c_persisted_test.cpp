#include "../../ffi/madmedia_c.cpp"
#include "check.h"
#include "tests/testutil.h"
#include <sqlite3.h>
#include <new>
#include <thread>
#include <fstream>
static std::atomic<int> step_fault{0};
static std::atomic<bool> column_oom{false};
extern "C" int __real_sqlite3_step(sqlite3_stmt*);
extern "C" int __wrap_sqlite3_step(sqlite3_stmt* s){
 const char* sql=sqlite3_sql(s);
 if(step_fault&&sql&&std::string(sql).find("SELECT task_id, kind,")==0)return step_fault;
 return __real_sqlite3_step(s);
}
extern "C" const unsigned char* __real_sqlite3_column_text(sqlite3_stmt*,int);
extern "C" const unsigned char* __wrap_sqlite3_column_text(sqlite3_stmt* s,int i){
 if(column_oom.exchange(false))throw std::bad_alloc();
 return __real_sqlite3_column_text(s,i);
}
static std::string read_record(madmedia_handle h,const char* id){
 size_t n=0;CHECK(madmedia_query_persisted(h,id,nullptr,0,&n)==MADMEDIA_BUFFER_TOO_SMALL);
 std::vector<char> out(n);CHECK(madmedia_query_persisted(h,id,out.data(),out.size(),&n)==MADMEDIA_OK);return out.data();
}
int main(){
 auto dir=immd_test::make_temp_dir();auto j=immd::Journal::open(dir+"/journal.db");CHECK(j.ok());
 immd::TaskRecord rec;rec.task_id="retained_task";rec.media_id="public-id";rec.orig_path="private-source";rec.dest_path="private-destination";rec.last_error="Bearer private-token";CHECK(!j.value()->save(rec));j.value().reset();
 madmedia_config_v2 cfg{sizeof(cfg),2,"http://127.0.0.1:1",dir.c_str(),1,0,MADMEDIA_JOURNAL_FULL,0};madmedia_handle h;CHECK(madmedia_create_v2(&cfg,&h)==MADMEDIA_OK);
 size_t n=99;CHECK(madmedia_query_persisted(h,"../file",nullptr,0,&n)==MADMEDIA_INVALID&&n==0);
 CHECK(madmedia_query_persisted(h,"",nullptr,0,&n)==MADMEDIA_INVALID);
 auto oversized=std::string(129,'a');CHECK(madmedia_query_persisted(h,oversized.c_str(),nullptr,0,&n)==MADMEDIA_INVALID);
 CHECK(madmedia_query_persisted(h,"absent",nullptr,0,&n)==MADMEDIA_NOT_FOUND&&n==0);
 auto out=read_record(h,rec.task_id.c_str());CHECK(out.find("private")==std::string::npos);CHECK(out.find("journal_receipt")==std::string::npos);CHECK(out.find("persisted_record")!=std::string::npos);
 CHECK(madmedia_poll(h,nullptr,0,&n)==MADMEDIA_EMPTY); // no recovery or slot admission
 for(int code:{SQLITE_IOERR,SQLITE_BUSY,SQLITE_NOMEM}){step_fault=code;n=99;CHECK(madmedia_query_persisted(h,rec.task_id.c_str(),nullptr,0,&n)==MADMEDIA_NATIVE_ERROR&&n==0);}step_fault=0;
 column_oom=true;n=99;CHECK(madmedia_query_persisted(h,rec.task_id.c_str(),nullptr,0,&n)==MADMEDIA_INTERNAL&&n==0);CHECK(!read_record(h,rec.task_id.c_str()).empty()); // scheduler survived OOM
 sqlite3* db;CHECK(sqlite3_open((dir+"/journal.db").c_str(),&db)==SQLITE_OK);
 CHECK(madmedia_query_persisted(h,rec.task_id.c_str(),nullptr,0,&n)==MADMEDIA_BUFFER_TOO_SMALL);std::vector<char> old(n);
 std::string sql="UPDATE transfer_task SET media_id='"+std::string(300,'m')+"' WHERE task_id='retained_task'";CHECK(sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK);
 CHECK(madmedia_query_persisted(h,rec.task_id.c_str(),old.data(),old.size(),&n)==MADMEDIA_BUFFER_TOO_SMALL&&n>old.size());CHECK(read_record(h,rec.task_id.c_str()).find(std::string(300,'m'))!=std::string::npos);
 CHECK(sqlite3_exec(db,"UPDATE transfer_task SET state=999",nullptr,nullptr,nullptr)==SQLITE_OK);CHECK(madmedia_query_persisted(h,rec.task_id.c_str(),nullptr,0,&n)==MADMEDIA_NATIVE_ERROR);
 CHECK(sqlite3_exec(db,"UPDATE transfer_task SET state=0",nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(db);
 CHECK(madmedia_destroy(h)==MADMEDIA_OK);CHECK(madmedia_query_persisted(h,rec.task_id.c_str(),nullptr,0,&n)==MADMEDIA_CLOSED);
 // Reopen and query old rows without recover; no dependence on original sync mode.
 madmedia_config_v1 v1{sizeof(v1),1,cfg.base_url,cfg.data_dir,1,0};CHECK(madmedia_create(&v1,&h)==MADMEDIA_OK);CHECK(!read_record(h,rec.task_id.c_str()).empty());
 std::thread reader([&]{for(;;){size_t bytes;auto rc=madmedia_query_persisted(h,rec.task_id.c_str(),nullptr,0,&bytes);if(rc==MADMEDIA_CLOSED)return;CHECK(rc==MADMEDIA_BUFFER_TOO_SMALL);}});
 CHECK(madmedia_destroy(h)==MADMEDIA_OK);reader.join();
 // Native persisted query deliberately bypasses a live in-memory record.
 immd::Config native;native.base_url=cfg.base_url;native.data_dir=dir;native.max_active_tasks=0;
 auto file=dir+"/source";std::ofstream(file)<<"fixture";auto engine=immd::Engine::start(native,{});CHECK(engine.ok());
 auto id=engine.value()->upload({file,"x",immd::Priority::P4_File,"live_task"});CHECK(id.ok());CHECK(engine.value()->query("live_task").value().state==immd::TaskState::Queued);
 CHECK(sqlite3_open((dir+"/journal.db").c_str(),&db)==SQLITE_OK);CHECK(sqlite3_exec(db,"UPDATE transfer_task SET state=6 WHERE task_id='live_task'",nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(db);
 CHECK(engine.value()->query("live_task").value().state==immd::TaskState::Queued);CHECK(engine.value()->query_persisted("live_task").value().state==immd::TaskState::Paused);engine.value().reset();
}
