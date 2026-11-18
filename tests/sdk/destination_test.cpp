#include <functional>
#include <memory>
#include <string>
#include "core/journal/journal.h"
#define private public
#include "core/engine/engine.h"
#undef private
#include "../../core/engine/engine.cpp"
#include "check.h"
#include "tests/testutil.h"
#include <future>
#ifdef MADMEDIA_DESTINATION_C_TEST
#include <madmedia/madmedia_c.h>
#endif
#include <sqlite3.h>
#include <cstring>
static int scan_fault=0,scan_fault_calls=0;
extern "C" int __real_sqlite3_step(sqlite3_stmt*);
extern "C" int __wrap_sqlite3_step(sqlite3_stmt* st){if(scan_fault&&std::strstr(sqlite3_sql(st),"WHERE state NOT IN")){++scan_fault_calls;return scan_fault;}return __real_sqlite3_step(st);}
using namespace immd;using namespace immd_test;
static Config config(const std::string& dir){Config c;c.base_url="http://127.0.0.1:1";c.data_dir=dir;c.max_active_tasks=0;return c;}
static void touch(const std::string& p){std::ofstream f(p);f<<"keep";CHECK(bool(f));}
static TaskRecord row(const std::string& id,const std::string& dest){TaskRecord r;r.task_id=id;r.kind=TaskKind::Download;r.state=TaskState::Paused;r.media_id="test";r.dest_path=dest;r.staging_path=dest+".part";return r;}
int main(){
 auto root=make_temp_dir();CHECK(!mkdirs(root+"/data/staging"));CHECK(!mkdirs(root+"/data/cache"));CHECK(!mkdirs(root+"/out"));touch(root+"/data/journal.db");
 DestinationRegistry reg;CHECK(!reg.initialize(root+"/data"));
 auto dest=root+"/out/x";CHECK(reg.reserve("one",dest).ok());
 CHECK(!reg.reserve("two",dest).ok());CHECK(!reg.reserve("two",dest+".part").ok());CHECK(!reg.reserve("two",dest+".cachetmp").ok());
 CHECK(::symlink((root+"/out").c_str(),(root+"/alias").c_str())==0);CHECK(!reg.reserve("two",root+"/alias/x").ok());CHECK(!reg.reserve("two",root+"/out/../out/x").ok());
 for(auto name:{"journal.db","journal.db-wal","journal.db-shm","journal.db-journal",".madmedia.lock","staging/file","cache/file"})CHECK(!reg.reserve("bad",root+"/data/"+name).ok());
 CHECK(reg.reserve("ordinary",root+"/data/output").ok());
 CHECK(::symlink((root+"/data/staging").c_str(),(root+"/private_alias").c_str())==0);CHECK(!reg.reserve("bad",root+"/private_alias/file").ok());
 for(auto suffix:{"",".part",".cachetmp"}){auto target=root+"/out/link"+suffix;CHECK(::symlink(dest.c_str(),target.c_str())==0);CHECK(!reg.reserve("bad",root+"/out/link").ok());CHECK(::unlink(target.c_str())==0);}
 touch(root+"/out/hard");CHECK(::link((root+"/out/hard").c_str(),(root+"/out/hard2").c_str())==0);CHECK(!reg.reserve("bad",root+"/out/hard").ok());
 CHECK(!reg.reserve("bad",root+"/missing/f").ok());CHECK(!reg.reserve("bad",root+"/out/.").ok());CHECK(!reg.reserve("bad",std::string(dest)+std::string("\0tail",5)).ok());
 reg.rollback("one");CHECK(reg.reserve("replacement",dest).ok());
 // Concurrent admission chooses exactly one owner, including a suffix collision.
 std::atomic<int> accepted{0};std::vector<std::thread> threads;for(int i=0;i<16;++i)threads.emplace_back([&,i]{if(reg.reserve("c"+std::to_string(i),root+"/out/shared").ok())++accepted;});for(auto& t:threads)t.join();CHECK(accepted==1);
 DestinationRegistry bounded;CHECK(!bounded.initialize(root+"/data"));for(size_t i=0;i<DestinationRegistry::capacity;++i)CHECK(bounded.reserve(std::to_string(i),root+"/out/b"+std::to_string(i)).ok());auto full=bounded.reserve("excess",root+"/out/excess");CHECK(!full.ok()&&full.error().code==Err::Busy);bounded.rollback("0");CHECK(bounded.reserve("available",root+"/out/excess").ok());
 // Public admission persists absolute paths and owns destinations past Cancel.
 auto data=make_temp_dir();auto e=Engine::start(config(data),{});CHECK(e.ok());DownloadRequest request;request.media_id="test";request.dest_path=root+"/out/public";request.task_id="public";CHECK(e.value()->download(request).ok());auto rec=e.value()->query("public");CHECK(rec.ok()&&rec.value().dest_path==request.dest_path);CHECK(!e.value()->cancel("public"));CHECK(e.value()->query("public").ok());request.task_id="public2";CHECK(!e.value()->download(request).ok());
 // Queue refusal rolls back path and ID atomically without retaining capacity.
 e.value()->impl_->q.close();e.value()->impl_->sched.join();request.task_id="rejected";request.dest_path=root+"/out/queue";CHECK(!e.value()->download(request).ok());CHECK(!e.value()->impl_->destinations.find("rejected").ok());CHECK(!e.value()->impl_->reserved_ids.count("rejected"));e.value().reset();
 // Unfinished rows reserve before recover; aliases normalize for actual recovery.
 auto saved=make_temp_dir();{auto j=Journal::open(saved+"/journal.db");CHECK(j.ok());CHECK(!j.value()->save(row("old",root+"/alias/recovered")));}
 auto recovered=Engine::start(config(saved),{});CHECK(recovered.ok());request.task_id="new";request.dest_path=root+"/out/recovered";CHECK(!recovered.value()->download(request).ok());CHECK(!recovered.value()->recover());auto old=recovered.value()->query("old");CHECK(old.ok()&&old.value().dest_path==request.dest_path&&old.value().staging_path==request.dest_path+".part");CHECK(!recovered.value()->recover());CHECK(recovered.value()->query("old").ok());recovered.value().reset();
 for(int mode=0;mode<3;++mode){auto d=make_temp_dir();{auto j=Journal::open(d+"/journal.db");CHECK(j.ok());auto r=row("a",root+"/out/legacy");if(mode==0){r.dest_path="relative";r.staging_path="relative.part";}if(mode==1)r.staging_path=root+"/out/unrelated";CHECK(!j.value()->save(r));if(mode==2){r.task_id="b";CHECK(!j.value()->save(r));}}CHECK(!Engine::start(config(d),{}).ok());}
 // Startup cannot accept a partial inventory, and fails on excess persisted owners.
 for(int code:{SQLITE_IOERR,SQLITE_BUSY}){auto d=make_temp_dir();scan_fault=code;scan_fault_calls=0;auto failed=Engine::start(config(d),{});scan_fault=0;CHECK(!failed.ok()&&failed.error().code==Err::Io&&scan_fault_calls==1);}
 auto oversized=make_temp_dir();{auto j=Journal::open(oversized+"/journal.db");CHECK(j.ok());for(size_t i=0;i<=DestinationRegistry::capacity;++i)CHECK(!j.value()->save(row("r"+std::to_string(i),root+"/out/persisted"+std::to_string(i))));}auto too_many=Engine::start(config(oversized),{});CHECK(!too_many.ok()&&too_many.error().code==Err::Busy);
 // Relative admission is resolved once, not reinterpreted after a cwd change.
 auto cwd=std::filesystem::current_path();auto relative_engine=Engine::start(config(make_temp_dir()),{});CHECK(relative_engine.ok());std::filesystem::current_path(root+"/out");request.task_id="relative";request.dest_path="relative.bin";CHECK(relative_engine.value()->download(request).ok());std::filesystem::current_path(cwd);auto relative=relative_engine.value()->query("relative");CHECK(relative.ok()&&relative.value().dest_path==root+"/out/relative.bin");

 #ifdef MADMEDIA_DESTINATION_C_TEST
 // C keeps its native-error mapping and rolls back the rejected event slot.
 auto cdir=make_temp_dir();madmedia_config_v1 cc{sizeof(cc),1,"http://127.0.0.1:1",cdir.c_str(),2,0};madmedia_handle handle=0;CHECK(madmedia_create(&cc,&handle)==MADMEDIA_OK);char idbuf[128];auto cpath=root+"/out/c-output";CHECK(madmedia_download(handle,"media",cpath.c_str(),idbuf,sizeof(idbuf))==MADMEDIA_OK);std::strcpy(idbuf,"unchanged");CHECK(madmedia_download(handle,"media",cpath.c_str(),idbuf,sizeof(idbuf))==MADMEDIA_NATIVE_ERROR);CHECK(idbuf[0]==0);auto another=root+"/out/c-another";CHECK(madmedia_download(handle,"media",another.c_str(),idbuf,sizeof(idbuf))==MADMEDIA_OK);CHECK(madmedia_destroy(handle)==MADMEDIA_OK);

 #endif
}
