#include <madmedia/madmedia.h>
#include "check.h"
#include "tests/testutil.h"
#include <sqlite3.h>
#include <atomic>
#include <fstream>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
using namespace immd;
static std::atomic<int> inserts{0}, read_fail_after{-1}, read_failure{SQLITE_IOERR};
static std::atomic<bool> fail_save{false}, fail_bind{false};
extern "C" int __real_sqlite3_bind_text(sqlite3_stmt*,int,const char*,int,void(*)(void*));
extern "C" int __wrap_sqlite3_bind_text(sqlite3_stmt* s,int i,const char* v,int n,void(*d)(void*)) {
 if(fail_bind)return SQLITE_NOMEM;
 return __real_sqlite3_bind_text(s,i,v,n,d);
}
extern "C" int __real_sqlite3_step(sqlite3_stmt*);
extern "C" int __wrap_sqlite3_step(sqlite3_stmt* s) {
 const char* sql=sqlite3_sql(s);
 if(sql && std::string(sql).find("SELECT task_id, kind,")==0 && read_fail_after>=0){
  if(read_fail_after--==0)return read_failure;
 }
 if(fail_save && sql && std::string(sql).find("INSERT INTO transfer_task")!=std::string::npos)return SQLITE_IOERR;
 int rc=__real_sqlite3_step(s);
 if(rc==SQLITE_DONE && sql && std::string(sql).find("INSERT INTO transfer_task")!=std::string::npos)++inserts;
 return rc;
}
static Config config(const std::string& dir){Config c;c.data_dir=dir;c.base_url="http://127.0.0.1:1";c.max_active_tasks=0;std::ofstream(dir+"/source")<<"retained-source";return c;}
int main(int argc,char** argv){
 if(argc==4){
  auto cfg=config(argv[1]);cfg.journal_durability=JournalDurability::Full;int fd=std::stoi(argv[2]);
  auto e=Engine::start(cfg,[&](const ProgressEvent& p){if(p.journal_receipt==JournalReceipt::Full){char ok='Y';CHECK(write(fd,&ok,1)==1);}});CHECK(e.ok());
  CHECK(e.value()->upload({cfg.data_dir+"/source","application/octet-stream",Priority::P4_File,"crash_receipt"}).ok());
  for(;;)pause();
 }
 for(auto mode:{JournalDurability::Normal,JournalDurability::Full}){
  auto cfg=config(immd_test::make_temp_dir());cfg.journal_durability=mode;
  std::mutex mu;std::condition_variable cv;std::vector<ProgressEvent> events;
  auto e=Engine::start(cfg,[&](const ProgressEvent& p){std::lock_guard<std::mutex> l(mu);events.push_back(p);cv.notify_all();});CHECK(e.ok());
  auto id=e.value()->upload({cfg.data_dir+"/source","application/octet-stream",Priority::P4_File,"receipt"});CHECK(id.ok());CHECK(!e.value()->cancel(id.value()));
  {std::unique_lock<std::mutex> l(mu);CHECK(cv.wait_for(l,std::chrono::seconds(5),[&]{return events.size()>=2;}));}
  e.value().reset();CHECK(events.size()==2);CHECK(events[0].state==TaskState::Queued);CHECK(events[0].journal_receipt==(mode==JournalDurability::Full?JournalReceipt::Full:JournalReceipt::Normal));CHECK(events[1].state==TaskState::Canceled);CHECK(events[1].journal_receipt==JournalReceipt::None);
 }
 {
  auto cfg=config(immd_test::make_temp_dir());cfg.journal_durability=JournalDurability::Full;
  std::mutex mu;std::condition_variable cv;std::vector<ProgressEvent> events;
  auto e=Engine::start(cfg,[&](const ProgressEvent& p){std::lock_guard<std::mutex> l(mu);events.push_back(p);cv.notify_all();});CHECK(e.ok());fail_save=true;
  CHECK(e.value()->upload({cfg.data_dir+"/source","application/octet-stream",Priority::P4_File,"failed_receipt"}).ok());
  {std::unique_lock<std::mutex> l(mu);CHECK(cv.wait_for(l,std::chrono::seconds(5),[&]{return !events.empty();}));}
  e.value().reset();fail_save=false;CHECK(events.size()==1);CHECK(events[0].journal_receipt==JournalReceipt::Failed);CHECK(events[0].state==TaskState::Failed);
  auto j=Journal::open(cfg.data_dir+"/journal.db");CHECK(j.ok());CHECK(!j.value()->load("failed_receipt").ok());
 }
 {
  auto cfg=config(immd_test::make_temp_dir());cfg.journal_durability=static_cast<JournalDurability>(99);CHECK(!Engine::start(cfg,{}).ok());
  CHECK(!Journal::open(":memory:",JournalDurability::Full).ok()); // WAL not available: fail closed
 }
 {
  auto dir=immd_test::make_temp_dir();auto j=Journal::open(dir+"/journal.db",JournalDurability::Full);CHECK(j.ok());
  TaskRecord rec;rec.task_id="bind_failure";fail_bind=true;CHECK(j.value()->save(rec));fail_bind=false;
  CHECK(!j.value()->load(rec.task_id).ok());CHECK(!j.value()->save(rec));CHECK(j.value()->load(rec.task_id).ok());
  for(int code:{SQLITE_IOERR,SQLITE_BUSY}){read_failure=code;
  read_fail_after=0;auto lookup=j.value()->load(rec.task_id);CHECK(!lookup.ok()&&lookup.error().code==Err::Io);
  read_fail_after=1;auto scan=j.value()->load_unfinished();CHECK(!scan.ok()&&scan.error().code==Err::Io);
  read_fail_after=1;auto all=j.value()->load_all();CHECK(!all.ok()&&all.error().code==Err::Io);read_fail_after=-1;
  }
 }
 {
  auto cfg=config(immd_test::make_temp_dir());inserts=0;std::mutex mu;std::condition_variable cv;bool release=false,entered=false;int receipts=0;
  auto e=Engine::start(cfg,[&](const ProgressEvent& p){std::unique_lock<std::mutex> l(mu);entered=true;cv.notify_all();cv.wait(l,[&]{return release;});CHECK(p.journal_receipt==JournalReceipt::Normal);++receipts;});CHECK(e.ok());
  CHECK(e.value()->upload({cfg.data_dir+"/source","application/octet-stream",Priority::P4_File,"saturation0"}).ok());
  {std::unique_lock<std::mutex> l(mu);CHECK(cv.wait_for(l,std::chrono::seconds(5),[&]{return entered;}));}
  for(int i=1;i<4200;++i){
   bool accepted=false;for(int tries=0;tries<5000&&!accepted;++tries){
    auto r=e.value()->upload({cfg.data_dir+"/source","application/octet-stream",Priority::P4_File,"saturation"+std::to_string(i)});
    if(r.ok())accepted=true;else{CHECK(r.error().code==Err::Busy);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
   }CHECK(accepted);
  }
  for(int tries=0;tries<15000&&inserts<4098;++tries)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::fprintf(stderr,"saturation committed=%d\n",inserts.load());
  CHECK(inserts==4098); // callback worker blocked + queue full + scheduler blocked on next receipt

  {std::lock_guard<std::mutex> l(mu);release=true;}cv.notify_all();e.value().reset();CHECK(receipts==4200);
 }
 auto dir=immd_test::make_temp_dir();int pipefd[2];CHECK(pipe(pipefd)==0);pid_t child=fork();CHECK(child>=0);
 if(child==0){close(pipefd[0]);std::string fd=std::to_string(pipefd[1]);execl(argv[0],argv[0],dir.c_str(),fd.c_str(),"child",nullptr);_exit(127);}
 close(pipefd[1]);char ok=0;CHECK(read(pipefd[0],&ok,1)==1&&ok=='Y');close(pipefd[0]);CHECK(kill(child,SIGKILL)==0);int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFSIGNALED(status));
 auto j=Journal::open(dir+"/journal.db",JournalDurability::Full);CHECK(j.ok());auto record=j.value()->load("crash_receipt");CHECK(record.ok());CHECK(record.value().state==TaskState::Queued);CHECK(record.value().orig_path==dir+"/source");j.value().reset();
 std::mutex mu;std::condition_variable cv;bool recovered=false;
 auto e=Engine::start(config(dir),[&](const ProgressEvent& p){if(p.task_id=="crash_receipt"){CHECK(p.journal_receipt==JournalReceipt::None);std::lock_guard<std::mutex> l(mu);recovered=true;cv.notify_all();}});CHECK(e.ok());CHECK(!e.value()->recover());
 {std::unique_lock<std::mutex> l(mu);CHECK(cv.wait_for(l,std::chrono::seconds(5),[&]{return recovered;}));}e.value().reset();
}
