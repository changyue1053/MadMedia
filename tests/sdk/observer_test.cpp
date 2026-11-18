#include <madmedia/madmedia.h>
#include "check.h"
#include <stdexcept>
int main() {
  int user=0, obs=0;
  madmedia::ProgressEvent e;
  e.task_id="secret-id"; e.media_id="secret-media"; e.message="secret-url";
  e.kind=madmedia::TaskKind::Upload; e.state=madmedia::TaskState::Ready;
  auto callback=madmedia::observe([&](const auto&){++user;},[&](const auto& o){
    ++obs; CHECK(o.state==madmedia::TaskState::Ready); throw std::runtime_error("observer");
  });
  callback(e); CHECK(user==1 && obs==1);
  madmedia::observe([&](const auto&){++user;})(e); CHECK(user==2);
  madmedia::observe({},{})(e);
  auto invalid=madmedia::start({}); CHECK(!invalid.ok());
}
