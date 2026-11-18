#include <madmedia/madmedia.h>
#include "check.h"
#include "tests/testutil.h"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <thread>
using namespace immd;
// Deliberately stalls scheduler ONLY to inject a full command queue. Real ports
// must not block; fixture always releases before Engine destruction.
struct Gate:ControlPlanePort{
 mutable std::mutex mutex;mutable std::condition_variable cv;mutable bool entered=false;bool released=false;
 Expected<ControlRequest> encode(ControlOperation,const ControlRequest&)const override{
  std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_all();cv.wait(lock,[&]{return released;});return make_err(Err::Protocol,"fixture");
 }
 Expected<ControlResponse> decode(ControlOperation,const ControlResponse& r)const override{return r;}
};
int main(){
 auto gate=std::make_shared<Gate>();Config cfg;cfg.base_url="http://127.0.0.1:1";cfg.data_dir=immd_test::make_temp_dir();cfg.control_plane=gate;
 auto file=cfg.data_dir+"/file";std::ofstream(file)<<"fixture";
 auto engine=Engine::start(cfg,{});CHECK(engine.ok());CHECK(engine.value()->upload({file,"application/octet-stream",Priority::P4_File,"seed_id"}).ok());
 {std::unique_lock<std::mutex> lock(gate->mutex);CHECK(gate->cv.wait_for(lock,std::chrono::seconds(5),[&]{return gate->entered;}));}
 int accepted=0;for(int n=0;n<5000;++n){if(engine.value()->set_rate_limit(0))break;++accepted;}CHECK(accepted>0&&accepted<5000);
 auto rejected=engine.value()->upload({file,"application/octet-stream",Priority::P4_File,"retry_id"});CHECK(!rejected.ok());CHECK(rejected.error().code==Err::Busy);
 {std::lock_guard<std::mutex> lock(gate->mutex);gate->released=true;}gate->cv.notify_all();
 bool drained=false;for(int n=0;n<500;++n){auto result=engine.value()->query("absent");if(!result.ok()&&result.error().code==Err::NotFound){drained=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}CHECK(drained);
 auto retry=engine.value()->upload({file,"application/octet-stream",Priority::P4_File,"retry_id"});CHECK(retry.ok());CHECK(engine.value()->query(retry.value()).ok());engine.value().reset();
}
