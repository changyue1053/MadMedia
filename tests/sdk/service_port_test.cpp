#include <madmedia/madmedia.h>
#include "check.h"
#include "tests/mockproc.h"
#include "core/util/json.h"
#include "core/journal/journal.h"
#include <condition_variable>
#include <stdexcept>
using namespace immd;
struct Port final:ControlPlanePort {
 std::string base;int mode;mutable int encoded=0,decoded=0;
 Port(std::string b,int m):base(std::move(b)),mode(m){}
 Expected<ControlRequest> encode(ControlOperation op,const ControlRequest& r)const override{
  ++encoded;CHECK(op==ControlOperation::Init);
  if(mode==2)throw std::runtime_error("Bearer private-do-not-log");
  auto out=r;out.url=base+(mode==0?"/json/objects":mode==1?"/text/store":"/bad");
  out.headers={"Authorization: Bearer test-only-fixture"};
  out.body=mode==0?"{\"operation\":\"store\"}":"store:"+r.body;
  if(mode==4)out.headers.push_back("X-Test: ok\r\nAuthorization: injected");
  if(mode==5)out.body.resize(1024*1024+1);
  return out;
 }
 Expected<ControlResponse> decode(ControlOperation,const ControlResponse& r)const override{
  ++decoded;
  if(mode==6)throw std::runtime_error("Bearer private-do-not-log");
  if(mode==8)return ControlResponse{200,"{\"result\":\"Bearer private-do-not-log\"}"};
  if(mode==7)return ControlResponse{200,std::string(1024*1024+1,'x')};
  if(mode>=3)return r; // malformed native schema must be rejected by core parser
  std::string id;
  if(mode==0){CHECK(r.status==201);auto parsed=Json::parse(r.body);if(!parsed.ok())return parsed.error();id=parsed.value().get_str("object");}
  else {CHECK(r.status==202);if(r.body.rfind("STORED ",0)!=0)return make_err(Err::Protocol,"invalid");id=r.body.substr(7);}
  return ControlResponse{200,Json::object().set("result",Json::str("instant")).set("media_id",Json::str(id)).dump()};
 }
};
struct Identity final:ControlPlanePort {
 mutable int reads=0;
 Expected<ControlRequest> encode(ControlOperation,const ControlRequest& r)const override{return r;}
 Expected<ControlResponse> decode(ControlOperation,const ControlResponse& r)const override{++reads;return r;}
};
void validation(){
 Identity p;ControlRequest r{"POST","https://example.invalid/control","{}",{"Authorization: fixture"}};
 for(auto op:{ControlOperation::Init,ControlOperation::PartUrls,ControlOperation::Complete,ControlOperation::Challenge,ControlOperation::DownloadUrl,ControlOperation::MediaStatus})CHECK(encode_control(p,op,r).ok());
 auto bad=r;bad.method="DELETE";CHECK(!encode_control(p,ControlOperation::Init,bad).ok());
 for(const auto* url:{"file:///tmp/input","https://user:password@example.invalid/","https://example.invalid/a b","https://example.invalid/#fragment"}){bad=r;bad.url=url;CHECK(!encode_control(p,ControlOperation::Init,bad).ok());}
 for(const auto* header:{"Host: other.invalid","Content-Length: 1","Transfer-Encoding: chunked","X: yes\r\nInjected: true",": empty"}){bad=r;bad.headers={header};CHECK(!encode_control(p,ControlOperation::Init,bad).ok());}
 bad=r;bad.method="GET";CHECK(!encode_control(p,ControlOperation::Init,bad).ok());
 bad=r;bad.headers.resize(65,"X: yes");CHECK(!encode_control(p,ControlOperation::Init,bad).ok());
 CHECK(!decode_control(p,ControlOperation::Init,{700,"{}"}).ok());
 int before=p.reads;CHECK(!decode_control(p,ControlOperation::Init,{200,std::string(16*1024*1024+1,'x')}).ok());CHECK(p.reads==before);
}
struct BadStage final:ControlPlanePort {
 std::string base;ControlOperation target;bool missing;
 BadStage(std::string b,ControlOperation t,bool m=false):base(std::move(b)),target(t),missing(m){}
 Expected<ControlRequest> encode(ControlOperation,const ControlRequest&)const override{return ControlRequest{"POST",base+"/bad","{}",{"Authorization: Bearer test-only-fixture"}};}
 Expected<ControlResponse> decode(ControlOperation op,const ControlResponse&)const override{
  if(op==target)return ControlResponse{missing?409:200,op==ControlOperation::MediaStatus?"invalid-private-do-not-log":"{\"result\":\"Bearer private-do-not-log\"}"};
  if(op==ControlOperation::Init){
   if(target==ControlOperation::Challenge)return ControlResponse{200,R"({"result":"challenge","challenge_id":"x","nonce":"x","ranges":[[0,1]]})"};
   std::string received=target==ControlOperation::PartUrls?"[]":"[0]";
   return ControlResponse{200,"{\"result\":\"upload\",\"upload_id\":\"x\",\"part_size\":1024,\"nparts\":1,\"received_parts\":"+received+"}"};
  }
  return ControlResponse{200,R"({"media_id":"x"})"};
 }
};
void parse_failures(const std::string& base){
 for(auto op:{ControlOperation::Init,ControlOperation::PartUrls,ControlOperation::Complete,ControlOperation::Challenge,ControlOperation::DownloadUrl,ControlOperation::MediaStatus}){
  for(int missing=0;missing<(op==ControlOperation::Complete?2:1);++missing){
   Config cfg;cfg.base_url=base;cfg.data_dir=immd_test::make_temp_dir();cfg.ready_on_uploaded=false;
   cfg.control_plane=std::make_shared<BadStage>(base,op,missing);
   std::string file=cfg.data_dir+"/input";immd_test::write_random_file(file,1024,9);
   std::mutex mutex;std::condition_variable cv;bool done=false;ProgressEvent event;
   auto engine=Engine::start(cfg,[&](const ProgressEvent& e){if(e.state==TaskState::Failed){std::lock_guard<std::mutex> lock(mutex);event=e;done=true;cv.notify_all();}});CHECK(engine.ok());
   if(op==ControlOperation::DownloadUrl)CHECK(engine.value()->download({"x",cfg.data_dir+"/output",Priority::P4_File,""}).ok());
   else CHECK(engine.value()->upload({file,"application/octet-stream",Priority::P4_File,""}).ok());
   {std::unique_lock<std::mutex> lock(mutex);CHECK(cv.wait_for(lock,std::chrono::seconds(5),[&]{return done;}));}
   CHECK(event.error==Err::Protocol);CHECK(event.message=="Protocol: control protocol rejected");engine.value().reset();
   auto journal=Journal::open(cfg.data_dir+"/journal.db");CHECK(journal.ok());auto rows=journal.value()->load_all();CHECK(rows.ok());CHECK(rows.value().size()==1);
   CHECK(rows.value()[0].last_error=="Protocol: control protocol rejected");
  }
 }
}
int main(){
 validation();
 immd_test::MockCtl server;HttpClient client;CHECK(server.wait_ready(client));
 parse_failures(server.base());
 for(int mode=0;mode<9;++mode){
  auto port=std::make_shared<Port>(server.base(),mode);std::weak_ptr<Port> lifetime=port;
  Config cfg;cfg.base_url="http://invalid.test";cfg.data_dir=immd_test::make_temp_dir();cfg.control_plane=port;
  std::string file=cfg.data_dir+"/input";immd_test::write_random_file(file,1024,1);
  std::mutex mutex;std::condition_variable cv;bool done=false;ProgressEvent terminal;
  auto engine=Engine::start(cfg,[&](const ProgressEvent& e){if(e.state==TaskState::Ready||e.state==TaskState::Failed){std::lock_guard<std::mutex> lock(mutex);terminal=e;done=true;cv.notify_all();}});
  CHECK(engine.ok());cfg.control_plane.reset();CHECK(engine.value()->upload({file,"application/octet-stream",Priority::P4_File,""}).ok());
  {std::unique_lock<std::mutex> lock(mutex);CHECK(cv.wait_for(lock,std::chrono::seconds(5),[&]{return done;}));}
  CHECK(terminal.state==(mode<2?TaskState::Ready:TaskState::Failed));
  if(mode<2)CHECK(terminal.media_id==(mode==0?"json-object":"text-object"));
  else {CHECK(terminal.error==Err::Protocol);CHECK(terminal.message.find("private-do-not-log")==std::string::npos);}
  CHECK(port->encoded==1);port.reset();CHECK(!lifetime.expired());engine.value().reset();CHECK(lifetime.expired());
  auto journal=Journal::open(cfg.data_dir+"/journal.db");CHECK(journal.ok());
  auto rows=journal.value()->load_all();CHECK(rows.ok());CHECK(rows.value().size()==1);
  CHECK(rows.value()[0].last_error.find("private-do-not-log")==std::string::npos);
 }
}
