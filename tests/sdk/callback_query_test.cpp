#include <madmedia/madmedia.h>
#include "check.h"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unistd.h>
int main() {
 madmedia::Config c;c.base_url="http://127.0.0.1:1";
 c.data_dir="/tmp/madmedia-reentrant-"+std::to_string(getpid());
 c.connect_timeout_ms=50;c.http_timeout_ms=100;
 std::filesystem::create_directories(c.data_dir);
 std::string file=c.data_dir+"/input";std::ofstream(file)<<"fixture";
 std::mutex mu;std::condition_variable cv;bool completed=false;
 madmedia::Engine* engine=nullptr;
 auto e=madmedia::start(c,[&](const auto& p){
   auto result=engine->query(p.task_id);
   CHECK(!result.ok() && result.error().code==madmedia::Err::State);
   std::lock_guard<std::mutex> lock(mu);completed=true;cv.notify_one();
 });
 CHECK(e.ok());engine=e.value().get();
 madmedia::UploadRequest request;request.file_path=file;
 CHECK(engine->upload(request).ok());
 {std::unique_lock<std::mutex> lock(mu); CHECK(cv.wait_for(lock,std::chrono::seconds(5),[&]{return completed;}));}
 e.value().reset();std::filesystem::remove_all(c.data_dir);
}
