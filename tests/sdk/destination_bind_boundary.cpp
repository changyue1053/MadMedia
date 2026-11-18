// Optional NEGATIVE boundary probe, not an automated protection qualification.
// Host must never supply this input. Build with -std=c++17 -I<repo>, then:
// mkdir -p "$D/data/cache/sub" "$D/data/staging" "$D/alias"
// bwrap --bind / / --bind "$D/data/cache/sub" "$D/alias" ./probe "$D/data" "$D/alias/file"
// Current output: accepted=1. The mount hides the cache ancestor from traversal.
// Future hardening may reject it; do not turn this into a permanent acceptance test.
#include "core/engine/destination_registry.h"
#include <cstdio>
int main(int argc,char** argv){
 if(argc!=3)return 2;
 immd::DestinationRegistry registry;
 if(registry.initialize(argv[1]))return 2;
 auto result=registry.reserve("boundary",argv[2]);
 std::printf("unsupported private-descendant bind alias accepted=%d\n",result.ok());
 return 0;
}
