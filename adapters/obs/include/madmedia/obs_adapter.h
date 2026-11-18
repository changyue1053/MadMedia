#pragma once
#include <madmedia/madmedia.h>
#include <madobs/observer.h>
#include <atomic>
#include <chrono>
#include <memory>
namespace madmedia_obs {
struct Counters { std::atomic<uint64_t> invoked{0}, rejected{0}, filtered{0}; };
// sdk_id is explicitly assigned by the host until the shared registry is frozen.
// A callback invocation is not proof of runtime admission or remote delivery.
inline madmedia::ObserverPort make_observer(madobs_observer_v1 observer,
    uint32_t sdk_id, std::shared_ptr<Counters> counters = {}) {
  return [observer, sdk_id, counters](const madmedia::Observation& o) {
    auto kind=static_cast<uint32_t>(o.kind), state=static_cast<uint32_t>(o.state), error=static_cast<uint32_t>(o.error);
    if (sdk_id == 0 || kind > 1 || state > 7 || error > 13) {
      if (counters) ++counters->filtered;
      return;
    }
    madobs_event_v1 event{};
    event.struct_size=sizeof(event); event.abi_version=MADOBS_ABI_VERSION;
    event.sdk_id=sdk_id; event.sdk_version=1; event.event_id=1;
    event.severity=error ? 3 : 1;
    event.monotonic_ns=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
    event.attribute_count=3;
    event.attributes[0]={1,kind}; event.attributes[1]={2,state}; event.attributes[2]={3,error};
    const auto result=madobs_observer_emit(&observer,&event);
    if (counters) { if (result==MADOBS_EMITTED) ++counters->invoked; else ++counters->rejected; }
  };
}
}
