#include <functional>
#include <memory>
#include <string>
#include "core/journal/journal.h"
#define private public
#include "core/engine/engine.h"
#undef private
// White-box scheduler handlers: stop its input thread first, retain real storage,
// workers and callback FIFO; no production hooks or concurrent task mutation.
#include "../../core/engine/engine.cpp"
#include "check.h"
#include "tests/testutil.h"
#include <sqlite3.h>
using namespace immd;
static std::atomic<bool> fail_write{false};static std::atomic<int> failed_writes{0};
extern "C" int __real_sqlite3_step(sqlite3_stmt*);
extern "C" int __wrap_sqlite3_step(sqlite3_stmt* s){
 const char* sql=sqlite3_sql(s);if(fail_write&&sql&&std::string(sql).find("INSERT INTO transfer_task")!=std::string::npos){++failed_writes;return SQLITE_IOERR;}
 return __real_sqlite3_step(s);
}
struct CountPort:ControlPlanePort{
 mutable int calls=0;
 Expected<ControlRequest> encode(ControlOperation,const ControlRequest&)const override{++calls;return make_err(Err::Protocol,"unexpected post-checkpoint request");}
 Expected<ControlResponse> decode(ControlOperation,const ControlResponse& r)const override{return r;}
};
int main(){
 for(int mode=0;mode<17;++mode){
  auto dir=immd_test::make_temp_dir();Config cfg;cfg.base_url="http://127.0.0.1:1";cfg.data_dir=dir;cfg.max_active_tasks=0;
  auto port=std::make_shared<CountPort>();cfg.control_plane=port;
  std::mutex mu;std::vector<ProgressEvent> events;
  auto e=Engine::start(cfg,[&](const ProgressEvent& event){std::lock_guard<std::mutex> l(mu);events.push_back(event);});CHECK(e.ok());auto& im=*e.value()->impl_;
  im.q.close();im.sched.join(); // no scheduler access after this point
  TaskCtx ctx;ctx.rec.task_id="fixture";ctx.rec.size=1;ctx.rec.part_size=1;ctx.rec.nparts=1;ctx.rec.bitmap=PartBitmap(1);
  ctx.rec.staging_path=dir+"/staging/fixture.bin";ctx.rec.dest_path=dir+"/dest";std::ofstream(ctx.rec.staging_path)<<"x";
  ctx.rec.state=TaskState::Transferring;
  if(mode==10||mode==14||mode==15)ctx.rec.kind=TaskKind::Download;
  if(mode==0||mode==2)ctx.rec.state=TaskState::Queued;
  if(mode==1||mode==15||mode==16)ctx.rec.state=TaskState::Finalizing;
  if(mode==4)ctx.rec.state=TaskState::Failed;
  if(mode==7||mode==12)ctx.rec.state=TaskState::Preparing;
  if(mode==11){ctx.rec.streaming=true;ctx.rec.state=TaskState::Queued;}
  const auto before=ctx.rec;CHECK(!im.journal->save(before));im.tasks.emplace("fixture",std::move(ctx));auto& c=im.tasks.at("fixture");
  fail_write=mode!=16;failed_writes=0;
  switch(mode){
   case 0:CHECK(!im.to_state(c,TaskState::Preparing));break;
   case 1:im.finish_ready(c);break;
   case 2:im.on_cancel("fixture");break;
   case 3:im.on_pause("fixture");break;
   case 4:im.on_resume("fixture");break;
   case 5:{c.rec.streaming=true;EvStreamFlushed ev;ev.id="fixture";ev.generation=c.generation;ev.part=1;ev.len=1;im.on_stream_flushed(ev);break;}
   case 6:{EvStreamHashed ev;ev.id="fixture";ev.generation=c.generation;ev.sha="digest";im.on_stream_hashed(ev);break;}
   case 7:{EvPrepared ev;ev.id="fixture";ev.generation=c.generation;ev.size=1;ev.sha="digest";im.on_prepared(ev);break;}
   case 8:{HttpResponse r;r.status=200;r.body=R"({"result":"upload","upload_id":"u","part_size":1,"nparts":1,"received_parts":[0]})";im.on_init(c,r);break;}
   case 9:{HttpResponse r;r.status=200;im.on_put_part(c,0,r);break;}
   case 10:{HttpResponse r;r.status=206;r.bytes_down=1;im.on_get_part(c,0,r);break;}
   case 11:case 12:case 13:im.tasks.clear();im.on_recover();break;
   case 14:{HttpResponse r;r.status=200;r.body=R"({"url":"http://127.0.0.1:1/part","size":1})";im.on_dl_url(c,r);break;}
   case 15:{EvVerify ev;ev.id="fixture";ev.generation=c.generation;im.on_verify(ev);break;}
   case 16:im.finish_ready(c);break;
  }
  CHECK(port->calls==0);
  auto stored=im.journal->load("fixture");CHECK(stored.ok());
  if(mode==16){CHECK(failed_writes==0);CHECK(stored.value().state==TaskState::Ready);CHECK(!file_exists(before.staging_path));CHECK(im.tasks.empty());}
  else{
   CHECK(failed_writes==1);CHECK(stored.value().state==before.state);CHECK(im.tasks.at("fixture").rec.state==TaskState::Failed);CHECK(im.tasks.at("fixture").last_code==Err::Io);
   CHECK(file_exists(mode==15?before.dest_path:before.staging_path));
  }
  fail_write=false;e.value().reset();
  CHECK(events.size()==1);CHECK(events[0].journal_receipt==JournalReceipt::None);
  CHECK(events[0].state==(mode==16?TaskState::Ready:TaskState::Failed));
  if(mode!=16){CHECK(events[0].error==Err::Io);CHECK(events[0].message=="Io: journal checkpoint failed");}
 }
 // Public asynchronous path: Failed is runtime-only, a manual resume can retry
 // after storage recovery, and no automatic checkpoint retry storm occurs.
 auto dir=immd_test::make_temp_dir();Config cfg;cfg.base_url="http://127.0.0.1:1";cfg.data_dir=dir;cfg.max_active_tasks=0;
 auto file=dir+"/source";std::ofstream(file)<<"x";std::mutex mu;std::vector<ProgressEvent> events;
 auto e=Engine::start(cfg,[&](const ProgressEvent& ev){std::lock_guard<std::mutex> l(mu);events.push_back(ev);});CHECK(e.ok());
 auto id=e.value()->upload({file,"x",Priority::P4_File,"public_task"});CHECK(id.ok());CHECK(e.value()->query(id.value()).ok());
 fail_write=true;failed_writes=0;CHECK(!e.value()->cancel(id.value()));
 CHECK(e.value()->query(id.value()).value().state==TaskState::Failed);CHECK(e.value()->query_persisted(id.value()).value().state==TaskState::Queued);CHECK(failed_writes==1);
 fail_write=false;CHECK(!e.value()->resume(id.value()));CHECK(e.value()->query_persisted(id.value()).value().state==TaskState::Queued);e.value().reset();
 CHECK(events.size()==3);CHECK(events[0].journal_receipt==JournalReceipt::Normal);CHECK(events[1].state==TaskState::Failed&&events[1].error==Err::Io);CHECK(events[1].journal_receipt==JournalReceipt::None);CHECK(events[2].state==TaskState::Queued);

}
