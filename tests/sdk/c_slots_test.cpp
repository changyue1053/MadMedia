// White-box bounded-slot invariants; compile implementation once in this test.
#include "../../ffi/madmedia_c.cpp"
#include "check.h"
int main(){
 auto state=std::make_shared<State>();state->slots.resize(1);state->id=999;
 auto& slot=state->slots[0];slot.id="task";slot.used=true;
 {std::lock_guard<std::mutex> lock(registry_mutex);registry.emplace(state->id,state);}
 immd::ProgressEvent progress;progress.task_id="task";progress.state=immd::TaskState::Preparing;
 for(int i=0;i<10000;++i){progress.bytes_done=i;receive(state,progress);}
 CHECK(state->slots.size()==1 && slot.pending && slot.event.bytes_done==9999);
 progress.state=immd::TaskState::Ready;progress.media_id="final";receive(state,progress);
 progress.state=immd::TaskState::Transferring;progress.media_id="bad-overwrite";receive(state,progress);
 CHECK(slot.terminal && slot.event.media_id=="final");
 size_t size=0;CHECK(madmedia_poll(999,nullptr,0,&size)==MADMEDIA_BUFFER_TOO_SMALL);
 char tiny[1];CHECK(madmedia_poll(999,tiny,1,&size)==MADMEDIA_BUFFER_TOO_SMALL);
 CHECK(slot.used && slot.pending && slot.terminal);
 std::vector<char> out(size);CHECK(madmedia_poll(999,out.data(),out.size(),&size)==MADMEDIA_OK);
 CHECK(!slot.used && !slot.pending);
 CHECK(std::string(out.data()).find("final")!=std::string::npos);
 slot.used=true;slot.terminal=false;slot.pending=false;progress.media_id=std::string(4097,'x');receive(state,progress);
 CHECK(state->fault);CHECK(madmedia_poll(999,nullptr,0,&size)==MADMEDIA_INTERNAL);
 CHECK(madmedia_destroy(999)==MADMEDIA_OK);
}
