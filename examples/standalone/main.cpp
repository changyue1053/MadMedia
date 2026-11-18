#include <madmedia/madmedia.h>
#include <iostream>
int main() {
  // Exercise an out-of-line native symbol without requiring a service or disk.
  auto invalid = madmedia::start({});
  if (invalid.ok() || invalid.error().code != madmedia::Err::State) return 1;
  std::cout << "MadMedia " << madmedia::version() << " native link verified\n";
}
