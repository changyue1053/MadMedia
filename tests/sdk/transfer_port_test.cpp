#include <madmedia/madmedia.h>
#include "check.h"
#include "tests/mockproc.h"
#include "core/journal/journal.h"
#include <atomic>
#include <condition_variable>
#include <map>
#include <stdexcept>
using namespace immd;
struct Port:DataPlanePort {
 std::atomic<int> token{1};mutable std::atomic<int> calls{0},puts{0},gets{0};int mode=0;
 Expected<TransferTarget> prepare(const TransferRequest& r)const override{
  ++calls;(r.operation==TransferOperation::UploadPart?puts:gets)++;
  if(mode==1)throw std::runtime_error("Bearer private-transfer-secret");
  return TransferTarget{mode==2?"https://other.invalid/stolen":r.url,{"Authorization: Bearer fixture-"+std::to_string(token.load())}};
 }
};
struct Bad:DataPlanePort{
 TransferTarget target;mutable int calls=0;
 Expected<TransferTarget> prepare(const TransferRequest&)const override{++calls;return target;}
};
void validation(){
 Bad p;p.target={"https://example.invalid/b",{"Authorization: test"}};
 TransferRequest r{TransferOperation::UploadPart,"https://example.invalid/a",0,1};
 CHECK(prepare_transfer(p,{"https://EXAMPLE.invalid:443"},r).ok());
 auto before=p.calls;
 auto zoned=r;zoned.url="http://[fe80::1%25eth1]/data";
 CHECK(!prepare_transfer(p,{"http://[fe80::1%25eth0]"},zoned).ok());CHECK(p.calls==before);
 zoned.url="http://[fe80::1]/data";p.target.url="http://[fe80::1%25eth1]/data";
 CHECK(!prepare_transfer(p,{"http://[fe80::1]"},zoned).ok());CHECK(p.calls==before+1);
 p.target.url="https://example.invalid/b";before=p.calls;
 CHECK(!prepare_transfer(p,{"https://different.invalid"},r).ok());CHECK(p.calls==before);
 for(const auto* url:{"https://other.invalid/","http://example.invalid/","https://user:secret@example.invalid/","https://example.invalid:444/","https://example.invalid/#x","file:///tmp/x"}){p.target.url=url;CHECK(!prepare_transfer(p,{"https://example.invalid"},r).ok());}
 p.target.url=r.url;
 for(const auto* h:{"Range: bytes=0-1","X-Part-CRC32C: forged","Host: other","Content-Length: 0","Transfer-Encoding: chunked","Authorization: x\r\nInjected: true","Connection: keep-alive"}){p.target.headers={h};CHECK(!prepare_transfer(p,{"https://example.invalid"},r).ok());}
 p.target.headers={"Authorization: a","authorization: b"};CHECK(!prepare_transfer(p,{"https://example.invalid"},r).ok());
 CHECK(!valid_transfer_origins({"http://[fe80::1%25eth0]"}));CHECK(!valid_transfer_origins({"http://[fe80::1%25]"}));CHECK(!valid_transfer_origins({"http://[fe80::1%]"}));CHECK(!valid_transfer_origins({"https://%65xample.invalid"}));CHECK(!valid_transfer_origins({"https://example.invalid?"}));CHECK(!valid_transfer_origins({"https://example.invalid#"}));CHECK(!valid_transfer_origins({}));CHECK(!valid_transfer_origins({"https://example.invalid/path"}));CHECK(!valid_transfer_origins({"https://u:p@example.invalid"}));
}
struct Events{
 std::mutex mu;std::condition_variable cv;std::map<std::string,ProgressEvent> done;
 void receive(const ProgressEvent& e){if(e.state==TaskState::Ready||e.state==TaskState::Failed){std::lock_guard<std::mutex> l(mu);done[e.task_id]=e;cv.notify_all();}}
 ProgressEvent wait(const std::string& id){std::unique_lock<std::mutex> l(mu);CHECK(cv.wait_for(l,std::chrono::seconds(10),[&]{return done.count(id); }));return done.at(id);}
};
int main(){
 validation();immd_test::MockCtl server,other;HttpClient client;CHECK(server.wait_ready(client));CHECK(other.wait_ready(client));
 server.set_faults(client,Json::object().set("challenge_on_dedup",Json::boolean(false)).set("require_data_authorization",Json::str("Bearer fixture-1")));
 auto port=std::make_shared<Port>();std::weak_ptr<Port> lifetime=port;
 Config cfg;cfg.base_url=server.base();cfg.data_dir=immd_test::make_temp_dir();cfg.force_part_size=128*1024;
 cfg.data_plane=port;CHECK(!Engine::start(cfg,{}).ok());cfg.data_plane_allowed_origins={server.base()};
 auto file=cfg.data_dir+"/input";immd_test::write_random_file(file,800*1024,37);Events events;
 auto engine=Engine::start(cfg,[&](const auto& e){events.receive(e);});CHECK(engine.ok());cfg.data_plane.reset();
 auto up=engine.value()->upload({file,"application/octet-stream",Priority::P4_File,""});CHECK(up.ok());auto uploaded=events.wait(up.value());CHECK(uploaded.state==TaskState::Ready);CHECK(port->puts>=7);
 // Host refreshes outside scheduler, with an atomic snapshot consumed per request.
 server.set_faults(client,Json::object().set("require_data_authorization",Json::str("Bearer fixture-2")));port->token=2;
 auto destination=cfg.data_dir+"/download";auto dl=engine.value()->download({uploaded.media_id,destination,Priority::P4_File,""});CHECK(dl.ok());CHECK(events.wait(dl.value()).state==TaskState::Ready);CHECK(port->gets>=7);CHECK(immd_test::read_file(file)==immd_test::read_file(destination));
 port.reset();CHECK(!lifetime.expired());engine.value().reset();CHECK(lifetime.expired());
 // Same transport refuses redirects, so credential headers never reach other origin.
 server.set_faults(client,Json::object().set("test_redirect_to",Json::str(other.base()+"/test/sink")));
 HttpRequest redirect;redirect.url=server.base()+"/test/redirect";redirect.headers={"Authorization: Bearer fixture-2"};CHECK(immd_test::http_sync(client,redirect).status==302);
 CHECK(other.stats(client).find("stats")->get_i64("test_sink_hits")==0);
 for(int mode=1;mode<=2;++mode){
  server.reset(client);auto bad=std::make_shared<Port>();bad->mode=mode;Config c=cfg;c.data_dir=immd_test::make_temp_dir();c.data_plane=bad;Events failed;
  auto e=Engine::start(c,[&](const auto& x){failed.receive(x);});CHECK(e.ok());auto id=e.value()->upload({file,"application/octet-stream",Priority::P4_File,""});CHECK(id.ok());auto terminal=failed.wait(id.value());CHECK(terminal.state==TaskState::Failed);CHECK(terminal.error==Err::Protocol);CHECK(terminal.message=="Protocol: data operation failed");e.value().reset();
  auto journal=Journal::open(c.data_dir+"/journal.db");CHECK(journal.ok());auto rows=journal.value()->load_all();CHECK(rows.ok());CHECK(rows.value().at(0).last_error=="Protocol: data operation failed");CHECK(server.stats(client).find("stats")->get_i64("part_put_total")==0);
 }
}
