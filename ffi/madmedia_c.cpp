#include <madmedia/madmedia_c.h>
#include <madmedia/madmedia.h>
#include "core/util/json.h"
#if !defined(__linux__)
#error "MadMedia C ABI entropy provider requires target-specific implementation and validation"
#endif
#include "platform/entropy.h"
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {
struct Slot {
 std::string id;
 immd::ProgressEvent event;
 bool used=false, pending=false, terminal=false;
};
struct State {
 std::mutex operations, events;
 std::unique_ptr<immd::Engine> engine;
 std::vector<Slot> slots;
 std::atomic<bool> fault{false};
 bool closed=false;
 size_t cursor=0;
 uint64_t id=0;
};
std::mutex registry_mutex;
std::map<madmedia_handle,std::shared_ptr<State>> registry;
uint64_t next_handle=1;
std::shared_ptr<State> lookup(madmedia_handle h) {
 std::lock_guard<std::mutex> lock(registry_mutex);
 auto it=registry.find(h);return it==registry.end()?nullptr:it->second;
}
bool terminal(immd::TaskState s) {
 return s==immd::TaskState::Ready || s==immd::TaskState::Failed || s==immd::TaskState::Canceled;
}
void receive(std::weak_ptr<State> weak,const immd::ProgressEvent& event) noexcept {
 auto state=weak.lock();if(!state)return;
 try {
  std::lock_guard<std::mutex> lock(state->events);
  for(auto& slot:state->slots) if(slot.used && slot.id==event.task_id) {
   if(slot.terminal)return; // retain first terminal, never overwrite with progress
   if(event.task_id.size()>=MADMEDIA_TASK_ID_CAPACITY || event.media_id.size()>4096) {
    state->fault.store(true);return;
   }
   slot.event.task_id=event.task_id;slot.event.media_id=event.media_id;
   slot.event.kind=event.kind;slot.event.state=event.state;slot.event.error=event.error;
   slot.event.bytes_done=event.bytes_done;slot.event.bytes_total=event.bytes_total;
   slot.pending=true;slot.terminal=terminal(event.state);return;
  }
 } catch(...) {state->fault.store(true);}
}
template<class F> madmedia_status guarded(F&& f) noexcept {
 try{return f();}catch(...){return MADMEDIA_INTERNAL;}
}
std::string json(const immd::ProgressEvent& e) {
 auto j=immd::Json::object();
 j.set("task_id",immd::Json::str(e.task_id));
 j.set("media_id",immd::Json::str(e.media_id));
 j.set("kind",immd::Json::number(static_cast<int>(e.kind)));
 j.set("state",immd::Json::number(static_cast<int>(e.state)));
 j.set("error",immd::Json::number(static_cast<int>(e.error)));
 j.set("terminal",immd::Json::boolean(terminal(e.state)));
 j.set("bytes_done",immd::Json::str(std::to_string(e.bytes_done)));
 j.set("bytes_total",immd::Json::str(std::to_string(e.bytes_total)));
 // Native error text omitted: may contain URLs, paths or credentials.
 return j.dump();
}
std::string secure_task_id() {
 std::array<unsigned char,16> bytes{};
 if(!madmedia_platform::entropy128(bytes))throw std::runtime_error("entropy unavailable");
 static const char hex[]="0123456789abcdef";
 std::string id="madmedia_";id.reserve(41);
 for(unsigned char b:bytes){id.push_back(hex[b>>4]);id.push_back(hex[b&15]);}
 return id;
}
template<class Submit> madmedia_status submit(madmedia_handle h,char* out,size_t cap,Submit fn) {
 if(!out || cap<MADMEDIA_TASK_ID_CAPACITY)return MADMEDIA_BUFFER_TOO_SMALL;
 out[0]='\0';
 auto state=lookup(h);if(!state)return MADMEDIA_CLOSED;
 std::lock_guard<std::mutex> op(state->operations);
 if(state->closed)return MADMEDIA_CLOSED;
 if(state->fault.load())return MADMEDIA_INTERNAL;
 std::lock_guard<std::mutex> events(state->events);
 auto it=std::find_if(state->slots.begin(),state->slots.end(),[](const Slot& s){return !s.used;});
 if(it==state->slots.end())return MADMEDIA_BUSY;
 const auto id=secure_task_id(); // no PRNG seed, counter or restart-dependent fallback
 it->id=id; it->used=true; it->pending=false; it->terminal=false;
 try {
  auto result=fn(*state->engine,id);
  if(!result.ok()){it->used=false;return MADMEDIA_NATIVE_ERROR;}
 } catch(...) {it->used=false;throw;}
 std::memcpy(out,id.c_str(),id.size()+1);
 return MADMEDIA_OK;
}
}
extern "C" {
uint32_t madmedia_abi_version(void){return MADMEDIA_ABI_VERSION;}
madmedia_status madmedia_create(const madmedia_config_v1* c,madmedia_handle* out){return guarded([&]{
 if(out)*out=0;
 if(!c||!out||c->struct_size<sizeof(*c)||c->abi_version!=1||c->reserved||
 !c->base_url||!c->data_dir||!*c->base_url||!*c->data_dir||c->max_pending_tasks<1||c->max_pending_tasks>256)return MADMEDIA_INVALID;
 auto state=std::make_shared<State>();state->slots.resize(c->max_pending_tasks);
 for(auto& slot:state->slots){slot.id.reserve(MADMEDIA_TASK_ID_CAPACITY);slot.event.task_id.reserve(MADMEDIA_TASK_ID_CAPACITY);slot.event.media_id.reserve(4096);}
 {std::lock_guard<std::mutex> lock(registry_mutex);
  if(next_handle==std::numeric_limits<uint64_t>::max())return MADMEDIA_INTERNAL;
  state->id=next_handle++;
 }
 immd::Config cfg;cfg.base_url=c->base_url;cfg.data_dir=c->data_dir;
 auto engine=immd::Engine::start(std::move(cfg),[weak=std::weak_ptr<State>(state)](const auto& e){receive(weak,e);});
 if(!engine.ok())return MADMEDIA_NATIVE_ERROR;
 state->engine=std::move(engine.value());
 {std::lock_guard<std::mutex> lock(registry_mutex);registry.emplace(state->id,state);}
 *out=state->id;return MADMEDIA_OK;
});}
madmedia_status madmedia_upload(madmedia_handle h,const char* path,const char* mime,char* out,size_t cap){return guarded([&]{
 if(!path||!*path||!mime)return MADMEDIA_INVALID;
 return submit(h,out,cap,[&](immd::Engine& e,const std::string& id){immd::UploadRequest r;r.file_path=path;r.mime=mime;r.task_id=id;return e.upload(std::move(r));});
});}
madmedia_status madmedia_download(madmedia_handle h,const char* media,const char* destination,char* out,size_t cap){return guarded([&]{
 if(!media||!*media||!destination||!*destination)return MADMEDIA_INVALID;
 return submit(h,out,cap,[&](immd::Engine& e,const std::string& id){immd::DownloadRequest r;r.media_id=media;r.dest_path=destination;r.task_id=id;return e.download(std::move(r));});
});}
madmedia_status madmedia_cancel(madmedia_handle h,const char* id){return guarded([&]{
 if(!id||!*id)return MADMEDIA_INVALID;
 auto s=lookup(h);if(!s)return MADMEDIA_CLOSED;
 std::lock_guard<std::mutex> op(s->operations);if(s->closed)return MADMEDIA_CLOSED;
 std::lock_guard<std::mutex> ev(s->events);
 auto found=std::find_if(s->slots.begin(),s->slots.end(),[&](const Slot& slot){return slot.used&&slot.id==id;});
 if(found==s->slots.end())return MADMEDIA_INVALID;
 if(found->terminal)return MADMEDIA_OK;
 return s->engine->cancel(id)?MADMEDIA_NATIVE_ERROR:MADMEDIA_OK;
});}
madmedia_status madmedia_poll(madmedia_handle h,char* buffer,size_t capacity,size_t* required){return guarded([&]{
 if(!required||(!buffer&&capacity))return MADMEDIA_INVALID;
 *required=0;auto s=lookup(h);if(!s)return MADMEDIA_CLOSED;
 std::lock_guard<std::mutex> op(s->operations);if(s->closed)return MADMEDIA_CLOSED;
 if(s->fault.load())return MADMEDIA_INTERNAL;
 std::lock_guard<std::mutex> lock(s->events);
 for(size_t n=0;n<s->slots.size();++n){size_t i=(s->cursor+n)%s->slots.size();auto& slot=s->slots[i];
  if(!slot.used||!slot.pending)continue;
  auto data=json(slot.event);*required=data.size()+1;
  if(!buffer||capacity<*required)return MADMEDIA_BUFFER_TOO_SMALL;
  std::memcpy(buffer,data.c_str(),*required);slot.pending=false;
  if(slot.terminal)slot.used=false;
  s->cursor=(i+1)%s->slots.size();return MADMEDIA_OK;
 }
 return MADMEDIA_EMPTY;
});}
madmedia_status madmedia_destroy(madmedia_handle h){return guarded([&]{
 std::shared_ptr<State> state;
 {std::lock_guard<std::mutex> lock(registry_mutex);auto it=registry.find(h);if(it==registry.end())return MADMEDIA_CLOSED;state=it->second;registry.erase(it);}
 std::lock_guard<std::mutex> op(state->operations);state->closed=true;
 state->engine.reset();return MADMEDIA_OK;
});}
}
