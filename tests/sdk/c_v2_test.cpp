// White-box receive saturation + public v2 end-to-end, without a remote service.
#include "../../ffi/madmedia_c.cpp"
#include "check.h"
#include "tests/testutil.h"
#include <sqlite3.h>
#include <fstream>
#include <thread>
#include <chrono>
static void wait_terminal(madmedia_handle h){
 auto s=lookup(h);CHECK(s);
 for(int i=0;i<5000;++i){
  {std::lock_guard<std::mutex> l(s->events);if(s->slots[0].terminal)return;}
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
 }CHECK(false);
}
static std::string take(madmedia_handle h){
 size_t size=0;CHECK(madmedia_poll(h,nullptr,0,&size)==MADMEDIA_BUFFER_TOO_SMALL);
 char tiny[1];CHECK(madmedia_poll(h,tiny,1,&size)==MADMEDIA_BUFFER_TOO_SMALL);
 std::vector<char> out(size);CHECK(madmedia_poll(h,out.data(),out.size(),&size)==MADMEDIA_OK);return out.data();
}
int main(){
 CHECK(madmedia_abi_version()==1);CHECK(MADMEDIA_ABI_VERSION==1);
 auto dir=immd_test::make_temp_dir();std::string file=dir+"/source";std::ofstream(file)<<"fixture";
 madmedia_config_v2 cfg{sizeof(cfg),2,"http://127.0.0.1:1",dir.c_str(),1,0,MADMEDIA_JOURNAL_FULL,0};
 madmedia_handle h=42;
 auto bad=cfg;bad.struct_size=sizeof(cfg)-1;CHECK(madmedia_create_v2(&bad,&h)==MADMEDIA_INVALID&&h==0);
 bad=cfg;bad.abi_version=1;CHECK(madmedia_create_v2(&bad,&h)==MADMEDIA_INVALID);
 bad=cfg;bad.reserved=1;CHECK(madmedia_create_v2(&bad,&h)==MADMEDIA_INVALID);
 bad=cfg;bad.reserved2=1;CHECK(madmedia_create_v2(&bad,&h)==MADMEDIA_INVALID);
 bad=cfg;bad.journal_mode=2;CHECK(madmedia_create_v2(&bad,&h)==MADMEDIA_INVALID);
 CHECK(madmedia_create_v2(&cfg,&h)==MADMEDIA_OK);
 char id[MADMEDIA_TASK_ID_CAPACITY],other[MADMEDIA_TASK_ID_CAPACITY];
 CHECK(madmedia_upload(h,file.c_str(),"x",id,1)==MADMEDIA_BUFFER_TOO_SMALL);
 CHECK(madmedia_upload(h,file.c_str(),"x",id,sizeof(id))==MADMEDIA_OK);
 CHECK(madmedia_upload(h,file.c_str(),"x",other,sizeof(other))==MADMEDIA_BUSY);
 CHECK(madmedia_cancel(h,id)==MADMEDIA_OK);wait_terminal(h);
 auto output=take(h);CHECK(output.find("\"journal_receipt\":2")!=std::string::npos);
 CHECK(madmedia_destroy(h)==MADMEDIA_OK);size_t size=99;CHECK(madmedia_poll(h,nullptr,0,&size)==MADMEDIA_CLOSED&&size==0);
 // Failed journal insert followed by reuse must not retain the old Failed receipt.
 cfg.journal_mode=MADMEDIA_JOURNAL_NORMAL;CHECK(madmedia_create_v2(&cfg,&h)==MADMEDIA_OK);
 sqlite3* db=nullptr;CHECK(sqlite3_open((dir+"/journal.db").c_str(),&db)==SQLITE_OK);
 CHECK(sqlite3_exec(db,"CREATE TRIGGER reject_insert BEFORE INSERT ON transfer_task BEGIN SELECT RAISE(ABORT,'fixture'); END;",nullptr,nullptr,nullptr)==SQLITE_OK);
 CHECK(madmedia_upload(h,file.c_str(),"x",id,sizeof(id))==MADMEDIA_OK);wait_terminal(h);
 output=take(h);CHECK(output.find("\"journal_receipt\":3")!=std::string::npos);
 CHECK(sqlite3_exec(db,"DROP TRIGGER reject_insert",nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(db);
 CHECK(madmedia_upload(h,file.c_str(),"x",id,sizeof(id))==MADMEDIA_OK);CHECK(madmedia_cancel(h,id)==MADMEDIA_OK);wait_terminal(h);
 output=take(h);CHECK(output.find("\"journal_receipt\":1")!=std::string::npos);CHECK(madmedia_destroy(h)==MADMEDIA_OK);
 // Confirm old API still omits the new JSON field.
 madmedia_config_v1 old{sizeof(old),1,cfg.base_url,cfg.data_dir,1,0};CHECK(madmedia_create(&old,&h)==MADMEDIA_OK);
 CHECK(madmedia_upload(h,file.c_str(),"x",id,sizeof(id))==MADMEDIA_OK);CHECK(madmedia_cancel(h,id)==MADMEDIA_OK);wait_terminal(h);
 CHECK(take(h).find("journal_receipt")==std::string::npos);CHECK(madmedia_destroy(h)==MADMEDIA_OK);
 // Deterministic 10k coalescing, first terminal pin, short buffer preservation.
 auto state=std::make_shared<State>();state->id=999;state->receipt_json=true;state->slots.resize(1);
 auto& slot=state->slots[0];slot.used=true;slot.id="task";
 {std::lock_guard<std::mutex> l(registry_mutex);registry.emplace(999,state);}
 immd::ProgressEvent event;event.task_id="task";event.journal_receipt=immd::JournalReceipt::Full;receive(state,event);
 CHECK(take(999).find("\"journal_receipt\":2")!=std::string::npos);CHECK(slot.used);
 event.journal_receipt=immd::JournalReceipt::None;event.state=immd::TaskState::Preparing;
 for(int i=0;i<10000;++i){event.bytes_done=i;receive(state,event);}
 CHECK(slot.event.journal_receipt==immd::JournalReceipt::Full&&slot.event.bytes_done==9999);
 event.state=immd::TaskState::Canceled;receive(state,event);event.state=immd::TaskState::Ready;receive(state,event);
 CHECK(take(999).find("\"journal_receipt\":2")!=std::string::npos);CHECK(!slot.used);CHECK(madmedia_destroy(999)==MADMEDIA_OK);
 // Conflicting receipts fail closed, while repeated identical receipts are harmless.
 state=std::make_shared<State>();state->id=999;state->receipt_json=true;state->slots.resize(1);state->slots[0].used=true;state->slots[0].id="task";
 {std::lock_guard<std::mutex> l(registry_mutex);registry.emplace(999,state);}
 event.state=immd::TaskState::Queued;event.journal_receipt=immd::JournalReceipt::Full;receive(state,event);receive(state,event);CHECK(!state->fault);
 event.journal_receipt=immd::JournalReceipt::Failed;receive(state,event);CHECK(state->fault);CHECK(madmedia_poll(999,nullptr,0,&size)==MADMEDIA_INTERNAL);CHECK(madmedia_destroy(999)==MADMEDIA_OK);
 // Concurrent poll/destroy never accesses a destroyed native engine.
 CHECK(madmedia_create_v2(&cfg,&h)==MADMEDIA_OK);std::atomic<bool> closed{false};
 std::thread poller([&]{for(;;){size_t n;auto rc=madmedia_poll(h,nullptr,0,&n);if(rc==MADMEDIA_CLOSED){closed=true;return;}CHECK(rc==MADMEDIA_EMPTY);}});
 CHECK(madmedia_destroy(h)==MADMEDIA_OK);poller.join();CHECK(closed);
}
