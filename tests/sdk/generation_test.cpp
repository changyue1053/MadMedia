#include <functional>
#include <memory>
#include <string>
#include "core/journal/journal.h"
#define private public
#include "core/engine/engine.h"
#undef private
#include "../../core/engine/engine.cpp"
#include "../../ffi/madmedia_c.cpp"
#include <sqlite3.h>
#include <cstdarg>
#include "check.h"
#include "tests/testutil.h"
#include <condition_variable>
#include <sys/socket.h>
#include <netinet/in.h>
using namespace immd;
static std::atomic<bool> gate_write{false};static std::mutex gate_mu;static std::condition_variable gate_cv;static bool write_entered=false,write_release=false;
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t size,off_t offset){
 if(gate_write.exchange(false)){
  std::unique_lock<std::mutex> l(gate_mu);write_entered=true;gate_cv.notify_all();gate_cv.wait(l,[]{return write_release;});
 }
 return __real_pwrite(fd,data,size,offset);
}
static std::atomic<bool> gate_open{false}, fail_checkpoint{false};
static std::mutex open_mu;static std::condition_variable open_cv;static bool open_entered=false,open_release=false;
extern "C" int __real_open(const char*,int,...);
extern "C" int __wrap_open(const char* path,int flags,...){
 mode_t mode=0;if(flags&O_CREAT){va_list ap;va_start(ap,flags);mode=va_arg(ap,int);va_end(ap);}
 if((flags&O_CREAT)&&std::string(path).find("/staging/")!=std::string::npos&&gate_open.exchange(false)){
  std::unique_lock<std::mutex> l(open_mu);open_entered=true;open_cv.notify_all();open_cv.wait(l,[]{return open_release;});
 }
 return __real_open(path,flags,mode);
}
extern "C" int __real_sqlite3_step(sqlite3_stmt*);
extern "C" int __wrap_sqlite3_step(sqlite3_stmt* st){
 const char* sql=sqlite3_sql(st);if(fail_checkpoint&&sql&&std::string(sql).find("INSERT INTO transfer_task")!=std::string::npos)return SQLITE_IOERR;
 return __real_sqlite3_step(st);
}
static void arm_open(){std::lock_guard<std::mutex> l(open_mu);open_entered=false;open_release=false;gate_open=true;}
static void wait_open(){std::unique_lock<std::mutex> l(open_mu);CHECK(open_cv.wait_for(l,std::chrono::seconds(5),[]{return open_entered;}));}
static void release_open(){{std::lock_guard<std::mutex> l(open_mu);open_release=true;}open_cv.notify_all();}
struct CountPort:ControlPlanePort{
 mutable int calls=0;
 Expected<ControlRequest> encode(ControlOperation,const ControlRequest&)const override{++calls;return make_err(Err::Protocol,"unexpected stale dispatch");}
 Expected<ControlResponse> decode(ControlOperation,const ControlResponse& r)const override{return r;}
};
static void wait_done(Engine::Impl& im,TaskCtx& c){for(int i=0;i<5000;++i){if(im.quiescent(c))return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}CHECK(false);}
static std::string contents(const std::string& p){std::ifstream in(p);return std::string(std::istreambuf_iterator<char>(in),{});}
struct Fixture{
 std::string dir=immd_test::make_temp_dir();Config cfg;std::shared_ptr<CountPort> port=std::make_shared<CountPort>();
 std::mutex mu;std::vector<ProgressEvent> events;std::unique_ptr<Engine> engine;
 Fixture(){cfg.data_dir=dir;cfg.base_url="http://127.0.0.1:1";cfg.max_active_tasks=0;cfg.control_plane=port;
  auto e=Engine::start(cfg,[this](const ProgressEvent& p){std::lock_guard<std::mutex> l(mu);events.push_back(p);});CHECK(e.ok());engine=std::move(e.value());engine->impl_->q.close();engine->impl_->sched.join();}
 Engine::Impl& im(){return *engine->impl_;}
 TaskCtx& task(){return im().tasks.at("task");}
 void seed(TaskState state=TaskState::Preparing){TaskCtx c;c.rec.task_id="task";c.rec.state=state;c.rec.staging_path=dir+"/staging/task.bin";c.rec.orig_path=dir+"/source";c.rec.dest_path=dir+"/destination";c.rec.size=3;c.rec.part_size=3;c.rec.nparts=1;c.rec.bitmap=PartBitmap(1);std::ofstream(c.rec.orig_path)<<"new";CHECK(!im().journal->save(c.rec));im().tasks.emplace("task",std::move(c));}
 ~Fixture(){engine.reset();}
};
int main(){
 // Every task-producing compute event and every HTTP operation drops old epochs
 // BEFORE mutating current attempt flags, IDs, timers, files or records.
 {
  Fixture f;f.seed();auto& c=f.task();c.generation=2;c.compute_job=777;c.ctrl_inflight=true;c.url_fetch_inflight=true;c.stream_flush_inflight=true;c.stream_hash_inflight=true;c.stream_buf={1,2};c.http_ids[0]=888;c.inflight.insert(0);c.ctrl_retry_at=123;c.retry_at[0]=456;
  EvPrepared prepared;prepared.id="task";prepared.generation=1;prepared.err=make_err(Err::Io,"old");f.im().on_prepared(prepared);
  EvProof proof;proof.id="task";proof.generation=1;f.im().on_proof(proof);
  EvVerify verify;verify.id="task";verify.generation=1;f.im().on_verify(verify);
  EvCacheHit cache;cache.id="task";cache.generation=1;cache.err=make_err(Err::NotFound,"old");f.im().on_cache_hit(cache);
  EvStreamFlushed flushed;flushed.id="task";flushed.generation=1;flushed.len=2;f.im().on_stream_flushed(flushed);
  EvStreamHashed hashed;hashed.id="task";hashed.generation=1;hashed.sha="old";f.im().on_stream_hashed(hashed);
  for(int op=0;op<8;++op){EvHttp http;http.id="task";http.generation=1;http.op=static_cast<Op>(op);http.resp.status=200;http.resp.body="old";f.im().on_http(http);}
  CHECK(c.compute_job==777&&c.ctrl_inflight&&c.url_fetch_inflight&&c.stream_flush_inflight&&c.stream_hash_inflight);
  CHECK(c.rec.state==TaskState::Preparing&&c.rec.sha256_hex.empty()&&c.stream_buf.size()==2&&c.http_ids.at(0)==888&&c.inflight.count(0));
  CHECK(c.ctrl_retry_at==123&&c.retry_at.at(0)==456&&f.port->calls==0);CHECK(!file_exists(c.rec.dest_path));
  // Repeated recover does not reset a live context's epoch.
  f.im().on_recover();CHECK(c.generation==2);
  c.generation=std::numeric_limits<uint64_t>::max();f.im().on_pause("task");CHECK(c.generation==0);f.im().on_resume("task");CHECK(c.rec.state==TaskState::Failed&&c.last_code==Err::State&&!c.resume_requested);
 }
 // Pause also withdraws a deferred resume from Failed, not just Paused.
 {
  Fixture f;f.seed(TaskState::Failed);auto& c=f.task();auto held=f.im().track_work(c);
  f.im().on_resume("task");CHECK(c.resume_requested&&c.rec.state==TaskState::Failed);
  f.im().on_pause("task");CHECK(!c.resume_requested&&c.rec.state==TaskState::Failed);
  held->done.store(true);f.im().pump();CHECK(c.rec.state==TaskState::Failed);
 }
 // Streaming re-init preserves local extent; conflicting server session,
 // part geometry or received indices must fail closed without rewriting parts.
 for(int conflict=0;conflict<4;++conflict){
  Fixture f;f.seed(TaskState::Preparing);auto& c=f.task();c.rec.streaming=true;c.rec.nparts=2;c.rec.size=6;c.rec.part_size=3;c.rec.upload_id="same";c.rec.bitmap=PartBitmap(2);
  HttpResponse r;r.status=200;r.body=std::string("{\"result\":\"upload\",\"upload_id\":\"")+(conflict==1?"changed":"same")+"\",\"part_size\":"+(conflict==2?"4":"3")+",\"nparts\":0,\"received_parts\":["+(conflict==3?"2":"0")+"]}";
  f.im().on_init(c,r);CHECK(c.rec.nparts==2&&c.rec.size==6&&c.rec.part_size==3);
  if(conflict)CHECK(c.rec.state==TaskState::Failed&&c.last_code==Err::Protocol);else CHECK(c.rec.state==TaskState::Transferring&&c.rec.bitmap.get(0)&&!c.rec.bitmap.get(1));
 }
 // A genuinely delayed compute write cannot overlap resumed preparation.
 {
  Fixture f;f.seed();auto& c=f.task();std::mutex mu;std::condition_variable cv;bool entered=false,release=false;
  const auto old_generation=c.generation;const auto path=c.rec.staging_path;
  c.compute_job=f.im().submit_compute(c,[&](const std::atomic<bool>&){
   {std::unique_lock<std::mutex> l(mu);entered=true;cv.notify_all();cv.wait(l,[&]{return release;});}
   std::ofstream(path)<<"old"; // intentionally ignores cancel until real I/O completes
  });
  {std::unique_lock<std::mutex> l(mu);CHECK(cv.wait_for(l,std::chrono::seconds(5),[&]{return entered;}));}
  c.ctrl_retry_at=f.im().now()-1;c.retry_at[0]=f.im().now()-1;f.im().on_pause("task");f.im().on_resume("task");
  CHECK(c.resume_requested&&c.rec.state==TaskState::Paused&&c.ctrl_retry_at==0&&c.retry_at.empty());
  f.im().cfg.max_active_tasks=1;auto waiting_generation=c.generation;for(int i=0;i<100;++i)f.im().pump();CHECK(c.generation==waiting_generation&&c.work.size()==1);CHECK(c.resume_requested&&c.rec.state==TaskState::Paused&&f.port->calls==0);CHECK(!file_exists(path));
  // A second pause withdraws deferred resume without spinning or starting work.
  f.im().on_pause("task");CHECK(!c.resume_requested);f.im().on_resume("task");CHECK(c.resume_requested);
  {std::lock_guard<std::mutex> l(mu);release=true;}cv.notify_all();wait_done(f.im(),c);CHECK(contents(path)=="old");
  f.im().pump();CHECK(!c.resume_requested&&c.rec.state==TaskState::Preparing);wait_done(f.im(),c);CHECK(contents(path)=="new");
  auto current_job=c.compute_job;EvPrepared late;late.id="task";late.generation=old_generation;late.sha="old-sha";f.im().on_prepared(late);CHECK(c.compute_job==current_job&&c.rec.sha256_hex.empty());
  f.engine.reset();bool saw_wait=false,saw_resume=false;for(auto& ev:f.events){saw_wait|=ev.resume_waiting;saw_resume|=ev.state==TaskState::Queued&&!ev.resume_waiting;}CHECK(saw_wait&&saw_resume);
 }
 // Real HTTP file write blocked inside pwrite: cancel cannot complete until the
 // transport callback returns; resumed attempt must not write the same path yet.
 for(int cancel_mode=0;cancel_mode<2;++cancel_mode){
  int server=socket(AF_INET,SOCK_STREAM,0);CHECK(server>=0);sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);CHECK(bind(server,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);CHECK(listen(server,2)==0);socklen_t alen=sizeof(addr);CHECK(getsockname(server,reinterpret_cast<sockaddr*>(&addr),&alen)==0);
  std::thread responder([&]{for(int request_index=0;request_index<(cancel_mode?1:2);++request_index){const char* body=request_index?"new":"old";int fd=accept(server,nullptr,nullptr);CHECK(fd>=0);std::string request;char bytes[512];while(request.find("\r\n\r\n")==std::string::npos){auto n=recv(fd,bytes,sizeof(bytes),0);CHECK(n>0);request.append(bytes,size_t(n));}std::string response="HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\n"+std::string(body);size_t sent=0;while(sent<response.size()){auto n=send(fd,response.data()+sent,response.size()-sent,MSG_NOSIGNAL);CHECK(n>0);sent+=size_t(n);}close(fd);}close(server);});
  Fixture f;f.seed(TaskState::Transferring);auto& c=f.task();c.rec.kind=TaskKind::Download;
  HttpRequest req;req.url="http://127.0.0.1:"+std::to_string(ntohs(addr.sin_port))+"/file";req.download_path=c.rec.staging_path;req.timeout_ms=5000;
  {std::lock_guard<std::mutex> l(gate_mu);write_entered=false;write_release=false;}
  gate_write=true;f.im().post_http("task",Op::GetPart,0,req);const auto old_generation=c.generation;
  {std::unique_lock<std::mutex> l(gate_mu);CHECK(gate_cv.wait_for(l,std::chrono::seconds(5),[]{return write_entered;}));}
  if(cancel_mode){
   // A control request queued behind blocked data I/O must be tracked too.
   f.im().cfg.control_plane.reset();HttpRequest control;control.url=req.url;control.method="POST";control.body="{}";control.timeout_ms=5000;f.im().post_http("task",Op::Init,0,control);CHECK(c.work.size()==2);
   const auto path=c.rec.staging_path;f.im().on_cancel("task");f.im().on_cancel("task");f.im().pump();CHECK(c.cancel_requested&&!f.im().tasks.empty());
   {std::lock_guard<std::mutex> l(gate_mu);write_release=true;}gate_cv.notify_all();wait_done(f.im(),c);f.im().pump();CHECK(f.im().tasks.empty()&&!file_exists(path));CHECK(f.im().journal->load("task").value().state==TaskState::Canceled);f.engine.reset();int terminals=0;for(const auto& ev:f.events)terminals+=ev.state==TaskState::Canceled;CHECK(terminals==1);responder.join();continue;
  }
  f.im().on_pause("task");f.im().on_resume("task");f.im().pump();CHECK(c.resume_requested&&c.rec.state==TaskState::Paused&&!f.im().quiescent(c));
  {std::lock_guard<std::mutex> l(gate_mu);write_release=true;}gate_cv.notify_all();wait_done(f.im(),c);CHECK(contents(c.rec.staging_path)=="old");
  f.im().pump();CHECK(c.rec.state==TaskState::Queued&&!c.resume_requested);c.rec.state=TaskState::Transferring;f.im().post_http("task",Op::GetPart,0,req);wait_done(f.im(),c);CHECK(contents(c.rec.staging_path)=="new");
  EvHttp late;late.id="task";late.generation=old_generation;late.op=Op::GetPart;late.resp.status=200;late.resp.bytes_down=3;f.im().on_http(late);CHECK(!c.rec.bitmap.get(0));responder.join();
 }
 // Stream bytes remain buffered through canceled flush and are removed only
 // after the current epoch's successful metadata checkpoint.
 {
  Fixture f;f.seed(TaskState::Transferring);auto& c=f.task();c.rec.streaming=true;c.rec.nparts=0;c.rec.size=0;c.rec.bitmap=PartBitmap(0);c.stream_buf={'a','b','c'};c.generation=3;c.stream_flush_inflight=true;
  EvStreamFlushed old;old.id="task";old.generation=2;old.len=3;f.im().on_stream_flushed(old);CHECK(c.stream_buf.size()==3&&c.rec.nparts==0&&c.stream_flush_inflight);
  auto current=old;current.generation=3;current.part=0;f.im().on_stream_flushed(current);CHECK(c.stream_buf.empty()&&c.rec.nparts==1&&c.rec.size==3);
 }
 // Close drains retained work without holding scheduler/event locks. It cannot
 // force a stuck syscall to finish, but must complete once that work returns.
 {
  Fixture f;f.seed();auto& c=f.task();std::mutex mu;std::condition_variable cv;bool entered=false,release=false;std::atomic<bool> closing{false},closed{false};
  c.compute_job=f.im().submit_compute(c,[&](const std::atomic<bool>&){std::unique_lock<std::mutex> l(mu);entered=true;cv.notify_all();cv.wait(l,[&]{return release;});});
  {std::unique_lock<std::mutex> l(mu);CHECK(cv.wait_for(l,std::chrono::seconds(5),[&]{return entered;}));}
  f.im().on_pause("task");f.im().on_resume("task");
  std::thread closer([&]{closing=true;f.engine.reset();closed=true;});while(!closing)std::this_thread::yield();std::this_thread::sleep_for(std::chrono::milliseconds(10));CHECK(!closed);
  {std::lock_guard<std::mutex> l(mu);release=true;}cv.notify_all();closer.join();CHECK(closed);
 }

 // Two-phase Cancel: reproduce the audited late O_CREAT stream writer, but
 // retain context/files until it finishes; no premature terminal/cleanup.
 for(int close_pending=0;close_pending<2;++close_pending){
  Fixture f;f.seed(TaskState::Transferring);auto& c=f.task();c.rec.streaming=true;c.rec.nparts=0;c.rec.size=0;c.rec.bitmap=PartBitmap(0);c.stream_buf={'a','b','c'};const auto path=c.rec.staging_path;std::ofstream(path)<<"prior";
  arm_open();f.im().maybe_stream_flush(c);wait_open();f.im().on_cancel("task");auto epoch=c.generation;f.im().on_cancel("task");f.im().on_resume("task");f.im().on_pause("task");
  CHECK(c.cancel_requested&&c.generation==epoch&&!f.im().tasks.empty()&&file_exists(path));CHECK(f.im().journal->load("task").value().state==TaskState::Transferring);
  for(int i=0;i<100;++i)f.im().pump();CHECK(c.cancel_requested&&f.port->calls==0);
  if(close_pending){std::atomic<bool> closing{false},closed{false};std::thread closer([&]{closing=true;f.engine.reset();closed=true;});while(!closing)std::this_thread::yield();CHECK(!closed);release_open();closer.join();CHECK(closed);}
  else{release_open();wait_done(f.im(),c);f.im().pump();CHECK(f.im().tasks.empty());CHECK(f.im().journal->load("task").value().state==TaskState::Canceled);f.engine.reset();}
  CHECK(!file_exists(path));int terminals=0,waiting=0;for(const auto& ev:f.events){terminals+=ev.state==TaskState::Canceled;waiting+=ev.cancel_waiting;}CHECK(terminals==1&&waiting==1);
 }
 // Cancel checkpoint error retains files, clears pending intent, emits Failed;
 // cleanup failure is separately explicit Io on Canceled and never recursive.
 {
  Fixture f;f.seed();auto& c=f.task();const auto path=c.rec.staging_path;std::ofstream(path)<<"evidence";fail_checkpoint=true;f.im().on_cancel("task");fail_checkpoint=false;
  CHECK(c.rec.state==TaskState::Failed&&!c.cancel_requested&&file_exists(path));CHECK(f.im().journal->load("task").value().state==TaskState::Preparing);f.im().pump();CHECK(c.rec.state==TaskState::Failed&&file_exists(path));
 }
 {
  Fixture f;f.seed();auto& c=f.task();const auto path=c.rec.staging_path;CHECK(::mkdir(path.c_str(),0700)==0);f.im().on_cancel("task");CHECK(f.im().tasks.empty());f.engine.reset();CHECK(f.events.size()==1&&f.events[0].state==TaskState::Canceled&&f.events[0].error==Err::Io&&f.events[0].message=="cancel cleanup failed");CHECK(file_exists(path));
 }
 // Public C capacity is not released by a Cancel intent while native I/O can
 // still create staging. The pinned terminal appears only after quiescence.
 {
  auto dir=immd_test::make_temp_dir();auto source=dir+"/source";std::ofstream(source)<<"abc";madmedia_config_v2 cfg{sizeof(cfg),2,"http://127.0.0.1:1",dir.c_str(),1,0,MADMEDIA_JOURNAL_NORMAL,0};madmedia_handle h=0;CHECK(madmedia_create_v2(&cfg,&h)==MADMEDIA_OK);
  char id[MADMEDIA_TASK_ID_CAPACITY],second[MADMEDIA_TASK_ID_CAPACITY],buffer[8192];arm_open();CHECK(madmedia_upload(h,source.c_str(),"x",id,sizeof(id))==MADMEDIA_OK);wait_open();CHECK(madmedia_cancel(h,id)==MADMEDIA_OK);
  auto state=lookup(h);CHECK(state->engine->query(id).ok()); // FIFO barrier after Cancel command
  size_t n=0;auto status=madmedia_poll(h,buffer,sizeof(buffer),&n);CHECK(status==MADMEDIA_OK||status==MADMEDIA_EMPTY);if(status==MADMEDIA_OK)CHECK(std::string(buffer).find("\"terminal\":true")==std::string::npos);
  CHECK(madmedia_upload(h,source.c_str(),"x",second,sizeof(second))==MADMEDIA_BUSY);release_open();bool terminal_seen=false;
  for(int i=0;i<5000;++i){auto rc=madmedia_poll(h,buffer,sizeof(buffer),&n);CHECK(rc==MADMEDIA_OK||rc==MADMEDIA_EMPTY);if(rc==MADMEDIA_OK&&std::string(buffer).find("\"terminal\":true")!=std::string::npos){terminal_seen=true;CHECK(std::string(buffer).find("\"state\":7")!=std::string::npos);break;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  CHECK(terminal_seen);CHECK(!file_exists(dir+"/staging/"+id+".bin"));CHECK(madmedia_upload(h,source.c_str(),"x",second,sizeof(second))==MADMEDIA_OK);CHECK(madmedia_destroy(h)==MADMEDIA_OK);
 }

}
