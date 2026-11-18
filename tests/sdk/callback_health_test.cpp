#include <madmedia/madmedia.h>
#include "core/engine/callback_fault.h"
#include "check.h"
#include "tests/testutil.h"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <future>
#include <thread>
#include <pthread.h>
#include <dlfcn.h>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>
using namespace immd;using namespace immd_test;
static std::atomic<bool> track_join{false},callback_join_started{false};
static pthread_t callback_thread_id{};
extern "C" int pthread_join(pthread_t thread,void** result){
 using Fn=int(*)(pthread_t,void**);static auto real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"pthread_join"));CHECK(real);
 if(track_join.load(std::memory_order_acquire)&&pthread_equal(thread,callback_thread_id))callback_join_started.store(true,std::memory_order_release);
 return real(thread,result);
}
static Config config(const std::string& d){Config c;c.data_dir=d;c.base_url="http://127.0.0.1:1";c.max_active_tasks=0;return c;}
static CallbackHealth await_faults(Engine& e,uint64_t count){for(int i=0;i<5000;++i){auto h=e.callback_health();if(h.faulted_invocations>=count)return h;std::this_thread::sleep_for(std::chrono::milliseconds(1));}CHECK(false);return {};}
int main(){
 // Saturation never loses newly observed source or terminal/receipt bits.
 auto max=detail::callback_fault_count_max;auto packed=detail::merge_callback_faults((max-1)<<4,2,false,false);CHECK(packed>>4==max);packed=detail::merge_callback_faults(packed,1,true,true);CHECK((packed>>4)==max&&(packed&15)==15);CHECK(detail::merge_callback_faults(packed,0,false,false)==packed);
 // Nested TLS contexts restore their parent; reporting outside a context is inert.
 detail::report_callback_fault(false);{detail::CallbackFaultScope outer;detail::report_callback_fault(true);{detail::CallbackFaultScope inner;detail::report_callback_fault(false);CHECK(inner.flags()==1);}CHECK(outer.flags()==2);detail::report_callback_fault(false);CHECK(outer.flags()==3);}
 int direct=0;auto direct_cb=madmedia::observe([&](const auto&){++direct;throw 123;},[](const auto&){throw 456;});direct_cb({});CHECK(direct==1);
 auto logdir=make_temp_dir();auto logpath=logdir+"/captured.log";int logfd=open(logpath.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(logfd>=0);int original=dup(STDERR_FILENO);CHECK(original>=0&&dup2(logfd,STDERR_FILENO)>=0);close(logfd);
 for(int mode=0;mode<4;++mode){
  auto dir=make_temp_dir();auto input=dir+"/input";std::ofstream(input)<<"payload";Engine* ptr=nullptr;std::atomic<int> business_calls{0};std::atomic<bool> initial_health_clean{false};
  auto callback=[&](const ProgressEvent& ev){auto h=ptr->callback_health();if(business_calls.fetch_add(1)==0)initial_health_clean=h.faulted_invocations==0;auto q=ptr->query(ev.task_id);CHECK(!q.ok()&&q.error().code==Err::State);if(mode!=1)throw std::runtime_error("DO_NOT_LOG_EXCEPTION_SECRET");};
  auto observer=[&](const madmedia::Observation&){if(mode==1||mode==2)throw std::runtime_error("DO_NOT_LOG_OBSERVER_SECRET");};
  auto e= mode==3?Engine::start(config(dir),callback):madmedia::start(config(dir),callback,observer);CHECK(e.ok());ptr=e.value().get();CHECK(ptr->callback_health().faulted_invocations==0);
  UploadRequest r;r.file_path=input;r.task_id="fault-test";CHECK(ptr->upload(r).ok());auto first=await_faults(*ptr,1);CHECK(first.faulted_invocations==1&&initial_health_clean);CHECK(first.callback_fault==(mode!=1));CHECK(first.observer_fault==(mode==1||mode==2));CHECK(first.receipt_callback_fault==(mode!=1));CHECK(!first.terminal_callback_fault);
  CHECK(!ptr->cancel(r.task_id));auto terminal=await_faults(*ptr,2);CHECK(terminal.faulted_invocations==2&&terminal.terminal_callback_fault==(mode!=1));CHECK(business_calls==2);auto persisted=ptr->query_persisted(r.task_id);CHECK(persisted.ok()&&persisted.value().state==TaskState::Canceled);e.value().reset();
 }
 std::fflush(stderr);CHECK(dup2(original,STDERR_FILENO)>=0);close(original);std::ifstream logs(logpath);std::string text((std::istreambuf_iterator<char>(logs)),{});CHECK(text.find("DO_NOT_LOG")==std::string::npos);size_t lines=0,pos=0;while((pos=text.find("host callback fault; inspect callback health",pos))!=std::string::npos){++lines;++pos;}CHECK(lines==4);
 // Independent callback threads cannot attribute faults to another Engine.
 auto d1=make_temp_dir(),d2=make_temp_dir();std::ofstream(d1+"/in")<<"1";std::ofstream(d2+"/in")<<"2";
 auto a=madmedia::start(config(d1),[](const auto&){throw 1;});auto b=madmedia::start(config(d2),[](const auto&){},[](const auto&){throw 2;});CHECK(a.ok()&&b.ok());UploadRequest r1;r1.file_path=d1+"/in";UploadRequest r2;r2.file_path=d2+"/in";auto t1=std::async(std::launch::async,[&]{return a.value()->upload(r1);});auto t2=std::async(std::launch::async,[&]{return b.value()->upload(r2);});CHECK(t1.get().ok()&&t2.get().ok());auto h1=await_faults(*a.value(),1),h2=await_faults(*b.value(),1);CHECK(h1.callback_fault&&!h1.observer_fault&&!h2.callback_fault&&h2.observer_fault);a.value().reset();b.value().reset();
 // Health is readable while a host callback is blocked; close waits for return.
 auto blocked_dir=make_temp_dir();std::ofstream(blocked_dir+"/in")<<"x";std::mutex mu;std::condition_variable cv;bool entered=false,release=false;Engine* blocked_ptr=nullptr;std::atomic<bool> read_while_closing{false};std::atomic<int> blocked_calls{0};
 auto blocked=Engine::start(config(blocked_dir),[&](const auto&){if(blocked_calls.fetch_add(1)==0)throw 1;std::unique_lock<std::mutex> l(mu);callback_thread_id=pthread_self();entered=true;cv.notify_all();cv.wait(l,[&]{return release;});auto health=blocked_ptr->callback_health();CHECK(health.faulted_invocations==1);read_while_closing=true;throw 1;});CHECK(blocked.ok());blocked_ptr=blocked.value().get();UploadRequest br;br.file_path=blocked_dir+"/in";auto blocked_id=blocked.value()->upload(br);CHECK(blocked_id.ok());await_faults(*blocked.value(),1);CHECK(!blocked.value()->pause(blocked_id.value()));{std::unique_lock<std::mutex> l(mu);CHECK(cv.wait_for(l,std::chrono::seconds(5),[&]{return entered;}));}CHECK(blocked.value()->callback_health().faulted_invocations==1);
 track_join.store(true,std::memory_order_release);auto close=std::async(std::launch::async,[&]{blocked.value().reset();});for(int i=0;i<5000&&!callback_join_started.load(std::memory_order_acquire);++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));CHECK(callback_join_started.load(std::memory_order_acquire));CHECK(close.wait_for(std::chrono::milliseconds(1))==std::future_status::timeout);{std::lock_guard<std::mutex> l(mu);release=true;}cv.notify_all();CHECK(close.wait_for(std::chrono::seconds(5))==std::future_status::ready);close.get();track_join=false;CHECK(read_while_closing);
}
