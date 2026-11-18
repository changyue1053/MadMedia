#include <madmedia/madmedia.h>
#include "check.h"
#include "tests/mockproc.h"
#include "tests/httputil.h"
#include "core/crypto/sha256.h"
#include <condition_variable>
#include <map>
#include <sys/wait.h>
using namespace immd;using namespace immd_test;
struct Observer{
 std::mutex mu;std::condition_variable cv;std::map<std::string,ProgressEvent> last;
 ProgressCallback callback(){return [this](const auto& e){std::lock_guard<std::mutex> l(mu);last[e.task_id]=e;cv.notify_all();};}
 ProgressEvent wait(const std::string& id,TaskState state){std::unique_lock<std::mutex> l(mu);CHECK(cv.wait_for(l,std::chrono::seconds(12),[&]{return last.count(id)&&(last[id].state==state||(state==TaskState::Ready&&last[id].state==TaskState::Failed));}));if(last[id].state!=state)std::fprintf(stderr,"stream failed: %s\n",last[id].message.c_str());CHECK(last[id].state==state);return last[id];}
};
static Config config(const std::string& base,const std::string& dir){Config c;c.base_url=base;c.data_dir=dir;c.force_part_size=16*1024;c.retry_base_ms=20;c.retry_cap_ms=100;c.part_retry_limit=5;return c;}
static void await_parts(Engine& e,const std::string& id){for(int i=0;i<5000;++i){auto r=e.query(id);CHECK(r.ok());if(r.value().nparts>=2)return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}CHECK(false);}
int main(int argc,char** argv){
 if(argc==4){auto c=config(argv[1],argv[2]);auto e=Engine::start(c,{});CHECK(e.ok());auto id=e.value()->stream_begin({});CHECK(id.ok());std::string bytes(32*1024,'r');CHECK(!e.value()->stream_append(id.value(),bytes.data(),bytes.size()));await_parts(*e.value(),id.value());CHECK(write(std::stoi(argv[3]),id.value().c_str(),id.value().size()+1)==ssize_t(id.value().size()+1));for(;;)pause();}
 MockCtl mock;HttpClient http;CHECK(mock.wait_ready(http));
 for(int drop=0;drop<2;++drop){
  mock.reset(http);auto dir=make_temp_dir();Observer ob;auto e=Engine::start(config(mock.base(),dir),ob.callback());CHECK(e.ok());auto id=e.value()->stream_begin({});CHECK(id.ok());
  std::string first(32*1024,'a'),second(24*1024,'b');for(size_t i=0;i<first.size();++i)first[i]=char((i*17+3)%251);for(size_t i=0;i<second.size();++i)second[i]=char((i*31+7)%251);std::string all=first+second;
  CHECK(!e.value()->stream_append(id.value(),first.data(),first.size()));await_parts(*e.value(),id.value());CHECK(!e.value()->pause(id.value()));ob.wait(id.value(),TaskState::Paused);
  CHECK(!e.value()->stream_append(id.value(),second.data(),second.size()));CHECK(!e.value()->stream_finish(id.value()));
  if(drop)mock.set_faults(http,Json::object().set("drop_connections",Json::number(1)));
  CHECK(!e.value()->resume(id.value()));auto ready=ob.wait(id.value(),TaskState::Ready);CHECK(!ready.media_id.empty());
  auto signed_url=post_json(http,mock.base()+"/v1/media/dl_url",Json::object().set("media_id",Json::str(ready.media_id)));CHECK(signed_url.status==200);auto parsed=Json::parse(signed_url.body);CHECK(parsed.ok());HttpRequest download;download.url=parsed.value().get_str("url");auto response=http_sync(http,download);CHECK(response.status==200&&response.body==all);e.value().reset();
 }
 // Actual process death: streaming state can be inspected/canceled, but cannot
 // resume after lost recording buffers. Do not silently upload truncated data.
 mock.reset(http);auto dir=make_temp_dir();int pipefd[2];CHECK(pipe(pipefd)==0);auto base=mock.base();auto fd=std::to_string(pipefd[1]);pid_t child=fork();CHECK(child>=0);
 if(child==0){close(pipefd[0]);execl(argv[0],argv[0],base.c_str(),dir.c_str(),fd.c_str(),nullptr);_exit(127);}
 close(pipefd[1]);char received[128];auto bytes_read=read(pipefd[0],received,sizeof(received));CHECK(bytes_read>0&&received[bytes_read-1]==0);std::string recovered_id=received;close(pipefd[0]);CHECK(kill(child,SIGKILL)==0);int status;CHECK(waitpid(child,&status,0)==child&&WIFSIGNALED(status));
 Observer ob;auto e=Engine::start(config(mock.base(),dir),ob.callback());CHECK(e.ok());CHECK(!e.value()->recover());ob.wait(recovered_id,TaskState::Failed);CHECK(!e.value()->resume(recovered_id));auto record=e.value()->query(recovered_id);CHECK(record.ok()&&record.value().state==TaskState::Failed);CHECK(record.value().last_error=="State: streaming task cannot resume after restart");CHECK(!e.value()->cancel(recovered_id));ob.wait(recovered_id,TaskState::Canceled);
}
