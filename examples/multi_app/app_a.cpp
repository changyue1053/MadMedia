#include <madmedia/madmedia.h>
#ifdef WITH_OBS
#include <madmedia/obs_adapter.h>
#endif
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
int main(int argc,char** argv){
 if(argc!=3&&argc!=4)return 2;
 madmedia::Config cfg;cfg.base_url="http://127.0.0.1:1";cfg.data_dir=argv[2];cfg.max_active_tasks=0;
 std::atomic<unsigned> events{0};madmedia::ObserverPort port;
#ifdef WITH_OBS
 auto sink=madobs_noop_observer();sink.ctx=&events;
 sink.emit=[](void* ctx,const madobs_event_v1* e){if(e->sdk_id==5&&e->attribute_count==3)++*static_cast<std::atomic<unsigned>*>(ctx);};
 port=madmedia_obs::make_observer(sink,5);
#endif
 auto engine=madmedia::start(cfg,{},port);if(!engine.ok())return 3;
 if(std::string(argv[1])=="foreign"&&argc==4){auto row=engine.value()->query_persisted(argv[3]);return !row.ok()&&row.error().code==madmedia::Err::NotFound?0:4;}
 if(std::string(argv[1])!="create"||argc!=3)return 5;
 madmedia::DownloadRequest request;request.media_id="fixture";request.dest_path=cfg.data_dir+"/output";
 auto id=engine.value()->download(request);if(!id.ok())return 6;
 if(engine.value()->cancel(id.value()))return 7;
 auto row=engine.value()->query_persisted(id.value());if(!row.ok()||row.value().state!=madmedia::TaskState::Canceled)return 8;
 if(engine.value()->callback_health().faulted_invocations)return 9;
 engine.value().reset();
#ifdef WITH_OBS
 if(events.load()==0)return 10;
#endif
 std::cout<<id.value()<<'\n';
}
