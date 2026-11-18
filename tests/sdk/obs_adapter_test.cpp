#include <madmedia/obs_adapter.h>
#include "check.h"
int main() {
  unsigned calls=0;
  madobs_observer_v1 sink=madobs_noop_observer(); sink.ctx=&calls;
  sink.emit=[](void* ctx,const madobs_event_v1* e){
    ++*static_cast<unsigned*>(ctx); CHECK(e->sdk_id==5 && e->attribute_count==3);
    CHECK(e->attributes[0].enum_value==0 && e->attributes[1].enum_value==4);
  };
  auto counts=std::make_shared<madmedia_obs::Counters>();
  auto port=madmedia_obs::make_observer(sink,5,counts);
  port({madmedia::TaskKind::Upload,madmedia::TaskState::Ready,madmedia::Err::Ok});
  CHECK(calls==1 && counts->invoked==1);
  port({static_cast<madmedia::TaskKind>(99),madmedia::TaskState::Ready,madmedia::Err::Ok});
  CHECK(calls==1 && counts->filtered==1);
  auto noop=madmedia_obs::make_observer(madobs_noop_observer(),5,counts);
  noop({madmedia::TaskKind::Upload,madmedia::TaskState::Ready,madmedia::Err::Ok});
  CHECK(counts->rejected==1);
}
